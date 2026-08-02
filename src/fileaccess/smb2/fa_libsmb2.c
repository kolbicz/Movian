/*
 *  Copyright (C) 2026 Movian contributors
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

/*
 * SMB2 fileaccess VFS backend.
 *
 * Implements the fa_protocol callbacks (scan/open/read/write/.../stat/...) on
 * top of a pooled libsmb2 session provided by fa_libsmb2_pool.c. libsmb2 is
 * not thread safe and a context is bound to one socket, so each VFS op
 * describes its work in an op blob (movian_smb2_op_t first member) and runs
 * it through the session's I/O owner thread via movian_smb2_op_run(); only
 * that thread ever touches the smb2_context. Several VFS threads can have
 * ops in flight on the same session at once, and each waiter has its own
 * deadline (fa_set_read_timeout(0) = wait forever, for that caller only).
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/libsmb2-dcerpc-srvsvc.h>

#include "main.h"
#include "fileaccess/fa_proto.h"
#include "misc/rstr.h"

#include "fa_libsmb2_pool.h"

typedef struct {
  fa_handle_t h;
  movian_smb2_session_t *session;
  struct smb2fh *fh;
  int64_t size;
  int read_timeout_sec;
  /*
   * Client-side file cursor: reads/writes go out as explicit-offset
   * pread/pwrite ops and fa_seek is pure arithmetic. fh_lock serializes
   * operations on this one handle (so two calls cannot interleave their
   * offsets) without serializing other handles or ops on the session.
   */
  int64_t fpos;
  hts_mutex_t fh_lock;
} movian_smb2_file_t;


/*
 * Op blobs for the file-handle operations, submitted to the session's I/O
 * owner thread via movian_smb2_op_run() (see fa_libsmb2_pool.h). The
 * movian_smb2_op_t must stay the first member so a timed-out blob can be
 * parked on the session's deferred-free list as one allocation.
 */

typedef struct {
  movian_smb2_op_t op;
  struct smb2fh *fh;            /* result, set by the completion callback */
  int open_flags;
  char path[];
} movian_smb2_open_op_t;

typedef struct {
  movian_smb2_op_t op;
  struct smb2fh *fh;
  struct smb2_stat_64 st;
} movian_smb2_fstat_op_t;

typedef struct {
  movian_smb2_op_t op;
  struct smb2fh *fh;
  uint8_t *rbuf;
  const uint8_t *wbuf;
  uint32_t count;
  uint64_t offset;
} movian_smb2_rw_op_t;

typedef struct {
  movian_smb2_op_t op;
  struct smb2fh *fh;
  uint64_t length;              /* ftruncate only */
} movian_smb2_fh_op_t;


static void
movian_smb2_open_op_cb(struct smb2_context *smb2, int status,
                       void *command_data, void *cb_data)
{
  movian_smb2_op_t *op = cb_data;
  ((movian_smb2_open_op_t *)op)->fh = command_data;
  movian_smb2_op_completed(op, status);
}


static int
movian_smb2_open_submit(movian_smb2_session_t *session, movian_smb2_op_t *op)
{
  movian_smb2_open_op_t *o = (movian_smb2_open_op_t *)op;
  return smb2_open_async(session->smb2, o->path, o->open_flags,
                         movian_smb2_open_op_cb, op);
}


static int
movian_smb2_fstat_submit(movian_smb2_session_t *session, movian_smb2_op_t *op)
{
  movian_smb2_fstat_op_t *o = (movian_smb2_fstat_op_t *)op;
  return smb2_fstat_async(session->smb2, o->fh, &o->st,
                          movian_smb2_op_cb, op);
}


static int
movian_smb2_pread_submit(movian_smb2_session_t *session, movian_smb2_op_t *op)
{
  movian_smb2_rw_op_t *o = (movian_smb2_rw_op_t *)op;
  return smb2_pread_async(session->smb2, o->fh, o->rbuf, o->count, o->offset,
                          movian_smb2_op_cb, op);
}


static int
movian_smb2_pwrite_submit(movian_smb2_session_t *session, movian_smb2_op_t *op)
{
  movian_smb2_rw_op_t *o = (movian_smb2_rw_op_t *)op;
  return smb2_pwrite_async(session->smb2, o->fh, o->wbuf, o->count, o->offset,
                           movian_smb2_op_cb, op);
}


static int
movian_smb2_close_submit(movian_smb2_session_t *session, movian_smb2_op_t *op)
{
  movian_smb2_fh_op_t *o = (movian_smb2_fh_op_t *)op;
  return smb2_close_async(session->smb2, o->fh, movian_smb2_op_cb, op);
}


static int
movian_smb2_ftruncate_submit(movian_smb2_session_t *session,
                             movian_smb2_op_t *op)
{
  movian_smb2_fh_op_t *o = (movian_smb2_fh_op_t *)op;
  return smb2_ftruncate_async(session->smb2, o->fh, o->length,
                              movian_smb2_op_cb, op);
}


/*
 * Close a server-side file handle through the owner thread, best effort
 * (used both by fap_close and by open()'s failure paths).
 */
static void
movian_smb2_close_fh(movian_smb2_session_t *session, struct smb2fh *fh)
{
  movian_smb2_fh_op_t *o = calloc(1, sizeof(*o));
  if(o == NULL)
    return;                     /* leak the handle; teardown reaps it */
  o->fh = fh;
  movian_smb2_op_run(session, &o->op, "close", movian_smb2_close_submit,
                     SMB2_DEFAULT_TIMEOUT, NULL, 0);
  if(!o->op.parked)
    free(o);
}


/*
 * Path-keyed op blobs (stat/statvfs/unlink/rmdir/mkdir/rename/scandir).
 */

typedef struct {
  movian_smb2_op_t op;
  char path[];
} movian_smb2_path_op_t;

typedef struct {
  movian_smb2_op_t op;
  struct smb2_stat_64 st;
  char path[];
} movian_smb2_stat_op_t;

typedef struct {
  movian_smb2_op_t op;
  struct smb2_statvfs vfs;
  char path[];
} movian_smb2_statvfs_op_t;

typedef struct {
  movian_smb2_op_t op;
  const char *oldpath;          /* both point into buf */
  const char *newpath;
  char buf[];
} movian_smb2_rename_op_t;

/*
 * Plain scandir result entry, copied out of the smb2dir by the owner thread
 * so the VFS thread never touches the context.
 */
typedef struct {
  char *name;
  uint32_t attributes;
  int is_dir;
  int64_t size;
  time_t mtime;
} movian_smb2_dirent_t;

typedef struct {
  movian_smb2_op_t op;
  movian_smb2_dirent_t *entries;
  int nentries;
  char path[];
} movian_smb2_scandir_op_t;


static int
movian_smb2_unlink_submit(movian_smb2_session_t *session,
                          movian_smb2_op_t *op)
{
  return smb2_unlink_async(session->smb2,
                           ((movian_smb2_path_op_t *)op)->path,
                           movian_smb2_op_cb, op);
}


static int
movian_smb2_rmdir_submit(movian_smb2_session_t *session, movian_smb2_op_t *op)
{
  return smb2_rmdir_async(session->smb2,
                          ((movian_smb2_path_op_t *)op)->path,
                          movian_smb2_op_cb, op);
}


static int
movian_smb2_mkdir_submit(movian_smb2_session_t *session, movian_smb2_op_t *op)
{
  return smb2_mkdir_async(session->smb2,
                          ((movian_smb2_path_op_t *)op)->path,
                          movian_smb2_op_cb, op);
}


static int
movian_smb2_stat_submit(movian_smb2_session_t *session, movian_smb2_op_t *op)
{
  movian_smb2_stat_op_t *o = (movian_smb2_stat_op_t *)op;
  return smb2_stat_async(session->smb2, o->path, &o->st,
                         movian_smb2_op_cb, op);
}


static int
movian_smb2_statvfs_submit(movian_smb2_session_t *session,
                           movian_smb2_op_t *op)
{
  movian_smb2_statvfs_op_t *o = (movian_smb2_statvfs_op_t *)op;
  return smb2_statvfs_async(session->smb2, o->path, &o->vfs,
                            movian_smb2_op_cb, op);
}


