/*
 *  Copyright (C) 2026 Movian contributors
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

/*
 * SMB2 pooled-session layer.
 *
 * Keeps one long lived libsmb2 context per (server, share, user, domain) so the
 * TCP+negotiate+session-setup+tree-connect handshake happens once and is reused
 * across the many VFS calls Movian issues while browsing a share. Sessions are
 * refcounted, reaped when idle past SMB2_POOL_IDLE_TTL_SEC, and probed with a
 * 30 s echo keepalive so a parked connection does not silently go stale. This
 * mirrors the cifs native cifs_connections / cc_refcount / cifs_periodic scheme.
 *
 * Concurrency model: libsmb2 contexts are single threaded, so each session
 * runs a dedicated I/O owner thread that exclusively drives its context. The
 * VFS backend (fa_libsmb2.c) submits async op descriptors through
 * movian_smb2_op_run() and blocks on a condvar until the owner thread
 * completes them, so several VFS threads can have operations in flight on the
 * same session simultaneously (the same model as fa_nativesmb's per-connection
 * smb_dispatch thread). Waiter timeouts are per-op, not per-context.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#include "main.h"
#include "keyring.h"
#include "misc/rstr.h"
#include "networking/net.h"
#include "fileaccess/smb/nmb.h"

#include "fa_libsmb2_pool.h"


static LIST_HEAD(, movian_smb2_session) smb2_sessions =
  LIST_HEAD_INITIALIZER(smb2_sessions);
static hts_mutex_t smb2_pool_mutex;
static hts_cond_t smb2_pool_cond;

/*
 * In-flight connects, keyed like the pool. A second acquirer for the same key
 * waits on smb2_pool_cond for the first handshake to settle instead of racing
 * a duplicate TCP+auth+tree connect that would immediately be torn down.
 */
typedef struct movian_smb2_connecting {
  LIST_ENTRY(movian_smb2_connecting) link;
  const char *key;
} movian_smb2_connecting_t;

static LIST_HEAD(, movian_smb2_connecting) smb2_connecting_list =
  LIST_HEAD_INITIALIZER(smb2_connecting_list);

/*
 * One long-lived context for smb2_parse_url(), shared under its own mutex.
 * smb2_init_context() is far too heavy to run per parse (multi-KB calloc, a
 * getlogin_r() syscall and a process-global srandom() reseed) and
 * movian_smb2_target_parse() runs on every VFS operation.
 */
static hts_mutex_t smb2_parser_mutex;
static struct smb2_context *smb2_parser_ctx;

static void movian_smb2_keepalive_cb(callout_t *c, void *opaque);


/*
 * URL parsing.
 *
 * libsmb2's URL parser expects an smb:// scheme, so we temporarily rewrite the
 * smb2:// prefix and parse the share/path/user/domain out of the result.
 */
int
movian_smb2_target_parse(const char *url, movian_smb2_target_t *target,
                         char *errbuf, size_t errlen)
{
  if(strncmp(url, "smb2://", 7)) {
    snprintf(errbuf, errlen, "Invalid SMB2 URL");
    return -1;
  }

  size_t parser_url_len = strlen(url);
  char *parser_url = malloc(parser_url_len);
  if(parser_url == NULL) {
    snprintf(errbuf, errlen, "Out of memory while parsing SMB2 URL");
    return -1;
  }
  snprintf(parser_url, parser_url_len, "smb://%s", url + 7);

  hts_mutex_lock(&smb2_parser_mutex);
  if(smb2_parser_ctx == NULL)
    smb2_parser_ctx = smb2_init_context();
  if(smb2_parser_ctx == NULL) {
    hts_mutex_unlock(&smb2_parser_mutex);
    snprintf(errbuf, errlen, "Unable to initialize SMB2");
    free(parser_url);
    return -1;
  }

  struct smb2_url *parsed = smb2_parse_url(smb2_parser_ctx, parser_url);
  free(parser_url);
  if(parsed == NULL || parsed->server == NULL || parsed->server[0] == '\0') {
    snprintf(errbuf, errlen, "Invalid SMB2 URL: %s",
             smb2_get_error(smb2_parser_ctx));
    if(parsed != NULL)
      smb2_destroy_url(parsed);
    hts_mutex_unlock(&smb2_parser_mutex);
    return -1;
  }

  int has_share = parsed->share != NULL;
  int has_user = parsed->user != NULL;
  int has_domain = parsed->domain != NULL;

  target->server = strdup(parsed->server);
  target->share = has_share ? strdup(parsed->share) : NULL;
  target->path = parsed->path != NULL ? strdup(parsed->path) : strdup("");
  target->user = has_user ? strdup(parsed->user) : NULL;
  target->domain = has_domain ? strdup(parsed->domain) : NULL;

  smb2_destroy_url(parsed);
  hts_mutex_unlock(&smb2_parser_mutex);

  if(target->server == NULL || target->path == NULL ||
     (has_share && target->share == NULL) ||
     (has_user && target->user == NULL) ||
     (has_domain && target->domain == NULL)) {
    snprintf(errbuf, errlen, "Out of memory while parsing SMB2 URL");
    movian_smb2_target_fini(target);
    memset(target, 0, sizeof(*target));
    return -1;
  }
  return 0;
}


void
movian_smb2_target_fini(movian_smb2_target_t *target)
{
  free(target->server);
  free(target->share);
  free(target->path);
  free(target->user);
  free(target->domain);
}


/*
 * Credential resolution + connect.
 *
 * The keyring prompt can block the UI, so the connect runs without the pool
 * lock held; a miss then inserts the freshly built session under the lock.
 */

static void
movian_smb2_credentials_fini(movian_smb2_credentials_t *credentials)
{
  free(credentials->user);
  free(credentials->password);
  free(credentials->domain);
}


static char *
movian_smb2_keyring_id(const movian_smb2_target_t *target)
{
  size_t len = strlen(target->server) + 32;

  if(target->user != NULL)
    len += strlen(target->user);
  if(target->domain != NULL)
    len += strlen(target->domain);

  char *id = malloc(len);
  if(id == NULL)
    return NULL;

  if(target->user != NULL) {
    snprintf(id, len, "smb2:connection:%s%s%s@%s",
             target->domain != NULL ? target->domain : "",
             target->domain != NULL ? ";" : "",
             target->user, target->server);
  } else {
    snprintf(id, len, "smb2:connection:%s", target->server);
  }
  return id;
}