static int
movian_smb2_rename_submit(movian_smb2_session_t *session,
                          movian_smb2_op_t *op)
{
  movian_smb2_rename_op_t *o = (movian_smb2_rename_op_t *)op;
  return smb2_rename_async(session->smb2, o->oldpath, o->newpath,
                           movian_smb2_op_cb, op);
}


static void
movian_smb2_scandir_entries_free(movian_smb2_scandir_op_t *o)
{
  for(int i = 0; i < o->nentries; i++)
    free(o->entries[i].name);
  free(o->entries);
  o->entries = NULL;
  o->nentries = 0;
}


/*
 * Called (owner thread) when a scandir op completes after its waiter gave
 * up: nobody will consume the entry vector, release it. The blob itself is
 * parked on the session's deferred list.
 */
static void
movian_smb2_scandir_abandoned_fini(movian_smb2_op_t *op)
{
  movian_smb2_scandir_entries_free((movian_smb2_scandir_op_t *)op);
}


/*
 * Owner-thread completion for opendir: iterate the smb2dir, copy the fields
 * the VFS layer needs into a plain vector, close the dir, then resolve the
 * op. On entry-copy OOM the op fails with -ENOMEM instead of publishing a
 * silently truncated listing.
 */
static void
movian_smb2_scandir_op_cb(struct smb2_context *smb2, int status,
                          void *command_data, void *cb_data)
{
  movian_smb2_op_t *op = cb_data;
  movian_smb2_scandir_op_t *o = (movian_smb2_scandir_op_t *)op;

  if(status == 0 && command_data != NULL) {
    struct smb2dir *dir = command_data;
    struct smb2dirent *entry;
    int alloc = 0;

    while((entry = smb2_readdir(smb2, dir)) != NULL) {
      if(!strcmp(entry->name, ".") || !strcmp(entry->name, ".."))
        continue;

      if(o->nentries == alloc) {
        alloc = alloc == 0 ? 64 : alloc * 2;
        movian_smb2_dirent_t *nv =
          realloc(o->entries, alloc * sizeof(o->entries[0]));
        if(nv == NULL) {
          status = -ENOMEM;
          break;
        }
        o->entries = nv;
      }

      movian_smb2_dirent_t *de = &o->entries[o->nentries];
      de->name = strdup(entry->name);
      if(de->name == NULL) {
        status = -ENOMEM;
        break;
      }
      de->attributes = entry->st.smb2_file_attributes;
      de->is_dir = entry->st.smb2_type == SMB2_TYPE_DIRECTORY;
      de->size = entry->st.smb2_size;
      de->mtime = entry->st.smb2_mtime;
      o->nentries++;
    }
    smb2_closedir(smb2, dir);
    if(status != 0)
      movian_smb2_scandir_entries_free(o);
  }
  movian_smb2_op_completed(op, status);
}


static int
movian_smb2_scandir_submit(movian_smb2_session_t *session,
                           movian_smb2_op_t *op)
{
  movian_smb2_scandir_op_t *o = (movian_smb2_scandir_op_t *)op;
  return smb2_opendir_async(session->smb2, o->path,
                            movian_smb2_scandir_op_cb, op);
}

/* Error-code translation kept local to the VFS layer. */

static fa_err_code_t
movian_smb2_errno_to_fap(int status)
{
  int err = -status;
  if(status < -4096)
    err = nterror_to_errno((uint32_t)status);

  switch(err) {
  case 0:
    return FAP_OK;
  case ENOENT:
    return FAP_NOENT;
  case EEXIST:
    return FAP_EXIST;
  case EACCES:
  case EPERM:
  case EROFS:
    return FAP_PERMISSION_DENIED;
  default:
    return FAP_ERROR;
  }
}


static char *
movian_smb2_child_url(const char *parent, const char *name)
{
  size_t parent_len = strlen(parent);
  while(parent_len > 0 && parent[parent_len - 1] == '/')
    parent_len--;

  size_t name_len = strlen(name);
  if(parent_len > SIZE_MAX - name_len - 2)
    return NULL;

  size_t len = parent_len + name_len + 2;
  char *url = malloc(len);
  if(url != NULL) {
    memcpy(url, parent, parent_len);
    url[parent_len] = '/';
    memcpy(url + parent_len + 1, name, name_len + 1);
  }
  return url;
}


static rstr_t *
movian_smb2_redirect(fa_protocol_t *fap, const char *url)
{
  const char *path = url + strlen("smb2://");
  if(*path == '\0' || strchr(path, '/') != NULL)
    return NULL;

  char redirected[URL_MAX];
  int len = snprintf(redirected, sizeof(redirected), "%s/", url);
  if(len < 0 || (size_t)len >= sizeof(redirected))
    return NULL;

  SMB2TRACE("Redirecting %s -> %s", url, redirected);
  return rstr_alloc(redirected);
}


static int
movian_smb2_normalize(fa_protocol_t *fap, const char *url,
                      char *dst, size_t dstlen)
{
  movian_smb2_target_t target = {};
  char errbuf[128];

  if(movian_smb2_target_parse(url, &target, errbuf, sizeof(errbuf)))
    return -1;

  int host_root = target.share == NULL || target.share[0] == '\0';
  movian_smb2_target_fini(&target);

  if(!host_root)
    return -1;

  size_t len = strlen(url);
  if(dstlen == 0 || len >= dstlen)
    return -1;

  memcpy(dst, url, len + 1);
  return 0;
}


/*
 * Share enumeration op. smb2_share_enum_async's builders are context calls,
 * so submission goes through the owner thread like everything else; the
 * completion callback (owner thread) copies the share names/types into a
 * plain vector and frees the srvsvc reply, so the VFS thread never touches
 * the context.
 */
typedef struct {
  char *name;
  uint32_t type;
} movian_smb2_share_t;

typedef struct {
  movian_smb2_op_t op;
  movian_smb2_share_t *shares;
  int nshares;
} movian_smb2_enum_op_t;


static void
movian_smb2_enum_shares_free(movian_smb2_enum_op_t *o)
{
  for(int i = 0; i < o->nshares; i++)
    free(o->shares[i].name);
  free(o->shares);
  o->shares = NULL;
  o->nshares = 0;
}


static void
movian_smb2_enum_abandoned_fini(movian_smb2_op_t *op)
{
  movian_smb2_enum_shares_free((movian_smb2_enum_op_t *)op);
}


static void
movian_smb2_enum_op_cb(struct smb2_context *smb2, int status,
                       void *command_data, void *cb_data)
{
  movian_smb2_op_t *op = cb_data;
  movian_smb2_enum_op_t *o = (movian_smb2_enum_op_t *)op;
  struct srvsvc_NetrShareEnum_rep *rep = command_data;
  int op_status;

  if(status == 0 && rep != NULL) {
    op_status = 0;
    struct srvsvc_SHARE_INFO_1_CONTAINER *shares =
      &rep->ses.ShareEnum.Level1;
    if(shares->share_info_1 != NULL && shares->EntriesRead > 0) {
      o->shares = calloc(shares->EntriesRead, sizeof(o->shares[0]));
      if(o->shares == NULL) {
        op_status = -ENOMEM;
      } else {
        for(uint32_t i = 0; i < shares->EntriesRead; i++) {
          const char *name = shares->share_info_1[i].netname;
          if(name == NULL)
            continue;
          o->shares[o->nshares].name = strdup(name);
          if(o->shares[o->nshares].name == NULL) {
            op_status = -ENOMEM;
            break;
          }
          o->shares[o->nshares].type = shares->share_info_1[i].type;
          o->nshares++;
        }
      }
      if(op_status != 0)
        movian_smb2_enum_shares_free(o);
    }
  } else {
    /* status is a raw DCERPC/NT verdict here (or the WERROR from the
       NetrShareEnum reply); normalize failures to a negative status so
       op_run callers see a failed op. */
    op_status = status < 0 ? status : -EIO;
  }

  if(rep != NULL)
    smb2_free_data(smb2, rep);
  movian_smb2_op_completed(op, op_status);
}