static void
movian_smb2_default_credentials(const movian_smb2_target_t *target,
                                movian_smb2_credentials_t *credentials)
{
  credentials->user = strdup(target->user != NULL ? target->user : "guest");
  credentials->password = NULL;
  credentials->domain =
    strdup(target->domain != NULL ? target->domain : "WORKGROUP");
}


static int
movian_smb2_apply_target_identity(const movian_smb2_target_t *target,
                                  movian_smb2_credentials_t *credentials,
                                  char *errbuf, size_t errlen)
{
  if(target->user != NULL) {
    char *user = strdup(target->user);
    if(user == NULL) {
      snprintf(errbuf, errlen, "Out of memory while preparing SMB2 login");
      return -1;
    }
    free(credentials->user);
    credentials->user = user;
  }

  if(target->domain != NULL) {
    char *domain = strdup(target->domain);
    if(domain == NULL) {
      snprintf(errbuf, errlen, "Out of memory while preparing SMB2 login");
      return -1;
    }
    free(credentials->domain);
    credentials->domain = domain;
  }
  return 0;
}


static int
movian_smb2_is_auth_error(int status, const char *reason)
{
  if(status == -EACCES || status == -EPERM)
    return 1;
  if(reason == NULL)
    return 0;
  return strstr(reason, "STATUS_LOGON_FAILURE") != NULL ||
    strstr(reason, "STATUS_ACCESS_DENIED") != NULL ||
    strstr(reason, "STATUS_NETWORK_ACCESS_DENIED") != NULL ||
    strstr(reason, "STATUS_INVALID_PARAMETER") != NULL ||
    strstr(reason, "STATUS_INVALID_ACCOUNT_NAME") != NULL ||
    strstr(reason, "STATUS_WRONG_PASSWORD") != NULL ||
    strstr(reason, "STATUS_ACCOUNT_RESTRICTION") != NULL ||
    strstr(reason, "STATUS_INVALID_LOGON_HOURS") != NULL ||
    strstr(reason, "STATUS_PASSWORD_EXPIRED") != NULL ||
    strstr(reason, "STATUS_PASSWORD_MUST_CHANGE") != NULL ||
    strstr(reason, "STATUS_ACCOUNT_DISABLED") != NULL ||
    strstr(reason, "STATUS_ACCOUNT_EXPIRED") != NULL ||
    strstr(reason, "STATUS_ACCOUNT_LOCKED_OUT") != NULL ||
    strstr(reason, "STATUS_LOGON_NOT_GRANTED") != NULL ||
    strstr(reason, "STATUS_LOGON_TYPE_NOT_GRANTED") != NULL;
}


static int
movian_smb2_can_query_user(int flags)
{
  if(flags & (FA_NON_INTERACTIVE | FA_DISABLE_AUTH))
    return 0;

  char name[64];
  return strcmp(hts_thread_name(name, sizeof(name)), "asyncio");
}


static struct smb2_context *
movian_smb2_connect_once(const movian_smb2_target_t *target,
                         const char *share,
                         const movian_smb2_credentials_t *credentials,
                         int timeout, int *status,
                         char *errbuf, size_t errlen)
{
  const char *user =
    credentials->user != NULL && credentials->user[0] != '\0' ?
    credentials->user : "guest";
  struct smb2_context *smb2 = smb2_init_context();
  if(smb2 == NULL) {
    *status = -ENOMEM;
    snprintf(errbuf, errlen, "Unable to initialize SMB2");
    SMB2TRACE("Unable to initialize SMB2 context");
    return NULL;
  }

  SMB2TRACE("Connecting to %s/%s timeout=%d",
            target->server, share != NULL ? share : "", timeout);
  SMB2TRACE("SETUP %s:%s:%s", user,
            credentials->password != NULL ? "<set>" : "<unset>",
            credentials->domain != NULL ? credentials->domain : "<unset>");

  smb2_set_timeout(smb2, timeout);
  smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
  smb2_set_user(smb2, user);
  smb2_set_password(smb2, credentials->password);
  if(credentials->domain != NULL)
    smb2_set_domain(smb2, credentials->domain);

  /*
   * libsmb2 resolves target->server itself (getaddrinfo), which cannot see
   * NetBIOS-only names. For a dot-less name -- one DNS has no chance of
   * resolving -- pre-resolve it ourselves: try normal DNS first, then fall
   * back to an NBT name query. Either way we only ever hand libsmb2 an IP
   * string; the pool key, keyring id and all traces above/below keep using
   * the original target->server so a resolved address never leaks into
   * session identity.
   */
  const char *connect_server = target->server;
  net_addr_t resolved_addr = {0};
  char resolved_str[64];
  if(strchr(target->server, '.') == NULL) {
    const char *errmsg = NULL;
    if(!net_resolve(target->server, &resolved_addr, &errmsg)) {
      net_fmt_host(resolved_str, sizeof(resolved_str), &resolved_addr);
      connect_server = resolved_str;
      SMB2TRACE("Pre-resolved dot-less name '%s' via DNS -> %s",
                target->server, connect_server);
    } else if(!nmb_resolve(target->server, &resolved_addr)) {
      net_fmt_host(resolved_str, sizeof(resolved_str), &resolved_addr);
      connect_server = resolved_str;
      SMB2TRACE("Pre-resolved dot-less name '%s' via NBT -> %s",
                target->server, connect_server);
    } else {
      SMB2TRACE("Pre-resolve failed for dot-less name '%s' (DNS: %s; NBT: "
                "no answer) -- falling back to libsmb2's own resolution",
                target->server, errmsg != NULL ? errmsg : "unknown error");
    }
  }

  *status = smb2_connect_share(smb2, connect_server, share, user);
  if(*status == 0) {
    if(credentials->password != NULL && credentials->password[0] != '\0' &&
       (smb2_get_session_flags(smb2) & SMB2_SESSION_FLAG_IS_GUEST)) {
      SMB2TRACE("%s/%s Server ignored credentials and granted guest access "
                "-- treating as an authentication failure", target->server,
                share != NULL ? share : "");
      smb2_destroy_context(smb2);
      *status = -EACCES;
      snprintf(errbuf, errlen,
               "Server ignored credentials and granted guest access");
      return NULL;
    }
    SMB2TRACE("%s/%s Session setup", target->server,
              share != NULL ? share : "");
    return smb2;
  }

  snprintf(errbuf, errlen, "%s", smb2_get_error(smb2));
  SMB2TRACE("SETUP status=%d reason=%s", *status, errbuf);
  smb2_destroy_context(smb2);
  return NULL;
}


/*
 * Resolve credentials and perform the (interactive, possibly retrying) connect.
 * Runs without the pool lock so that a keyring prompt does not stall other
 * sessions. Returns a fresh connected context and, on success, leaves the
 * resolved credentials in *resolved for the session to retain.
 */
static struct smb2_context *
movian_smb2_connect_impl(const movian_smb2_target_t *target, const char *share,
                         int flags, int timeout, int *auth_needed,
                         movian_smb2_credentials_t *resolved,
                         char *errbuf, size_t errlen)
{
  movian_smb2_credentials_t credentials = {};
  char *keyring_id = movian_smb2_keyring_id(target);
  int status;

  if(auth_needed != NULL)
    *auth_needed = 0;

  if(keyring_id == NULL) {
    snprintf(errbuf, errlen, "Out of memory while preparing SMB2 login");
    return NULL;
  }

  int keyring_status =
    keyring_lookup(keyring_id, &credentials.user, &credentials.password,
                   &credentials.domain, NULL, NULL, NULL, 0);
  int have_saved_credentials = keyring_status == KEYRING_OK;
  SMB2TRACE("Keyring lookup %s for %s",
            have_saved_credentials ? "hit" : "miss", keyring_id);
  if(keyring_status != KEYRING_OK) {
    movian_smb2_default_credentials(target, &credentials);
  } else if(movian_smb2_apply_target_identity(target, &credentials,
                                               errbuf, errlen)) {
    movian_smb2_credentials_fini(&credentials);
    free(keyring_id);
    return NULL;
  }

  char reason[256] = {};
  struct smb2_context *smb2 =
    movian_smb2_connect_once(target, share, &credentials, timeout, &status,
                            reason, sizeof(reason));
  if(smb2 != NULL) {
    if(resolved != NULL) {
      resolved->user = strdup(credentials.user != NULL ? credentials.user : "");
      resolved->password =
        strdup(credentials.password != NULL ? credentials.password : "");
      resolved->domain =
        strdup(credentials.domain != NULL ? credentials.domain : "");
    }
    movian_smb2_credentials_fini(&credentials);
    free(keyring_id);
    return smb2;
  }

  int auth_error = movian_smb2_is_auth_error(status, reason);
  if(auth_error && auth_needed != NULL)
    *auth_needed = 1;

  int can_query_user = movian_smb2_can_query_user(flags);
  char thread_name[64];
  SMB2TRACE("Auth retry auth_error=%d can_query=%d flags=0x%x "
            "thread=%s status=%d reason=%s",
            auth_error, can_query_user, flags,
            hts_thread_name(thread_name, sizeof(thread_name)), status,
            reason);

  if(!can_query_user || !auth_error) {
    snprintf(errbuf, errlen, "Unable to connect to SMB2 share: %s", reason);
    movian_smb2_credentials_fini(&credentials);
    free(keyring_id);
    return NULL;
  }

  /*
   * Prompt retry loop: mirrors native's smb_setup_andX "goto again" -- keep
   * re-prompting with the latest failure reason until the user cancels, a
   * non-auth error occurs, or the connect succeeds. No iteration cap: native
   * is unbounded too, the keyring dialog blocks (so there is no spin risk),
   * and the cancel button is the escape hatch.
   */
  int show_reason = have_saved_credentials;
  for(;;) {
    movian_smb2_credentials_fini(&credentials);
    memset(&credentials, 0, sizeof(credentials));
    if(movian_smb2_apply_target_identity(target, &credentials,
                                         errbuf, errlen)) {
      movian_smb2_credentials_fini(&credentials);
      free(keyring_id);
      return NULL;
    }
    if(credentials.domain == NULL) {
      credentials.domain = strdup("WORKGROUP");
      if(credentials.domain == NULL) {
        snprintf(errbuf, errlen, "Out of memory while preparing SMB2 login");
        movian_smb2_credentials_fini(&credentials);
        free(keyring_id);
        return NULL;
      }
    }

    size_t source_len = strlen(target->server) + 16;
    char *source = malloc(source_len);
    if(source == NULL) {
      snprintf(errbuf, errlen, "Out of memory while preparing SMB2 login");
      movian_smb2_credentials_fini(&credentials);
      free(keyring_id);
      return NULL;
    }
    snprintf(source, source_len, "SMB2 server '%s'", target->server);

    keyring_status =
      keyring_lookup(keyring_id,
                     target->user != NULL ? NULL : &credentials.user,
                     &credentials.password,
                     target->domain != NULL ? NULL : &credentials.domain,
                     NULL, source,
                     show_reason ? reason : "Login required",
                     KEYRING_QUERY_USER | KEYRING_SHOW_REMEMBER_ME |
                     KEYRING_REMEMBER_ME_SET);
    free(source);

    if(keyring_status != KEYRING_OK) {
      SMB2TRACE("Credential query rejected status=%d", keyring_status);
      snprintf(errbuf, errlen, "Authentication rejected by user");
      movian_smb2_credentials_fini(&credentials);
      free(keyring_id);
      return NULL;
    }
    SMB2TRACE("Credential query accepted");

    smb2 = movian_smb2_connect_once(target, share, &credentials, timeout,
                                    &status, reason, sizeof(reason));
    if(smb2 != NULL) {
      if(resolved != NULL) {
        resolved->user =
          strdup(credentials.user != NULL ? credentials.user : "");
        resolved->password =
          strdup(credentials.password != NULL ? credentials.password : "");
        resolved->domain =
          strdup(credentials.domain != NULL ? credentials.domain : "");
      }
      movian_smb2_credentials_fini(&credentials);
      free(keyring_id);
      return smb2;
    }

    auth_error = movian_smb2_is_auth_error(status, reason);
    SMB2TRACE("Auth retry (prompted) auth_error=%d status=%d reason=%s",
              auth_error, status, reason);
    if(!auth_error) {
      snprintf(errbuf, errlen, "Unable to connect to SMB2 share: %s", reason);
      movian_smb2_credentials_fini(&credentials);
      free(keyring_id);
      return NULL;
    }

    /* Auth failed again: loop back and re-prompt with this failure reason. */
    show_reason = 1;
  }
}