static int
movian_smb2_enum_submit(movian_smb2_session_t *session, movian_smb2_op_t *op)
{
  return smb2_share_enum_async(session->smb2, SHARE_INFO_1,
                               movian_smb2_enum_op_cb, op);
}


static int
movian_smb2_scan_host(fa_dir_t *fd, const char *url,
                      const movian_smb2_target_t *target, int flags,
                      char *errbuf, size_t errlen)
{
  SMB2TRACE("Enumerating shares on %s", target->server);
  movian_smb2_session_t *session =
    movian_smb2_session_acquire(target, "IPC$", flags, SMB2_DEFAULT_TIMEOUT,
                                NULL, errbuf, errlen);
  if(session == NULL)
    return -1;

  movian_smb2_enum_op_t *o = calloc(1, sizeof(*o));
  if(o == NULL) {
    snprintf(errbuf, errlen, "Out of memory while enumerating SMB2 shares");
    movian_smb2_session_release(session);
    return -1;
  }
  o->op.abandoned_fini = movian_smb2_enum_abandoned_fini;

  char reason[168];
  int status = movian_smb2_op_run(session, &o->op, "share-enum",
                                  movian_smb2_enum_submit,
                                  SMB2_DEFAULT_TIMEOUT,
                                  reason, sizeof(reason));
  if(status < 0) {
    snprintf(errbuf, errlen, "Unable to enumerate SMB2 shares: %s", reason);
    SMB2TRACE("Share enumeration failed status=%d reason=%s",
              status, errbuf);
    movian_smb2_session_error(session, status);
    if(!o->op.parked) {
      movian_smb2_enum_shares_free(o);
      free(o);
    }
    movian_smb2_session_release(session);
    return -1;
  }

  for(int i = 0; i < o->nshares; i++) {
    const char *name = o->shares[i].name;
    uint32_t share_type = o->shares[i].type;
    int add = (share_type & 3) == SHARE_TYPE_DISKTREE &&
      !(share_type & SHARE_TYPE_HIDDEN);

    SMB2TRACE("Enumerated share %s type=0x%x -> %s",
              name, share_type, add ? "add" : "filtered");

    if(!add)
      continue;

    char *child = movian_smb2_child_url(url, name);
    if(child == NULL)
      continue;

    fa_dir_entry_t *fde = fa_dir_add(fd, child, name, CONTENT_SHARE);
    if(fde != NULL) {
      fde->fde_stat.fs_type = CONTENT_SHARE;
      fde->fde_statdone = 1;
    }
    free(child);
  }

  movian_smb2_enum_shares_free(o);
  free(o);
  movian_smb2_session_release(session);
  return 0;
}


static int
movian_smb2_scan_directory(fa_dir_t *fd, const char *url,
                           const movian_smb2_target_t *target, int flags,
                           char *errbuf, size_t errlen)
{
  SMB2TRACE("scan directory url=%s share=%s path='%s' flags=0x%x",
            url, target->share, target->path, flags);

  movian_smb2_session_t *session =
    movian_smb2_session_acquire(target, target->share, flags,
                                SMB2_DEFAULT_TIMEOUT, NULL, errbuf, errlen);
  if(session == NULL) {
    SMB2TRACE("scan directory connect failed url=%s reason=%s", url, errbuf);
    return -1;
  }

  size_t path_len = strlen(target->path);
  movian_smb2_scandir_op_t *o = calloc(1, sizeof(*o) + path_len + 1);
  if(o == NULL) {
    snprintf(errbuf, errlen, "Out of memory while scanning SMB2 directory");
    movian_smb2_session_release(session);
    return -1;
  }
  memcpy(o->path, target->path, path_len + 1);
  o->op.abandoned_fini = movian_smb2_scandir_abandoned_fini;

  char reason[168];
  int status = movian_smb2_op_run(session, &o->op, "scandir",
                                  movian_smb2_scandir_submit,
                                  SMB2_DEFAULT_TIMEOUT,
                                  reason, sizeof(reason));
  if(status < 0) {
    snprintf(errbuf, errlen, "Unable to open SMB2 directory: %s", reason);
    SMB2TRACE("scan directory failed url=%s path='%s' reason=%s",
              url, target->path, errbuf);
    movian_smb2_session_error(session, status);
    if(!o->op.parked) {
      movian_smb2_scandir_entries_free(o);
      free(o);
    }
    movian_smb2_session_release(session);
    return -1;
  }

  int entries = 0;
  for(int i = 0; i < o->nentries; i++) {
    const movian_smb2_dirent_t *de = &o->entries[i];

    if(de->attributes &
       (SMB2_FILE_ATTRIBUTE_HIDDEN | SMB2_FILE_ATTRIBUTE_SYSTEM)) {
      SMB2TRACE("scan directory entry url=%s name=%s skipped "
                "(hidden/system attrs=0x%x)", url, de->name, de->attributes);
      continue;
    }

    int type = de->is_dir ? CONTENT_DIR : CONTENT_FILE;
    char *child = movian_smb2_child_url(url, de->name);
    if(child == NULL)
      continue;

    SMB2TRACE("scan directory entry url=%s name=%s child=%s type=%d",
              url, de->name, child, type);
    fa_dir_entry_t *fde = fa_dir_add(fd, child, de->name, type);
    if(fde != NULL) {
      fde->fde_stat.fs_type = type;
      fde->fde_stat.fs_size = de->size;
      fde->fde_stat.fs_mtime = de->mtime;
      fde->fde_statdone = 1;
      entries++;
    }
    free(child);
  }

  SMB2TRACE("scan directory done url=%s path='%s' entries=%d",
            url, target->path, entries);
  movian_smb2_scandir_entries_free(o);
  free(o);
  movian_smb2_session_release(session);
  return 0;
}


static int
movian_smb2_scan(fa_protocol_t *fap, fa_dir_t *fd, const char *url,
                 char *errbuf, size_t errlen, int flags)
{
  movian_smb2_target_t target = {};
  if(movian_smb2_target_parse(url, &target, errbuf, errlen)) {
    SMB2TRACE("scan parse failed url=%s reason=%s", url, errbuf);
    return -1;
  }

  SMB2TRACE("scan url=%s server=%s share=%s path='%s' flags=0x%x",
            url, target.server,
            target.share != NULL ? target.share : "<host>",
            target.path, flags);
  int status = target.share == NULL || target.share[0] == '\0' ?
    movian_smb2_scan_host(fd, url, &target, flags, errbuf, errlen) :
    movian_smb2_scan_directory(fd, url, &target, flags, errbuf, errlen);
  SMB2TRACE("scan done url=%s status=%d%s%s", url, status,
            status ? " reason=" : "", status ? errbuf : "");

  movian_smb2_target_fini(&target);
  return status;
}