/*
 * Pool state + lifecycle.
 */

void
movian_smb2_pool_init(void)
{
  hts_mutex_init(&smb2_pool_mutex);
  hts_cond_init(&smb2_pool_cond, &smb2_pool_mutex);
  hts_mutex_init(&smb2_parser_mutex);
}


/*
 * Per-session I/O owner thread + async op machinery.
 *
 * Ownership rule: after a session is connected, only its owner thread touches
 * the smb2_context -- request building (smb2_*_async), smb2_service(),
 * smb2_get_error(), freeing context-owned data. VFS threads describe work in
 * movian_smb2_op_t descriptors, enqueue them and block in
 * movian_smb2_op_run(); the owner loop polls the context socket plus a
 * self-pipe, drains the submit queue on wakeup, and libsmb2 completion
 * callbacks (running inside smb2_service on the owner thread) resolve ops.
 *
 * Transport death (poll/smb2_service failure, or a waiter abandoning an
 * in-flight op) fails every queued and in-flight op; in-flight blobs are
 * parked on the deferred list because the dead context still holds PDU
 * callback pointers into them. The context is only destroyed after the owner
 * thread has been joined, so parked blobs stay valid until
 * smb2_destroy_context() has failed every pending callback.
 */

static void
movian_smb2_deferred_push_locked(movian_smb2_session_t *session, void *ptr)
{
  movian_smb2_deferred_t *d = malloc(sizeof(*d));
  if(d == NULL)
    return;                     /* leak the blob rather than risk a UAF */
  d->ptr = ptr;
  d->next = session->deferred;
  session->deferred = d;
}


static void
movian_smb2_op_park_locked(movian_smb2_session_t *session,
                           movian_smb2_op_t *op)
{
  if(op->parked)
    return;
  op->parked = 1;
  movian_smb2_deferred_push_locked(session, op);
}


/*
 * Wake the owner thread out of poll(). The pipe is non-blocking; a full pipe
 * means a wakeup is already pending, which is just as good.
 */
static void
movian_smb2_io_wakeup(movian_smb2_session_t *session)
{
  uint8_t b = 0;
  if(write(session->wakeup_fd[1], &b, 1) < 0) {
    /* EAGAIN: wakeup already pending */
  }
}


/*
 * Fail every queued and in-flight op. Called with q_mutex held, on transport
 * death and on shutdown. Queued ops were never submitted to the context, so
 * their blobs stay caller-owned; in-flight ops have PDU callback pointers
 * into their blobs, so ownership moves to the deferred list (the waiter sees
 * op->parked and must not free).
 */
static void
movian_smb2_io_fail_all_locked(movian_smb2_session_t *session, int status,
                               const char *reason)
{
  movian_smb2_op_t *op;

  while((op = TAILQ_FIRST(&session->submit_queue)) != NULL) {
    TAILQ_REMOVE(&session->submit_queue, op, link);
    op->status = status;
    snprintf(op->errmsg, sizeof(op->errmsg), "%s", reason);
    op->state = MOVIAN_SMB2_OP_DONE;
  }

  while((op = TAILQ_FIRST(&session->inflight_queue)) != NULL) {
    TAILQ_REMOVE(&session->inflight_queue, op, link);
    op->status = status;
    snprintf(op->errmsg, sizeof(op->errmsg), "%s", reason);
    op->state = MOVIAN_SMB2_OP_DONE;
    if(op->abandoned) {
      if(op->abandoned_fini != NULL)
        op->abandoned_fini(op);
    } else {
      movian_smb2_op_park_locked(session, op);
    }
  }
  hts_cond_broadcast(&session->q_cond);
}


static void
movian_smb2_io_declare_dead_locked(movian_smb2_session_t *session,
                                   const char *reason)
{
  if(session->io_dead)
    return;
  session->io_dead = 1;
  SMB2TRACE("Session %s/%s transport dead: %s",
            session->server, session->share, reason);
  movian_smb2_io_fail_all_locked(session, -ENOTCONN, reason);
  movian_smb2_io_wakeup(session);
}


/*
 * The owner loop. Never takes the pool mutex (movian_smb2_session_free joins
 * this thread while holding it); pool-level invalidation on transport death
 * is done by the waiters when their ops fail.
 */
static void *
movian_smb2_io_thread_fn(void *aux)
{
  movian_smb2_session_t *session = aux;
  struct smb2_context *smb2 = session->smb2;

  SMB2TRACE("Session %s/%s I/O thread running",
            session->server, session->share);

  for(;;) {
    hts_mutex_lock(&session->q_mutex);

    if(session->io_shutdown) {
      movian_smb2_io_fail_all_locked(session, -ENOTCONN,
                                     "SMB2 session closed");
      hts_mutex_unlock(&session->q_mutex);
      break;
    }

    movian_smb2_op_t *op;
    while(!session->io_dead &&
          (op = TAILQ_FIRST(&session->submit_queue)) != NULL) {
      TAILQ_REMOVE(&session->submit_queue, op, link);
      op->state = MOVIAN_SMB2_OP_INFLIGHT;
      TAILQ_INSERT_TAIL(&session->inflight_queue, op, link);
      hts_mutex_unlock(&session->q_mutex);

      int rc = op->submit(session, op);

      hts_mutex_lock(&session->q_mutex);
      if(rc < 0 && op->state == MOVIAN_SMB2_OP_INFLIGHT) {
        /* Builder failed; no callback will ever fire. */
        TAILQ_REMOVE(&session->inflight_queue, op, link);
        op->status = rc;
        snprintf(op->errmsg, sizeof(op->errmsg), "%s", smb2_get_error(smb2));
        op->state = MOVIAN_SMB2_OP_DONE;
        if(!op->abandoned)
          hts_cond_broadcast(&session->q_cond);
      }
    }

    if(session->io_dead)
      movian_smb2_io_fail_all_locked(session, -ENOTCONN,
                                     "SMB2 transport is dead");

    int dead = session->io_dead;
    hts_mutex_unlock(&session->q_mutex);

    struct pollfd pfd[2];
    memset(pfd, 0, sizeof(pfd));
    pfd[0].fd = session->wakeup_fd[0];
    pfd[0].events = POLLIN;
    int nfds = 1;
    if(!dead) {
      pfd[1].fd = smb2_get_fd(smb2);
      pfd[1].events = smb2_which_events(smb2);
      nfds = 2;
    }

    int prc = poll(pfd, nfds, -1);
    if(prc < 0) {
      if(errno == EINTR)
        continue;
      hts_mutex_lock(&session->q_mutex);
      movian_smb2_io_declare_dead_locked(session, "SMB2 socket poll failed");
      hts_mutex_unlock(&session->q_mutex);
      continue;
    }

    if(pfd[0].revents & POLLIN) {
      uint8_t drain[64];
      while(read(session->wakeup_fd[0], drain, sizeof(drain)) ==
            (ssize_t)sizeof(drain)) {}
    }

    if(nfds == 2 && pfd[1].revents != 0) {
      /* Bracket the unlocked smb2_service() call with io_servicing so an
         in-flight-timeout waiter in op_run() can tell whether a PDU
         callback may still be writing into its (about-to-be-freed) caller
         buffer. smb2_service() fires completion callbacks that take
         q_mutex themselves, so it must run without the lock held.

         The io_dead re-check here is atomic with setting io_servicing:
         'dead' was sampled once at the top of this iteration, so a waiter
         could have declared the transport dead (and returned, freeing its
         buffer) after that sample but before this point. Re-checking under
         q_mutex makes the waiter's {set io_dead; check io_servicing} and
         this {check io_dead; set io_servicing} mutually exclusive: if the
         waiter won, we see io_dead and skip the service (no write into the
         freed buffer); if we won, the waiter sees io_servicing=1 and waits
         for the drain. */
      hts_mutex_lock(&session->q_mutex);
      if(session->io_dead) {
        hts_mutex_unlock(&session->q_mutex);
      } else {
        session->io_servicing = 1;
        hts_mutex_unlock(&session->q_mutex);

        int src = smb2_service(smb2, pfd[1].revents);
        char reason[164];
        if(src < 0)
          snprintf(reason, sizeof(reason), "%s", smb2_get_error(smb2));

        hts_mutex_lock(&session->q_mutex);
        session->io_servicing = 0;
        hts_cond_broadcast(&session->q_cond);
        if(src < 0)
          movian_smb2_io_declare_dead_locked(session, reason);
        hts_mutex_unlock(&session->q_mutex);
      }
    }
  }

  SMB2TRACE("Session %s/%s I/O thread exiting",
            session->server, session->share);
  return NULL;
}


void
movian_smb2_op_completed(movian_smb2_op_t *op, int status)
{
  movian_smb2_session_t *session = op->session;

  hts_mutex_lock(&session->q_mutex);
  if(op->state == MOVIAN_SMB2_OP_DONE) {
    /* Late completion (smb2_destroy_context failing pending callbacks) of
       an op that fail_all already resolved; the blob is parked, nothing
       left to do. */
    hts_mutex_unlock(&session->q_mutex);
    return;
  }
  if(op->state == MOVIAN_SMB2_OP_INFLIGHT)
    TAILQ_REMOVE(&session->inflight_queue, op, link);
  op->status = status;
  if(status < 0)
    snprintf(op->errmsg, sizeof(op->errmsg), "%s",
             smb2_get_error(session->smb2));
  op->state = MOVIAN_SMB2_OP_DONE;
  if(op->abandoned) {
    if(op->abandoned_fini != NULL)
      op->abandoned_fini(op);
  } else {
    hts_cond_broadcast(&session->q_cond);
  }
  hts_mutex_unlock(&session->q_mutex);
}


void
movian_smb2_op_cb(struct smb2_context *smb2, int status,
                  void *command_data, void *cb_data)
{
  movian_smb2_op_completed(cb_data, status);
}


int
movian_smb2_op_run(movian_smb2_session_t *session, movian_smb2_op_t *op,
                   const char *name, movian_smb2_op_submit_fn_t *submit,
                   int timeout_sec, char *errbuf, size_t errlen)
{
  op->session = session;
  op->submit = submit;
  op->name = name;

  int64_t deadline = timeout_sec > 0 ?
    arch_get_ts() + (int64_t)timeout_sec * 1000000LL : 0;

  hts_mutex_lock(&session->q_mutex);
  if(session->io_dead || session->io_shutdown) {
    hts_mutex_unlock(&session->q_mutex);
    if(errbuf != NULL)
      snprintf(errbuf, errlen, "SMB2 session is disconnected");
    op->status = -ENOTCONN;
    movian_smb2_session_invalidate(session);
    return -ENOTCONN;
  }

  op->state = MOVIAN_SMB2_OP_QUEUED;
  TAILQ_INSERT_TAIL(&session->submit_queue, op, link);
  movian_smb2_io_wakeup(session);
  SMB2TRACE("op %s submit ts=%"PRId64" session=%s/%s",
            name, arch_get_ts(), session->server, session->share);

  while(op->state != MOVIAN_SMB2_OP_DONE) {
    if(deadline == 0) {
      hts_cond_wait(&session->q_cond, &session->q_mutex);
      continue;
    }
    if(!hts_cond_wait_timeout_abs(&session->q_cond, &session->q_mutex,
                                  deadline))
      continue;
    if(op->state == MOVIAN_SMB2_OP_DONE)
      break;

    /* This waiter's deadline expired. */
    if(op->state == MOVIAN_SMB2_OP_QUEUED) {
      /* The owner thread never touched the context for this op: take it
         back; the session itself is not implicated. */
      TAILQ_REMOVE(&session->submit_queue, op, link);
      op->state = MOVIAN_SMB2_OP_DONE;
      op->status = -ETIMEDOUT;
      hts_mutex_unlock(&session->q_mutex);
      if(errbuf != NULL)
        snprintf(errbuf, errlen, "SMB2 %s timed out", name);
      SMB2TRACE("op %s timed out while queued ts=%"PRId64" session=%s/%s",
                name, arch_get_ts(), session->server, session->share);
      return -ETIMEDOUT;
    }

    /* In flight: a PDU with callback pointers into *op is stuck in the
       context. Park the blob, declare the transport dead (same policy as
       an smb2_service failure) and drop the session from the pool. Other
       waiters on this session fail with their own explicit statuses. */
    op->abandoned = 1;
    movian_smb2_op_park_locked(session, op);
    movian_smb2_io_declare_dead_locked(session, "SMB2 operation timed out");
    /* io_dead now blocks any *new* smb2_service() call, but one may
       already be in progress with a PDU callback pointing into this op's
       caller-owned buffer (rbuf/wbuf). Wait for it to drain before
       returning: only then is it safe for the caller to free/reuse buf. */
    while(session->io_servicing)
      hts_cond_wait(&session->q_cond, &session->q_mutex);
    hts_mutex_unlock(&session->q_mutex);
    movian_smb2_session_invalidate(session);
    if(errbuf != NULL)
      snprintf(errbuf, errlen, "SMB2 %s timed out", name);
    SMB2TRACE("op %s timed out in flight after %d s ts=%"PRId64
              " session=%s/%s -> session dropped",
              name, timeout_sec, arch_get_ts(),
              session->server, session->share);
    return -ETIMEDOUT;
  }

  int status = op->status;
  if(status < 0 && errbuf != NULL)
    snprintf(errbuf, errlen, "%s", op->errmsg);
  hts_mutex_unlock(&session->q_mutex);
  SMB2TRACE("op %s done status=%d ts=%"PRId64" session=%s/%s",
            name, status, arch_get_ts(), session->server, session->share);
  return status;
}