static fa_handle_t *
movian_smb2_open(fa_protocol_t *fap, const char *url,
                 char *errbuf, size_t errlen, int flags,
                 fa_open_extra_t *foe)
{
  movian_smb2_target_t target = {};
  if(movian_smb2_target_parse(url, &target, errbuf, errlen)) {
    SMB2TRACE("open parse failed url=%s reason=%s", url, errbuf);
    return NULL;
  }

  SMB2TRACE("open url=%s share=%s path='%s' flags=0x%x",
            url, target.share != NULL ? target.share : "<host>",
            target.path, flags);
  if(target.share == NULL || target.share[0] == '\0' ||
     target.path[0] == '\0') {
    snprintf(errbuf, errlen, "SMB2 URL does not identify a file");
    SMB2TRACE("open rejected url=%s reason=%s", url, errbuf);
    movian_smb2_target_fini(&target);
    return NULL;
  }

  int timeout = foe != NULL && foe->foe_open_timeout > 0 ?
    (foe->foe_open_timeout + 999) / 1000 : SMB2_DEFAULT_TIMEOUT;
  movian_smb2_session_t *session =
    movian_smb2_session_acquire(&target, target.share, flags, timeout,
                                NULL, errbuf, errlen);
  if(session == NULL) {
    movian_smb2_target_fini(&target);
    return NULL;
  }

  int open_flags = O_RDONLY;
  if(flags & FA_WRITE) {
    open_flags = O_RDWR | O_CREAT;
    if(!(flags & FA_APPEND))
      open_flags |= O_TRUNC;
  }

  char reason[168];
  size_t path_len = strlen(target.path);
  movian_smb2_open_op_t *oop = calloc(1, sizeof(*oop) + path_len + 1);
  if(oop == NULL) {
    snprintf(errbuf, errlen, "Out of memory while opening SMB2 file");
    movian_smb2_session_release(session);
    movian_smb2_target_fini(&target);
    return NULL;
  }
  memcpy(oop->path, target.path, path_len + 1);
  oop->open_flags = open_flags;

  int status = movian_smb2_op_run(session, &oop->op, "open",
                                  movian_smb2_open_submit, timeout,
                                  reason, sizeof(reason));
  struct smb2fh *fh = oop->fh;
  if(!oop->op.parked)
    free(oop);

  if(status < 0 || fh == NULL) {
    snprintf(errbuf, errlen, "Unable to open SMB2 file: %s", reason);
    SMB2TRACE("open failed url=%s path='%s' open_flags=0x%x reason=%s",
              url, target.path, open_flags, errbuf);
    movian_smb2_session_error(session, status < 0 ? status : -EIO);
    movian_smb2_session_release(session);
    movian_smb2_target_fini(&target);
    return NULL;
  }

  /* fstat: reject directories and learn the size (used for SEEK_END and
     the FA_APPEND initial cursor). */
  movian_smb2_fstat_op_t *fop = calloc(1, sizeof(*fop));
  if(fop == NULL) {
    snprintf(errbuf, errlen, "Out of memory while opening SMB2 file");
    movian_smb2_close_fh(session, fh);
    movian_smb2_session_release(session);
    movian_smb2_target_fini(&target);
    return NULL;
  }
  fop->fh = fh;
  status = movian_smb2_op_run(session, &fop->op, "fstat",
                              movian_smb2_fstat_submit, SMB2_DEFAULT_TIMEOUT,
                              reason, sizeof(reason));
  struct smb2_stat_64 statbuf = fop->st;
  if(!fop->op.parked)
    free(fop);

  if(status < 0 || statbuf.smb2_type == SMB2_TYPE_DIRECTORY) {
    snprintf(errbuf, errlen, "Unable to stat SMB2 file: %s",
             status < 0 ? reason : "is a directory");
    SMB2TRACE("open fstat rejected url=%s path='%s' smb2_type=%d reason=%s",
              url, target.path, statbuf.smb2_type, errbuf);
    if(status < 0)
      movian_smb2_session_error(session, status);
    movian_smb2_close_fh(session, fh);
    movian_smb2_session_release(session);
    movian_smb2_target_fini(&target);
    return NULL;
  }

  movian_smb2_file_t *file = calloc(1, sizeof(*file));
  if(file == NULL) {
    snprintf(errbuf, errlen, "Out of memory while opening SMB2 file");
    movian_smb2_close_fh(session, fh);
    movian_smb2_session_release(session);
    movian_smb2_target_fini(&target);
    return NULL;
  }

  SMB2TRACE("open ok url=%s path='%s' size=%"PRId64, url, target.path,
            statbuf.smb2_size);
  movian_smb2_target_fini(&target);

  file->h.fh_proto = fap;
  file->fh = fh;
  file->size = statbuf.smb2_size;
  file->read_timeout_sec = SMB2_DEFAULT_TIMEOUT;
  /* FA_APPEND: start the client-side cursor at EOF; writes carry explicit
     offsets so no server-side seek is involved. */
  file->fpos = (flags & FA_APPEND) ? file->size : 0;
  hts_mutex_init(&file->fh_lock);
  file->session = session;      /* ref transfers to the handle */
  return &file->h;
}


static void
movian_smb2_close(fa_handle_t *fh)
{
  movian_smb2_file_t *file = (movian_smb2_file_t *)fh;
  movian_smb2_session_t *session = file->session;

  movian_smb2_close_fh(session, file->fh);

  hts_mutex_destroy(&file->fh_lock);
  free(file);
  movian_smb2_session_release(session);
}


static int
movian_smb2_read(fa_handle_t *fh, void *buf, size_t size)
{
  movian_smb2_file_t *file = (movian_smb2_file_t *)fh;
  movian_smb2_session_t *session = file->session;
  uint32_t count = size > INT_MAX ? INT_MAX : size;

  hts_mutex_lock(&file->fh_lock);
  int timeout = file->read_timeout_sec;
  uint32_t max_read = session->max_read_size;

  uint32_t total = 0;
  int failed = 0;
  int fail_status = 0;
  while(total < count) {
    uint32_t chunk = count - total;
    if(max_read != 0 && chunk > max_read)
      chunk = max_read;

    movian_smb2_rw_op_t *rop = calloc(1, sizeof(*rop));
    if(rop == NULL) {
      failed = 1;
      fail_status = -ENOMEM;
      break;
    }
    rop->fh = file->fh;
    rop->rbuf = (uint8_t *)buf + total;
    rop->count = chunk;
    rop->offset = file->fpos + total;

    char reason[168];
    int status = movian_smb2_op_run(session, &rop->op, "pread",
                                    movian_smb2_pread_submit, timeout,
                                    reason, sizeof(reason));
    if(!rop->op.parked)
      free(rop);

    if(status < 0) {
      SMB2TRACE("read failed offset=%"PRId64" chunk=%u reason=%s",
                file->fpos + total, chunk, reason);
      failed = 1;
      fail_status = status;
      break;
    }
    if(status == 0)
      break;                    /* EOF */
    total += status;
    if((uint32_t)status < chunk)
      break;                    /* short read */
  }

  if(failed)
    movian_smb2_session_error(session, fail_status);
  else
    file->fpos += total;
  hts_mutex_unlock(&file->fh_lock);
  return failed ? -1 : (int)total;
}


static int
movian_smb2_write(fa_handle_t *fh, const void *buf, size_t size)
{
  movian_smb2_file_t *file = (movian_smb2_file_t *)fh;
  movian_smb2_session_t *session = file->session;
  uint32_t count = size > INT_MAX ? INT_MAX : size;

  hts_mutex_lock(&file->fh_lock);
  uint32_t max_write = session->max_write_size;

  uint32_t total = 0;
  int failed = 0;
  int fail_status = 0;
  while(total < count) {
    uint32_t chunk = count - total;
    if(max_write != 0 && chunk > max_write)
      chunk = max_write;

    movian_smb2_rw_op_t *wop = calloc(1, sizeof(*wop));
    if(wop == NULL) {
      failed = 1;
      fail_status = -ENOMEM;
      break;
    }
    wop->fh = file->fh;
    wop->wbuf = (const uint8_t *)buf + total;
    wop->count = chunk;
    wop->offset = file->fpos + total;

    char reason[168];
    int status = movian_smb2_op_run(session, &wop->op, "pwrite",
                                    movian_smb2_pwrite_submit,
                                    SMB2_DEFAULT_TIMEOUT,
                                    reason, sizeof(reason));
    if(!wop->op.parked)
      free(wop);

    if(status < 0) {
      SMB2TRACE("write failed offset=%"PRId64" chunk=%u reason=%s",
                file->fpos + total, chunk, reason);
      failed = 1;
      fail_status = status;
      break;
    }
    if(status == 0)
      break;
    total += status;
    if((uint32_t)status < chunk)
      break;
  }

  if(failed)
    movian_smb2_session_error(session, fail_status);

  /* POSIX-style short-write semantics: report bytes actually written
     whenever we wrote something, even if a later chunk failed. The cursor
     reflects the data that made it to the server. */
  file->fpos += total;
  if(file->fpos > file->size)
    file->size = file->fpos;
  hts_mutex_unlock(&file->fh_lock);
  return (failed && total == 0) ? -1 : (int)total;
}


static int64_t
movian_smb2_seek(fa_handle_t *fh, int64_t pos, int whence, int lazy)
{
  movian_smb2_file_t *file = (movian_smb2_file_t *)fh;
  int64_t np;

  hts_mutex_lock(&file->fh_lock);
  switch(whence) {
  case SEEK_SET:
    np = pos;
    break;
  case SEEK_CUR:
    np = file->fpos + pos;
    break;
  case SEEK_END:
    np = file->size + pos;
    break;
  default:
    np = -1;
    break;
  }
  if(np >= 0)
    file->fpos = np;
  hts_mutex_unlock(&file->fh_lock);
  return np >= 0 ? np : -1;
}


static int64_t
movian_smb2_fsize(fa_handle_t *fh)
{
  movian_smb2_file_t *file = (movian_smb2_file_t *)fh;
  return file->size;
}


static fa_err_code_t
movian_smb2_ftruncate(fa_handle_t *fh, uint64_t newsize)
{
  movian_smb2_file_t *file = (movian_smb2_file_t *)fh;
  movian_smb2_session_t *session = file->session;

  movian_smb2_fh_op_t *o = calloc(1, sizeof(*o));
  if(o == NULL)
    return FAP_ERROR;
  o->fh = file->fh;
  o->length = newsize;

  hts_mutex_lock(&file->fh_lock);
  char reason[168];
  int status = movian_smb2_op_run(session, &o->op, "ftruncate",
                                  movian_smb2_ftruncate_submit,
                                  SMB2_DEFAULT_TIMEOUT,
                                  reason, sizeof(reason));
  if(!o->op.parked)
    free(o);
  if(status == 0)
    file->size = newsize > INT64_MAX ? INT64_MAX : (int64_t)newsize;
  else if(status < 0)
    movian_smb2_session_error(session, status);
  hts_mutex_unlock(&file->fh_lock);

  return status < 0 ? movian_smb2_errno_to_fap(status) : FAP_OK;
}


static void
movian_smb2_set_read_timeout(fa_handle_t *fh, int ms)
{
  movian_smb2_file_t *file = (movian_smb2_file_t *)fh;
  int seconds = ms > 0 ? (ms + 999) / 1000 : 0;

  hts_mutex_lock(&file->fh_lock);
  file->read_timeout_sec = seconds;
  hts_mutex_unlock(&file->fh_lock);
}


static int
movian_smb2_stat(fa_protocol_t *fap, const char *url, struct fa_stat *fs,
                 int flags, char *errbuf, size_t errlen)
{
  movian_smb2_target_t target = {};
  if(movian_smb2_target_parse(url, &target, errbuf, errlen)) {
    SMB2TRACE("stat parse failed url=%s reason=%s", url, errbuf);
    return FAP_ERROR;
  }

  SMB2TRACE("stat url=%s share=%s path='%s' flags=0x%x", url,
            target.share != NULL ? target.share : "<host>", target.path,
            flags);
  memset(fs, 0, sizeof(*fs));
  if(target.share == NULL || target.share[0] == '\0') {
    fs->fs_type = CONTENT_SHARE;
    SMB2TRACE("stat host url=%s type=%d", url, fs->fs_type);
    movian_smb2_target_fini(&target);
    return FAP_OK;
  }

  int auth_needed = 0;
  movian_smb2_session_t *session =
    movian_smb2_session_acquire(&target, target.share, flags,
                                SMB2_DEFAULT_TIMEOUT, &auth_needed,
                                errbuf, errlen);
  if(session == NULL) {
    SMB2TRACE("stat connect failed url=%s auth_needed=%d reason=%s",
              url, auth_needed, errbuf);
    movian_smb2_target_fini(&target);
    return auth_needed ? FAP_NEED_AUTH : FAP_ERROR;
  }

  int status = FAP_OK;

  if(target.path[0] == '\0') {
    fs->fs_type = CONTENT_DIR;
  } else {
    size_t path_len = strlen(target.path);
    movian_smb2_stat_op_t *o = calloc(1, sizeof(*o) + path_len + 1);
    if(o == NULL) {
      movian_smb2_session_release(session);
      movian_smb2_target_fini(&target);
      return FAP_ERROR;
    }
    memcpy(o->path, target.path, path_len + 1);

    char reason[168];
    int rc = movian_smb2_op_run(session, &o->op, "stat",
                                movian_smb2_stat_submit, SMB2_DEFAULT_TIMEOUT,
                                reason, sizeof(reason));
    struct smb2_stat_64 statbuf = o->st;
    if(!o->op.parked)
      free(o);

    if(rc < 0) {
      snprintf(errbuf, errlen, "Unable to stat SMB2 path: %s", reason);
      SMB2TRACE("stat failed url=%s path='%s' reason=%s", url,
                target.path, errbuf);
      movian_smb2_session_error(session, rc);
      status = FAP_ERROR;
    } else {
      fs->fs_type = statbuf.smb2_type == SMB2_TYPE_DIRECTORY ?
        CONTENT_DIR : CONTENT_FILE;
      fs->fs_size = statbuf.smb2_size;
      fs->fs_mtime = statbuf.smb2_mtime;
      SMB2TRACE("stat ok url=%s path='%s' smb2_type=%d type=%d size=%"PRId64,
                url, target.path, statbuf.smb2_type, fs->fs_type,
                fs->fs_size);
    }
  }

  SMB2TRACE("stat done url=%s status=%d type=%d", url, status,
            fs->fs_type);
  movian_smb2_session_release(session);
  movian_smb2_target_fini(&target);
  return status;
}


/*
 * Run a single-path async op against a pooled session.
 *
 * Parses the URL, requires a share + path, acquires the session, runs the
 * op through the session's owner thread, and tears everything down on every
 * path. Returns the op status (0 on success, <0 = -errno on failure), or -1
 * for URL/connect/acquire failures before the op runs. Callers map that to
 * their own return type.
 *
 * This collapses the near-identical parse/validate/acquire/run/release
 * scaffolding that unlink/rmdir/makedir would otherwise each duplicate.
 */
static int
movian_smb2_run_path_op(const char *url, const char *what,
                        const char *opname,
                        movian_smb2_op_submit_fn_t *submit,
                        char *errbuf, size_t errlen)
{
  movian_smb2_target_t target = {};
  if(movian_smb2_target_parse(url, &target, errbuf, errlen))
    return -1;

  if(target.share == NULL || target.share[0] == '\0' ||
     target.path[0] == '\0') {
    snprintf(errbuf, errlen, "SMB2 URL does not identify a path");
    movian_smb2_target_fini(&target);
    return -1;
  }

  movian_smb2_session_t *session =
    movian_smb2_session_acquire(&target, target.share, 0,
                                SMB2_DEFAULT_TIMEOUT, NULL, errbuf, errlen);
  if(session == NULL) {
    movian_smb2_target_fini(&target);
    return -1;
  }

  size_t path_len = strlen(target.path);
  movian_smb2_path_op_t *o = calloc(1, sizeof(*o) + path_len + 1);
  if(o == NULL) {
    snprintf(errbuf, errlen, "Unable to %s: out of memory", what);
    movian_smb2_session_release(session);
    movian_smb2_target_fini(&target);
    return -1;
  }
  memcpy(o->path, target.path, path_len + 1);

  char reason[168];
  int status = movian_smb2_op_run(session, &o->op, opname, submit,
                                  SMB2_DEFAULT_TIMEOUT,
                                  reason, sizeof(reason));
  if(!o->op.parked)
    free(o);
  if(status < 0) {
    snprintf(errbuf, errlen, "Unable to %s: %s", what, reason);
    movian_smb2_session_error(session, status);
  }

  movian_smb2_session_release(session);
  movian_smb2_target_fini(&target);
  return status;
}


static int
movian_smb2_unlink(const fa_protocol_t *fap, const char *url,
                   char *errbuf, size_t errlen)
{
  int status = movian_smb2_run_path_op(url, "delete SMB2 file", "unlink",
                                       movian_smb2_unlink_submit,
                                       errbuf, errlen);
  return status < 0 ? -1 : 0;
}


static int
movian_smb2_rmdir(const fa_protocol_t *fap, const char *url,
                  char *errbuf, size_t errlen)
{
  int status = movian_smb2_run_path_op(url, "remove SMB2 directory", "rmdir",
                                       movian_smb2_rmdir_submit,
                                       errbuf, errlen);
  return status < 0 ? -1 : 0;
}