static int
movian_smb2_session_key(const movian_smb2_target_t *target, const char *share,
                        char **keyp)
{
  const char *user = target->user != NULL ? target->user : "";
  const char *domain = target->domain != NULL ? target->domain : "";
  const char *sh = share != NULL ? share : "";

  size_t len = strlen(target->server) + strlen(sh) + strlen(user) +
    strlen(domain) + 4;
  char *key = malloc(len);
  if(key == NULL)
    return -1;

  snprintf(key, len, "%s/%s/%s/%s", target->server, sh, user, domain);
  *keyp = key;
  return 0;
}


static void
movian_smb2_session_free(movian_smb2_session_t *session)
{
  if(session->io_thread_running) {
    hts_mutex_lock(&session->q_mutex);
    session->io_shutdown = 1;
    movian_smb2_io_wakeup(session);
    hts_mutex_unlock(&session->q_mutex);
    hts_thread_join(&session->io_thread);
  }
  if(session->wakeup_fd[0] >= 0)
    close(session->wakeup_fd[0]);
  if(session->wakeup_fd[1] >= 0)
    close(session->wakeup_fd[1]);

  if(session->smb2 != NULL) {
    /* The owner thread is gone, so this thread owns the context again.
       Restore a finite transport timeout so a dead server cannot hang the
       disconnect round trip. */
    smb2_set_timeout(session->smb2, SMB2_DEFAULT_TIMEOUT);
    smb2_disconnect_share(session->smb2);
    smb2_destroy_context(session->smb2);
  }
  /* smb2_destroy_context() has failed every still-queued PDU callback
     (SMB2_STATUS_SHUTDOWN), so states parked by
     movian_smb2_session_defer_free() can no longer be referenced. */
  movian_smb2_deferred_t *d = session->deferred;
  while(d != NULL) {
    movian_smb2_deferred_t *next = d->next;
    free(d->ptr);
    free(d);
    d = next;
  }
  hts_cond_destroy(&session->q_cond);
  hts_mutex_destroy(&session->q_mutex);
  free(session->key);
  free(session->server);
  free(session->share);
  free(session->user);
  free(session->domain);
  free(session);
}


static void
movian_smb2_session_retain_lifetime(movian_smb2_session_t *session)
{
  atomic_inc(&session->lifetime_refcount);
}


static void
movian_smb2_session_release_lifetime(movian_smb2_session_t *session)
{
  if(atomic_dec(&session->lifetime_refcount))
    return;
  movian_smb2_session_free(session);
}


static int
movian_smb2_session_lockmgr(void *ptr, lockmgr_op_t op)
{
  movian_smb2_session_t *session = ptr;

  switch(op) {
  case LOCKMGR_UNLOCK:
  case LOCKMGR_LOCK:
  case LOCKMGR_TRY:
    return 0;
  case LOCKMGR_RETAIN:
    movian_smb2_session_retain_lifetime(session);
    return 0;
  case LOCKMGR_RELEASE:
    movian_smb2_session_release_lifetime(session);
    return 0;
  }
  abort();
}


static void
movian_smb2_session_destroy(movian_smb2_session_t *session)
{
  callout_disarm(&session->keepalive);
  movian_smb2_session_release_lifetime(session);
}


static int
movian_smb2_session_unlink_locked(movian_smb2_session_t *session)
{
  if(session->linked) {
    LIST_REMOVE(session, link);
    session->linked = 0;
    return 1;
  }
  return 0;
}


/*
 * Reap idle and broken sessions. Called under the pool mutex. The least
 * recently used idle session is dropped when the pool grows past the limit.
 */
static void
movian_smb2_pool_evict_locked(void)
{
  if(LIST_EMPTY(&smb2_sessions))
    return;

  int64_t now = arch_get_ts();
  int64_t cutoff = now - SMB2_POOL_IDLE_TTL_SEC * 1000000LL;

  movian_smb2_session_t *session = LIST_FIRST(&smb2_sessions);
  while(session != NULL) {
    movian_smb2_session_t *next = LIST_NEXT(session, link);
    if(session->refcount <= 0 &&
       (session->broken || session->last_used < cutoff)) {
      if(movian_smb2_session_unlink_locked(session))
        movian_smb2_session_destroy(session);
    }
    session = next;
  }

  int count = 0;
  LIST_FOREACH(session, &smb2_sessions, link)
    count++;

  while(count > SMB2_POOL_MAX_SESSIONS) {
    movian_smb2_session_t *oldest = NULL;
    LIST_FOREACH(session, &smb2_sessions, link) {
      if(session->refcount > 0)
        continue;
      if(oldest == NULL || session->last_used < oldest->last_used)
        oldest = session;
    }
    if(oldest == NULL)
      break;
    if(movian_smb2_session_unlink_locked(oldest))
      movian_smb2_session_destroy(oldest);
    count--;
  }
}


/*
 * Drop our in-flight connect marker and wake acquirers that were waiting for
 * this handshake to settle (they rescan the pool and either reuse the session
 * we inserted or start their own connect if ours failed).
 */
static void
movian_smb2_connecting_done(movian_smb2_connecting_t *pending)
{
  hts_mutex_lock(&smb2_pool_mutex);
  LIST_REMOVE(pending, link);
  hts_cond_broadcast(&smb2_pool_cond);
  hts_mutex_unlock(&smb2_pool_mutex);
}


movian_smb2_session_t *
movian_smb2_session_acquire(const movian_smb2_target_t *target,
                            const char *share, int flags, int timeout,
                            int *auth_needed, char *errbuf, size_t errlen)
{
  char *key;
  if(movian_smb2_session_key(target, share, &key)) {
    snprintf(errbuf, errlen, "Out of memory while opening SMB2 session");
    return NULL;
  }

  movian_smb2_session_t *session = NULL;
  movian_smb2_connecting_t pending = { .key = key };

  hts_mutex_lock(&smb2_pool_mutex);
  for(;;) {
    LIST_FOREACH(session, &smb2_sessions, link) {
      if(!strcmp(session->key, key) && !session->broken) {
        session->refcount++;
        session->last_used = arch_get_ts();
        hts_mutex_unlock(&smb2_pool_mutex);
        free(key);
        SMB2TRACE("Pool reuse %s/%s refcount=%d",
                  target->server, share != NULL ? share : "",
                  session->refcount);
        return session;
      }
    }
    movian_smb2_connecting_t *conn;
    LIST_FOREACH(conn, &smb2_connecting_list, link)
      if(!strcmp(conn->key, key))
        break;
    if(conn == NULL)
      break;
    /* Same-key handshake already in flight: wait for it instead of racing
       a duplicate connect that would immediately be torn down again. */
    hts_cond_wait(&smb2_pool_cond, &smb2_pool_mutex);
  }
  LIST_INSERT_HEAD(&smb2_connecting_list, &pending, link);
  hts_mutex_unlock(&smb2_pool_mutex);

  movian_smb2_credentials_t resolved = {};
  struct smb2_context *smb2 =
    movian_smb2_connect_impl(target, share, flags, timeout, auth_needed,
                             &resolved, errbuf, errlen);
  if(smb2 == NULL) {
    movian_smb2_connecting_done(&pending);
    free(key);
    return NULL;
  }

  session = calloc(1, sizeof(*session));
  if(session == NULL) {
    snprintf(errbuf, errlen, "Out of memory while opening SMB2 session");
    smb2_disconnect_share(smb2);
    smb2_destroy_context(smb2);
    movian_smb2_credentials_fini(&resolved);
    movian_smb2_connecting_done(&pending);
    free(key);
    return NULL;
  }

  hts_mutex_init(&session->q_mutex);
  hts_cond_init(&session->q_cond, &session->q_mutex);
  TAILQ_INIT(&session->submit_queue);
  TAILQ_INIT(&session->inflight_queue);
  session->wakeup_fd[0] = session->wakeup_fd[1] = -1;
  atomic_set(&session->lifetime_refcount, 1);
  session->key = key;
  session->smb2 = smb2;
  session->refcount = 1;
  session->last_used = arch_get_ts();
  session->server = strdup(target->server);
  session->share = strdup(share != NULL ? share : "");
  session->user = resolved.user;
  session->domain = resolved.domain;
  /* Cached so VFS threads never have to query the context. */
  session->max_read_size = smb2_get_max_read_size(smb2);
  session->max_write_size = smb2_get_max_write_size(smb2);
  resolved.user = NULL;
  resolved.domain = NULL;
  if(session->server == NULL || session->share == NULL ||
     pipe(session->wakeup_fd)) {
    snprintf(errbuf, errlen, "Unable to set up SMB2 session");
    movian_smb2_credentials_fini(&resolved);
    movian_smb2_connecting_done(&pending);
    movian_smb2_session_destroy(session);
    return NULL;
  }
  fcntl(session->wakeup_fd[0], F_SETFL,
        fcntl(session->wakeup_fd[0], F_GETFL) | O_NONBLOCK);
  fcntl(session->wakeup_fd[1], F_SETFL,
        fcntl(session->wakeup_fd[1], F_GETFL) | O_NONBLOCK);
  fcntl(session->wakeup_fd[0], F_SETFD, FD_CLOEXEC);
  fcntl(session->wakeup_fd[1], F_SETFD, FD_CLOEXEC);
  /*
   * resolved.password is intentionally not retained: the session never
   * re-authenticates (it is dropped when the server tears it down), and the
   * keyring still holds the credentials for the next acquisition.
   */
  free(resolved.password);
  resolved = (movian_smb2_credentials_t){};

  /*
   * Operation pacing is per waiter (movian_smb2_op_run deadlines), not per
   * context: disable libsmb2's internal PDU timeout so it cannot fail a
   * request some caller is deliberately waiting forever on
   * (fa_set_read_timeout(0)). Dead transports are detected by the owner
   * loop (poll/smb2_service errors) and by waiter deadlines.
   */
  smb2_set_timeout(smb2, 0);

  /* From this point the ownership rule applies: only the owner thread may
     touch session->smb2. */
  hts_thread_create_joinable("smb2io", &session->io_thread,
                             movian_smb2_io_thread_fn, session,
                             THREAD_PRIO_FILESYSTEM);
  session->io_thread_running = 1;

  hts_mutex_lock(&smb2_pool_mutex);
  /* The connecting gate guarantees no same-key session appeared meanwhile. */
  LIST_REMOVE(&pending, link);
  hts_cond_broadcast(&smb2_pool_cond);
  LIST_INSERT_HEAD(&smb2_sessions, session, link);
  session->linked = 1;
  movian_smb2_pool_evict_locked();
  callout_arm_managed(&session->keepalive, movian_smb2_keepalive_cb, session,
                      SMB2_ECHO_INTERVAL * 1000000LL,
                      movian_smb2_session_lockmgr);
  hts_mutex_unlock(&smb2_pool_mutex);

  SMB2TRACE("Pool create %s/%s refcount=1",
            target->server, share != NULL ? share : "");
  return session;
}