static int
movian_smb2_rename(const fa_protocol_t *fap, const char *old_url,
                   const char *new_url, char *errbuf, size_t errlen)
{
  movian_smb2_target_t old_target = {};
  movian_smb2_target_t new_target = {};

  if(movian_smb2_target_parse(old_url, &old_target, errbuf, errlen))
    return -1;

  if(movian_smb2_target_parse(new_url, &new_target, errbuf, errlen)) {
    movian_smb2_target_fini(&old_target);
    return -1;
  }

  if(old_target.share == NULL || old_target.share[0] == '\0' ||
     new_target.share == NULL || new_target.share[0] == '\0' ||
     old_target.path[0] == '\0' || new_target.path[0] == '\0') {
    snprintf(errbuf, errlen, "SMB2 URL does not identify a path");
    movian_smb2_target_fini(&old_target);
    movian_smb2_target_fini(&new_target);
    return -1;
  }

  if(strcmp(old_target.server, new_target.server) != 0 ||
     strcmp(old_target.share, new_target.share) != 0) {
    snprintf(errbuf, errlen, "Cross-share SMB2 rename not supported");
    movian_smb2_target_fini(&old_target);
    movian_smb2_target_fini(&new_target);
    return -2;
  }

  movian_smb2_session_t *session =
    movian_smb2_session_acquire(&old_target, old_target.share, 0,
                                SMB2_DEFAULT_TIMEOUT, NULL, errbuf, errlen);
  if(session == NULL) {
    movian_smb2_target_fini(&old_target);
    movian_smb2_target_fini(&new_target);
    return -1;
  }

  size_t old_len = strlen(old_target.path);
  size_t new_len = strlen(new_target.path);
  movian_smb2_rename_op_t *o =
    calloc(1, sizeof(*o) + old_len + 1 + new_len + 1);
  if(o == NULL) {
    snprintf(errbuf, errlen, "Unable to rename SMB2 path: out of memory");
    movian_smb2_session_release(session);
    movian_smb2_target_fini(&old_target);
    movian_smb2_target_fini(&new_target);
    return -1;
  }
  memcpy(o->buf, old_target.path, old_len + 1);
  memcpy(o->buf + old_len + 1, new_target.path, new_len + 1);
  o->oldpath = o->buf;
  o->newpath = o->buf + old_len + 1;

  char reason[168];
  int status = movian_smb2_op_run(session, &o->op, "rename",
                                  movian_smb2_rename_submit,
                                  SMB2_DEFAULT_TIMEOUT,
                                  reason, sizeof(reason));
  if(!o->op.parked)
    free(o);
  if(status < 0) {
    snprintf(errbuf, errlen, "Unable to rename SMB2 path: %s", reason);
    movian_smb2_session_error(session, status);
  }

  movian_smb2_session_release(session);
  movian_smb2_target_fini(&old_target);
  movian_smb2_target_fini(&new_target);
  return status == -EXDEV ? -2 : status < 0 ? -1 : 0;
}


static fa_err_code_t
movian_smb2_makedir(fa_protocol_t *fap, const char *url)
{
  char errbuf[256];
  int status = movian_smb2_run_path_op(url, "create SMB2 directory", "mkdir",
                                       movian_smb2_mkdir_submit,
                                       errbuf, sizeof(errbuf));
  /* -1 is not a server verdict: run_path_op returns it for its own parse/
     connect failures (and -EPERM stays ambiguous with it). Mapping it
     through errno_to_fap would turn those into EPERM/"Permission denied". */
  if(status == -1)
    return FAP_ERROR;
  return status < 0 ? movian_smb2_errno_to_fap(status) : FAP_OK;
}


static fa_err_code_t
movian_smb2_fsinfo(fa_protocol_t *fap, const char *url, fa_fsinfo_t *ffi)
{
  char errbuf[256];
  movian_smb2_target_t target = {};

  memset(ffi, 0, sizeof(*ffi));

  if(movian_smb2_target_parse(url, &target, errbuf, sizeof(errbuf)))
    return FAP_ERROR;

  if(target.share == NULL || target.share[0] == '\0') {
    movian_smb2_target_fini(&target);
    return FAP_ERROR;
  }

  movian_smb2_session_t *session =
    movian_smb2_session_acquire(&target, target.share, 0,
                                SMB2_DEFAULT_TIMEOUT, NULL,
                                errbuf, sizeof(errbuf));
  if(session == NULL) {
    movian_smb2_target_fini(&target);
    return FAP_ERROR;
  }

  size_t path_len = strlen(target.path);
  movian_smb2_statvfs_op_t *o = calloc(1, sizeof(*o) + path_len + 1);
  if(o == NULL) {
    movian_smb2_session_release(session);
    movian_smb2_target_fini(&target);
    return FAP_ERROR;
  }
  memcpy(o->path, target.path, path_len + 1);

  char reason[168];
  int status = movian_smb2_op_run(session, &o->op, "statvfs",
                                  movian_smb2_statvfs_submit,
                                  SMB2_DEFAULT_TIMEOUT,
                                  reason, sizeof(reason));
  struct smb2_statvfs vfs = o->vfs;
  if(!o->op.parked)
    free(o);
  if(status < 0)
    movian_smb2_session_error(session, status);

  if(status == 0) {
    uint64_t frsize = vfs.f_frsize;
    ffi->ffi_size = frsize != 0 && vfs.f_blocks > UINT64_MAX / frsize ?
      UINT64_MAX : (uint64_t)vfs.f_blocks * frsize;
    ffi->ffi_avail = frsize != 0 && vfs.f_bavail > UINT64_MAX / frsize ?
      UINT64_MAX : (uint64_t)vfs.f_bavail * frsize;
  }

  movian_smb2_session_release(session);
  movian_smb2_target_fini(&target);
  return status < 0 ? movian_smb2_errno_to_fap(status) : FAP_OK;
}


/*
 * Extended attributes (SMB2 FILE_FULL_EA_INFORMATION), used by
 * fa_kvstore_as_xattr to store resume/watched metadata as
 * showtime.default.<domain>.<key> EAs on smb2:// files -- see fa_proto.h
 * fap_set_xattr/fap_get_xattr and kvstore.c opt_get_ea/kv_write_xattr.
 *
 * libsmb2 has no high-level EA helpers, so these drive the raw
 * CREATE -> SET_INFO|QUERY_INFO -> CLOSE commands directly, compounded into
 * a single request/response round trip (the same idiom libsmb2.c itself uses
 * internally for e.g. smb2_truncate_async/smb2_stat_async). The compound is
 * built by the op's submit function on the session's I/O owner thread; the
 * per-command callbacks stash the raw NT statuses in the op blob and the
 * trailing close callback resolves the op. Both ops resolve the session
 * non-interactively, matching the native cifs backend's smb_set_xattr/
 * smb_get_xattr (fa_nativesmb.c), which never prompt for credentials either.
 */

#define MOVIAN_SMB2_EA_NAME_MAX 255
#define MOVIAN_SMB2_EA_OUTPUT_MAX 4096

static fa_err_code_t
movian_smb2_xattr_status_to_fap(uint32_t status)
{
  switch(status) {
  case SMB2_STATUS_SUCCESS:
    return FAP_OK;
  case SMB2_STATUS_ACCESS_DENIED:
    return FAP_PERMISSION_DENIED;
  case SMB2_STATUS_EAS_NOT_SUPPORTED:
    return FAP_NOT_SUPPORTED;
  default:
    return FAP_ERROR;
  }
}


typedef struct {
  movian_smb2_op_t op;
  uint32_t create_status;      /* raw NT statuses from the compound */
  uint32_t set_status;
  struct smb2_file_full_ea_info eai;
  const char *path;            /* into data[] */
  uint8_t data[];              /* path NUL, EA name NUL, EA value */
} movian_smb2_xattr_set_op_t;


static void
movian_smb2_xattr_set_create_cb(struct smb2_context *smb2, int status,
                                void *command_data, void *cb_data)
{
  ((movian_smb2_xattr_set_op_t *)cb_data)->create_status = status;
}

static void
movian_smb2_xattr_set_info_cb(struct smb2_context *smb2, int status,
                              void *command_data, void *cb_data)
{
  ((movian_smb2_xattr_set_op_t *)cb_data)->set_status = status;
}

static void
movian_smb2_xattr_set_close_cb(struct smb2_context *smb2, int status,
                               void *command_data, void *cb_data)
{
  /* Last command in the compound: the op is complete. The verdict lives in
     create_status/set_status; the op itself succeeded as an exchange. */
  movian_smb2_op_completed(cb_data, 0);
}


static int
movian_smb2_xattr_set_submit(movian_smb2_session_t *session,
                             movian_smb2_op_t *op)
{
  movian_smb2_xattr_set_op_t *o = (movian_smb2_xattr_set_op_t *)op;
  struct smb2_context *smb2 = session->smb2;

  struct smb2_create_request cr_req = {};
  cr_req.requested_oplock_level = SMB2_OPLOCK_LEVEL_NONE;
  cr_req.impersonation_level = SMB2_IMPERSONATION_IMPERSONATION;
  cr_req.desired_access = SMB2_FILE_WRITE_EA;
  cr_req.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE;
  cr_req.create_disposition = SMB2_FILE_OPEN;
  cr_req.create_options = SMB2_FILE_NON_DIRECTORY_FILE;
  cr_req.name = o->path;

  struct smb2_set_info_request si_req = {};
  si_req.info_type = SMB2_0_INFO_FILE;
  si_req.file_info_class = SMB2_FILE_FULL_EA_INFORMATION;
  si_req.input_data = &o->eai;
  memcpy(si_req.file_id, compound_file_id, SMB2_FD_SIZE);

  struct smb2_close_request cl_req = {};
  memcpy(cl_req.file_id, compound_file_id, SMB2_FD_SIZE);

  struct smb2_pdu *pdu =
    smb2_cmd_create_async(smb2, &cr_req, movian_smb2_xattr_set_create_cb, op);
  struct smb2_pdu *next_pdu = pdu == NULL ? NULL :
    smb2_cmd_set_info_async(smb2, &si_req, movian_smb2_xattr_set_info_cb, op);
  if(next_pdu != NULL)
    smb2_add_compound_pdu(smb2, pdu, next_pdu);

  struct smb2_pdu *close_pdu = next_pdu == NULL ? NULL :
    smb2_cmd_close_async(smb2, &cl_req, movian_smb2_xattr_set_close_cb, op);
  if(close_pdu != NULL)
    smb2_add_compound_pdu(smb2, pdu, close_pdu);

  if(pdu == NULL || next_pdu == NULL || close_pdu == NULL) {
    if(pdu != NULL)
      smb2_free_pdu(smb2, pdu);
    return -ENOMEM;
  }
  smb2_queue_pdu(smb2, pdu);
  return 0;
}


static fa_err_code_t
movian_smb2_set_xattr(struct fa_protocol *fap, const char *url,
                      const char *name, const void *data, size_t len)
{
  movian_smb2_target_t target = {};
  char errbuf[256];

  if(movian_smb2_target_parse(url, &target, errbuf, sizeof(errbuf))) {
    SMB2TRACE("set_xattr parse failed url=%s reason=%s", url, errbuf);
    return FAP_ERROR;
  }

  if(target.share == NULL || target.share[0] == '\0' || target.path[0] == '\0') {
    movian_smb2_target_fini(&target);
    return FAP_ERROR;
  }

  size_t name_len = strlen(name);
  size_t value_len = data == NULL ? 0 : len;
  if(name_len > MOVIAN_SMB2_EA_NAME_MAX || value_len > UINT16_MAX) {
    movian_smb2_target_fini(&target);
    return FAP_ERROR;
  }

  SMB2TRACE("set_xattr url=%s name=%s len=%zu%s", url, name, value_len,
            data == NULL ? " (delete)" : "");

  movian_smb2_session_t *session =
    movian_smb2_session_acquire(&target, target.share, FA_NON_INTERACTIVE,
                                SMB2_DEFAULT_TIMEOUT, NULL, errbuf,
                                sizeof(errbuf));
  if(session == NULL) {
    SMB2TRACE("set_xattr connect failed url=%s reason=%s", url, errbuf);
    movian_smb2_target_fini(&target);
    return FAP_ERROR;
  }

  size_t path_len = strlen(target.path);
  movian_smb2_xattr_set_op_t *o =
    calloc(1, sizeof(*o) + path_len + 1 + name_len + 1 + value_len);
  if(o == NULL) {
    movian_smb2_session_release(session);
    movian_smb2_target_fini(&target);
    return FAP_ERROR;
  }
  memcpy(o->data, target.path, path_len + 1);
  memcpy(o->data + path_len + 1, name, name_len + 1);
  if(value_len > 0)
    memcpy(o->data + path_len + 1 + name_len + 1, data, value_len);
  o->path = (const char *)o->data;
  o->eai.ea_name_length = (uint8_t)name_len;
  o->eai.ea_value_length = (uint16_t)value_len;
  o->eai.ea_name = (const char *)o->data + path_len + 1;
  o->eai.ea_value = value_len > 0 ?
    o->data + path_len + 1 + name_len + 1 : NULL;

  char reason[168];
  int rc = movian_smb2_op_run(session, &o->op, "set-xattr",
                              movian_smb2_xattr_set_submit,
                              SMB2_DEFAULT_TIMEOUT,
                              reason, sizeof(reason));
  if(rc < 0) {
    SMB2TRACE("set_xattr failed url=%s reason=%s", url, reason);
    movian_smb2_session_error(session, rc);
    if(!o->op.parked)
      free(o);
    movian_smb2_session_release(session);
    movian_smb2_target_fini(&target);
    return FAP_ERROR;
  }

  uint32_t status =
    o->create_status != 0 ? o->create_status : o->set_status;
  free(o);
  movian_smb2_session_release(session);
  movian_smb2_target_fini(&target);

  fa_err_code_t result = movian_smb2_xattr_status_to_fap(status);
  SMB2TRACE("set_xattr done url=%s name=%s status=0x%x result=%d",
            url, name, status, result);
  return result;
}


typedef struct {
  movian_smb2_op_t op;
  uint32_t create_status;      /* raw NT statuses from the compound */
  uint32_t query_status;
  uint32_t value_len;
  uint8_t value[MOVIAN_SMB2_EA_OUTPUT_MAX];  /* copied by the owner thread */
  const char *path;            /* into data[] */
  /* FILE_GET_EA_INFORMATION (MS-FSCC 2.4.15.1) input buffer: a single
   * requested name -- NextEntryOffset(4)=0, EaNameLength(1), EaName, NUL. */
  uint8_t input_buf[4 + 1 + MOVIAN_SMB2_EA_NAME_MAX + 1];
  size_t input_len;
  char data[];                 /* path NUL */
} movian_smb2_xattr_get_op_t;


static void
movian_smb2_xattr_get_create_cb(struct smb2_context *smb2, int status,
                                void *command_data, void *cb_data)
{
  ((movian_smb2_xattr_get_op_t *)cb_data)->create_status = status;
}

static void
movian_smb2_xattr_get_query_cb(struct smb2_context *smb2, int status,
                               void *command_data, void *cb_data)
{
  movian_smb2_xattr_get_op_t *o = cb_data;

  o->query_status = status;
  if(status == 0 && command_data != NULL) {
    struct smb2_query_info_reply *rep = command_data;
    if(rep->output_buffer != NULL) {
      /* Copy the (bounded) reply into the op blob and free the context
         allocation right here on the owner thread; the VFS thread never
         touches context-owned data. */
      uint32_t len = rep->output_buffer_length;
      if(len > sizeof(o->value))
        len = sizeof(o->value);
      memcpy(o->value, rep->output_buffer, len);
      o->value_len = len;
      smb2_free_data(smb2, rep->output_buffer);
      rep->output_buffer = NULL;
    }
  }
}

static void
movian_smb2_xattr_get_close_cb(struct smb2_context *smb2, int status,
                               void *command_data, void *cb_data)
{
  /* Last command in the compound: the op is complete. The verdict lives in
     create_status/query_status; the op itself succeeded as an exchange. */
  movian_smb2_op_completed(cb_data, 0);
}