void
movian_smb2_session_release(movian_smb2_session_t *session)
{
  if(session == NULL)
    return;

  hts_mutex_lock(&smb2_pool_mutex);
  session->refcount--;
  session->last_used = arch_get_ts();
  SMB2TRACE("Pool release %s/%s refcount=%d",
            session->server, session->share, session->refcount);
  if(session->refcount <= 0)
    movian_smb2_pool_evict_locked();
  hts_mutex_unlock(&smb2_pool_mutex);
}


void
movian_smb2_session_invalidate(movian_smb2_session_t *session)
{
  if(session == NULL)
    return;

  hts_mutex_lock(&smb2_pool_mutex);
  session->broken = 1;
  if(session->refcount <= 0) {
    int unlinked = movian_smb2_session_unlink_locked(session);
    if(unlinked)
      movian_smb2_session_destroy(session);
  }
  hts_mutex_unlock(&smb2_pool_mutex);
}


void
movian_smb2_session_error(movian_smb2_session_t *session, int status)
{
  if(session == NULL || status >= 0)
    return;
  if(status < -4096)
    return;                     /* raw NT status: the server replied */

  switch(-status) {
  case ENOENT:
  case EEXIST:
  case EACCES:
  case EPERM:
  case ENOTDIR:
  case EISDIR:
  case ENOTEMPTY:
  case EINVAL:
  case ENOSPC:
  case EXDEV:
  case ENAMETOOLONG:
    return;                     /* application-level: the session is healthy */
  case ETIMEDOUT:
    /* op_run() owns the timeout->session policy: a queued timeout means the
       owner thread never touched the context (session left healthy in the
       pool), while an in-flight timeout already invalidated the session
       itself via movian_smb2_io_declare_dead_locked()+invalidate(). Either
       way this generic error path has nothing to add. */
    return;
  default:
    break;
  }

  /* Transport-suspect status (every op reports a real errno from its async
     completion callback, so -EPERM above is a genuine server verdict, not
     the legacy sync wrappers' ambiguous "-1"). The owner thread already
     failed every other queued op if the socket died; dropping the session
     from the pool is what is left to do. */
  SMB2TRACE("Session %s/%s failed op status=%d -> dropping from pool",
            session->server, session->share, status);
  movian_smb2_session_invalidate(session);
}


void
movian_smb2_session_defer_free(movian_smb2_session_t *session, void *ptr)
{
  hts_mutex_lock(&session->q_mutex);
  movian_smb2_deferred_push_locked(session, ptr);
  hts_mutex_unlock(&session->q_mutex);
}


/*
 * Echo keepalive op: the callout thread submits it through the owner thread
 * like any other operation instead of driving the context itself.
 */
static int
movian_smb2_echo_submit(movian_smb2_session_t *session, movian_smb2_op_t *op)
{
  return smb2_echo_async(session->smb2, movian_smb2_op_cb, op);
}


/*
 * Periodic keepalive. Runs on the callout thread. The managed callout holds a
 * lifetime ref while queued or running, so an idle-session eviction can unlink
 * and logically destroy the session without freeing memory out from under this
 * callback. Echoes only run for idle, refcount==0 sessions.
 *
 * The echo op blocks this callback (up to SMB2_DEFAULT_TIMEOUT) with the pool
 * mutex dropped, so a concurrent acquire()/evict_locked() can unlink (and
 * logically destroy) this very session while the echo is in flight. All
 * outcomes of that race are resolved in one place, at the end of the
 * callback, under the pool mutex: session->linked is the source of truth
 * for "someone else already tore this down", not session->broken alone.
 */
static void
movian_smb2_keepalive_cb(callout_t *c, void *opaque)
{
  movian_smb2_session_t *session = opaque;
  int do_echo = 0;
  int destroy = 0;

  hts_mutex_lock(&smb2_pool_mutex);
  do_echo = !session->broken && session->refcount <= 0;
  hts_mutex_unlock(&smb2_pool_mutex);

  int rc = 0;
  int missed = 0;
  if(do_echo) {
    movian_smb2_op_t *op = calloc(1, sizeof(*op));
    if(op != NULL) {
      rc = movian_smb2_op_run(session, op, "echo", movian_smb2_echo_submit,
                              SMB2_DEFAULT_TIMEOUT, NULL, 0);
      if(!op->parked)
        free(op);
    } else {
      rc = -ENOMEM;
    }
  }

  hts_mutex_lock(&smb2_pool_mutex);
  if(do_echo && !session->broken) {
    if(rc == 0)
      missed = session->echo_missed = 0;
    else
      missed = ++session->echo_missed;
    if(session->echo_missed > SMB2_ECHO_MAX_MISSED) {
      SMB2TRACE("Keepalive %s/%s giving up (missed=%d) -> broken",
                session->server, session->share, session->echo_missed);
      session->broken = 1;
    }
  }

  /* Single end-of-callback decision point (see block comment above). */
  if(!session->linked) {
    /* Unlinked (and logically destroyed) by someone else while the echo
     * was in flight: no re-arm, no extra release. The callout machinery
     * drops its own running ref once we return. */
  } else if(session->broken && session->refcount <= 0) {
    destroy = movian_smb2_session_unlink_locked(session);
  } else {
    callout_arm_managed(&session->keepalive, movian_smb2_keepalive_cb,
                        session, SMB2_ECHO_INTERVAL * 1000000LL,
                        movian_smb2_session_lockmgr);
  }
  hts_mutex_unlock(&smb2_pool_mutex);

  if(do_echo)
    SMB2TRACE("Keepalive %s/%s echo rc=%d missed=%d",
              session->server, session->share, rc, missed);

  if(destroy)
    movian_smb2_session_destroy(session);
}