static int
movian_smb2_xattr_get_submit(movian_smb2_session_t *session,
                             movian_smb2_op_t *op)
{
  movian_smb2_xattr_get_op_t *o = (movian_smb2_xattr_get_op_t *)op;
  struct smb2_context *smb2 = session->smb2;

  struct smb2_create_request cr_req = {};
  cr_req.requested_oplock_level = SMB2_OPLOCK_LEVEL_NONE;
  cr_req.impersonation_level = SMB2_IMPERSONATION_IMPERSONATION;
  cr_req.desired_access = SMB2_FILE_READ_EA;
  cr_req.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE;
  cr_req.create_disposition = SMB2_FILE_OPEN;
  cr_req.create_options = SMB2_FILE_NON_DIRECTORY_FILE;
  cr_req.name = o->path;

  struct smb2_query_info_request qi_req = {};
  qi_req.info_type = SMB2_0_INFO_FILE;
  qi_req.file_info_class = SMB2_FILE_FULL_EA_INFORMATION;
  qi_req.output_buffer_length = MOVIAN_SMB2_EA_OUTPUT_MAX;
  qi_req.input_buffer = o->input_buf;
  qi_req.input_buffer_length = o->input_len;
  memcpy(qi_req.file_id, compound_file_id, SMB2_FD_SIZE);

  struct smb2_close_request cl_req = {};
  memcpy(cl_req.file_id, compound_file_id, SMB2_FD_SIZE);

  struct smb2_pdu *pdu =
    smb2_cmd_create_async(smb2, &cr_req, movian_smb2_xattr_get_create_cb, op);
  struct smb2_pdu *next_pdu = pdu == NULL ? NULL :
    smb2_cmd_query_info_async(smb2, &qi_req, movian_smb2_xattr_get_query_cb,
                              op);
  if(next_pdu != NULL)
    smb2_add_compound_pdu(smb2, pdu, next_pdu);

  struct smb2_pdu *close_pdu = next_pdu == NULL ? NULL :
    smb2_cmd_close_async(smb2, &cl_req, movian_smb2_xattr_get_close_cb, op);
  if(close_pdu != NULL)
    smb2_add_compound_pdu(smb2, pdu, close_pdu);

  if(pdu == NULL || next_pdu == NULL || close_pdu == NULL) {
    if(pdu != NULL)
      smb2_free_pdu(smb2, pdu);
    return -ENOMEM;
  }
  smb2_queue_pdu(smb2, pdu);
  return 0;
}


static fa_err_code_t
movian_smb2_get_xattr(struct fa_protocol *fap, const char *url,
                      const char *name, void **datap, size_t *lenp)
{
  movian_smb2_target_t target = {};
  char errbuf[256];

  *datap = NULL;
  *lenp = 0;

  if(movian_smb2_target_parse(url, &target, errbuf, sizeof(errbuf))) {
    SMB2TRACE("get_xattr parse failed url=%s reason=%s", url, errbuf);
    return FAP_ERROR;
  }

  if(target.share == NULL || target.share[0] == '\0' || target.path[0] == '\0') {
    movian_smb2_target_fini(&target);
    return FAP_ERROR;
  }

  size_t name_len = strlen(name);
  if(name_len > MOVIAN_SMB2_EA_NAME_MAX) {
    movian_smb2_target_fini(&target);
    return FAP_ERROR;
  }

  SMB2TRACE("get_xattr url=%s name=%s", url, name);

  movian_smb2_session_t *session =
    movian_smb2_session_acquire(&target, target.share, FA_NON_INTERACTIVE,
                                SMB2_DEFAULT_TIMEOUT, NULL, errbuf,
                                sizeof(errbuf));
  if(session == NULL) {
    SMB2TRACE("get_xattr connect failed url=%s reason=%s", url, errbuf);
    movian_smb2_target_fini(&target);
    return FAP_ERROR;
  }

  size_t path_len = strlen(target.path);
  movian_smb2_xattr_get_op_t *o = calloc(1, sizeof(*o) + path_len + 1);
  if(o == NULL) {
    movian_smb2_session_release(session);
    movian_smb2_target_fini(&target);
    return FAP_ERROR;
  }
  memcpy(o->data, target.path, path_len + 1);
  o->path = o->data;
  o->input_len = 4 + 1 + name_len + 1;
  o->input_buf[4] = (uint8_t)name_len;
  memcpy(o->input_buf + 5, name, name_len);

  char reason[168];
  int rc = movian_smb2_op_run(session, &o->op, "get-xattr",
                              movian_smb2_xattr_get_submit,
                              SMB2_DEFAULT_TIMEOUT,
                              reason, sizeof(reason));
  if(rc < 0) {
    SMB2TRACE("get_xattr failed url=%s reason=%s", url, reason);
    movian_smb2_session_error(session, rc);
    if(!o->op.parked)
      free(o);
    movian_smb2_session_release(session);
    movian_smb2_target_fini(&target);
    return FAP_ERROR;
  }

  fa_err_code_t result;
  if(o->create_status != 0) {
    result = FAP_ERROR;
  } else if(o->query_status == SMB2_STATUS_NONEXISTENT_EA_ENTRY ||
            o->query_status == SMB2_STATUS_NO_EAS_ON_FILE) {
    /* No such EA on this file. Native's SMB1 QUERY_EAS_FROM_LIST always
       succeeds here with a zero-length value; mirror that instead of
       surfacing an error so a first-ever kvstore read isn't treated as a
       failure. */
    result = FAP_OK;
  } else if(o->query_status != 0) {
    result = FAP_ERROR;
  } else {
    const uint8_t *buf = o->value;
    uint32_t buflen = o->value_len;

    /* FILE_FULL_EA_INFORMATION (MS-FSCC 2.4.15) single entry: 8-byte fixed
     * header (NextEntryOffset, Flags, EaNameLength, EaValueLength) followed
     * by EaName, a NUL, then EaValue. */
    if(buflen < 8) {
      result = FAP_OK; /* empty/missing value, same as native */
    } else {
      uint8_t ea_name_length = buf[5];
      uint16_t ea_value_length = (uint16_t)buf[6] | ((uint16_t)buf[7] << 8);
      uint64_t value_offset = 8u + (uint64_t)ea_name_length + 1u;

      result = FAP_OK;
      if(ea_value_length > 0 && value_offset + ea_value_length <= buflen) {
        void *value = malloc(ea_value_length);
        if(value == NULL) {
          result = FAP_ERROR;
        } else {
          memcpy(value, buf + value_offset, ea_value_length);
          *datap = value;
          *lenp = ea_value_length;
        }
      }
    }
  }

  free(o);
  movian_smb2_session_release(session);
  movian_smb2_target_fini(&target);

  SMB2TRACE("get_xattr done url=%s name=%s result=%d len=%zu",
            url, name, result, *lenp);
  return result;
}


static int
movian_smb2_no_parking(fa_handle_t *fh)
{
  return 1;
}


static void
movian_smb2_init(void)
{
  movian_smb2_pool_init();
}


static fa_protocol_t fa_protocol_smb2 = {
  .fap_flags = FAP_INCLUDE_PROTO_IN_URL | FAP_ALLOW_CACHE,
  .fap_name = "smb2",
  .fap_init = movian_smb2_init,
  .fap_scan = movian_smb2_scan,
  .fap_open = movian_smb2_open,
  .fap_close = movian_smb2_close,
  .fap_read = movian_smb2_read,
  .fap_write = movian_smb2_write,
  .fap_seek = movian_smb2_seek,
  .fap_fsize = movian_smb2_fsize,
  .fap_ftruncate = movian_smb2_ftruncate,
  .fap_stat = movian_smb2_stat,
  .fap_unlink = movian_smb2_unlink,
  .fap_rmdir = movian_smb2_rmdir,
  .fap_rename = movian_smb2_rename,
  .fap_makedir = movian_smb2_makedir,
  .fap_fsinfo = movian_smb2_fsinfo,
  .fap_set_read_timeout = movian_smb2_set_read_timeout,
  .fap_set_xattr = movian_smb2_set_xattr,
  .fap_get_xattr = movian_smb2_get_xattr,
  .fap_no_parking = movian_smb2_no_parking,
  .fap_redirect = movian_smb2_redirect,
  .fap_normalize = movian_smb2_normalize,
};

FAP_REGISTER(smb2);
