/*
 *  Copyright (C) 2026 Movian contributors
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */
#ifndef FA_LIBSMB2_POOL_H__
#define FA_LIBSMB2_POOL_H__

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#include "arch/atomic.h"
#include "fileaccess/fa_proto.h"
#include "misc/callout.h"
#include "misc/queue.h"

/*
 * Tunables shared between the SMB2 fileaccess backend (fa_libsmb2.c) and the
 * pooled-session layer (fa_libsmb2_pool.c).
 */
#define SMB2_DEFAULT_TIMEOUT 15

/*
 * Connection pool limits.
 *
 * libsmb2 is not thread safe and binds a context to a single TCP socket, so a
 * session cannot be shared between threads without serialization. We keep one
 * long lived session per (server, share, user, domain) key in a pool so the
 * TCP+negotiate+session-setup+tree-connect handshake happens once and is reused
 * across the many VFS calls Movian issues while browsing a share. The cifs
 * native backend achieves the same with cifs_connections/cc_refcount.
 *
 * Idle sessions are kept around for a while so that bouncing back into a share
 * does not pay the handshake cost again, up to SMB2_POOL_MAX_SESSIONS. Older
 * sessions are reaped first.
 */
#define SMB2_POOL_MAX_SESSIONS 8
#define SMB2_POOL_IDLE_TTL_SEC 60

/*
 * Keepalive cadence (seconds) and how many consecutive missed echoes are
 * tolerated before the session is torn down. Mirrors the cifs native
 * SMB_ECHO_INTERVAL / cc_wait_for_ping scheme.
 */
#define SMB2_ECHO_INTERVAL 30
#define SMB2_ECHO_MAX_MISSED 2

#define SMB2TRACE(x, ...) do {                                 \
    if(gconf.enable_smb_debug)                                 \
      tracelog(0, TRACE_DEBUG, "SMB2-CLIENT", x, ##__VA_ARGS__); \
  } while(0)


/*
 * Parsed components of an smb2:// URL.
 */
typedef struct {
  char *server;
  char *share;
  char *path;
  char *user;
  char *domain;
} movian_smb2_target_t;

typedef struct {
  char *user;
  char *password;
  char *domain;
} movian_smb2_credentials_t;

/*
 * Per-operation state blobs parked on a session because a queued libsmb2 PDU
 * may still hold a callback pointer into them; freed by the pool after
 * smb2_destroy_context() has failed every pending callback.
 */
typedef struct movian_smb2_deferred {
  struct movian_smb2_deferred *next;
  void *ptr;
} movian_smb2_deferred_t;


struct movian_smb2_session;

/*
 * Async operation descriptor.
 *
 * libsmb2 contexts are single threaded, so after a session is connected only
 * its I/O owner thread may touch the smb2_context (build requests, run
 * smb2_service(), read smb2_get_error(), free context-owned data). VFS
 * threads describe the operation in one of these descriptors, enqueue it with
 * movian_smb2_op_run() and block on the session's completion condvar; the
 * owner thread invokes op->submit (which calls the matching smb2_*_async
 * builder) and later the libsmb2 completion callback resolves the op.
 *
 * The descriptor must be the FIRST member of a heap-allocated per-op blob so
 * that, when a waiter abandons a timed-out op whose PDU is still in flight,
 * the whole blob can be parked on the session's deferred-free list in one go
 * (the queued PDU keeps callback pointers into it).
 */
typedef struct movian_smb2_op movian_smb2_op_t;

/*
 * Runs on the session's I/O owner thread. Build and queue the libsmb2 async
 * request for this op. Return 0 when the request was queued (a libsmb2
 * callback will complete the op later), or a negative errno on immediate
 * failure (the op is then completed with that status and the context's
 * error string).
 */
typedef int (movian_smb2_op_submit_fn_t)(struct movian_smb2_session *session,
                                         movian_smb2_op_t *op);

typedef enum {
  MOVIAN_SMB2_OP_QUEUED = 0,
  MOVIAN_SMB2_OP_INFLIGHT,
  MOVIAN_SMB2_OP_DONE,
} movian_smb2_op_state_t;

struct movian_smb2_op {
  TAILQ_ENTRY(movian_smb2_op) link;
  struct movian_smb2_session *session;
  movian_smb2_op_submit_fn_t *submit;
  /*
   * Optional. Called (owner thread, queue mutex held) when the op completes
   * after having been abandoned by its waiter, so results built by the
   * completion callback that nobody will consume (e.g. a scandir entry
   * vector) can be released. The blob itself is already parked on the
   * session's deferred list and must not be freed here.
   */
  void (*abandoned_fini)(movian_smb2_op_t *op);
  const char *name;             /* static string, for tracing */
  movian_smb2_op_state_t state;
  int abandoned;                /* waiter gave up; do not publish results */
  int parked;                   /* blob ownership moved to deferred list */
  int status;                   /* completion status, libsmb2 conventions */
  char errmsg[168];             /* smb2_get_error() copied by owner thread */
};

TAILQ_HEAD(movian_smb2_op_queue, movian_smb2_op);

/*
 * A pooled, reusable libsmb2 session: one TCP connection + negotiated session
 * + tree connect to a single share, kept alive across VFS calls and re-used by
 * refcount. All context I/O is funneled through the per-session owner thread
 * (io_thread) via the submit queue.
 */
typedef struct movian_smb2_session {
  char *key;
  char *server;
  char *share;
  char *user;
  char *domain;
  struct smb2_context *smb2;
  atomic_t lifetime_refcount;
  int refcount;
  int broken;
  int linked;
  int64_t last_used;
  callout_t keepalive;
  int echo_missed;
  movian_smb2_deferred_t *deferred;
  LIST_ENTRY(movian_smb2_session) link;

  /*
   * I/O owner thread state. submit_queue/inflight_queue, the io_* flags and
   * the deferred list are protected by q_mutex; q_cond signals op
   * completion. wakeup_fd is a self-pipe that pulls the owner thread out of
   * poll() when ops are enqueued or the session is shut down.
   */
  hts_mutex_t q_mutex;
  hts_cond_t q_cond;
  struct movian_smb2_op_queue submit_queue;
  struct movian_smb2_op_queue inflight_queue;
  hts_thread_t io_thread;
  uint8_t io_thread_running;
  uint8_t io_shutdown;          /* ask the owner thread to exit */
  uint8_t io_dead;              /* transport failed; no more context I/O */
  uint8_t io_servicing;         /* owner thread is inside smb2_service():
                                  * a PDU callback may be writing into a
                                  * caller buffer right now. op_run's
                                  * in-flight-timeout path waits for this to
                                  * clear before returning, so the caller
                                  * cannot free/reuse its buffer while a
                                  * service call is still touching it. */
  int wakeup_fd[2];
  /* Cached at connect time so VFS threads never query the context. */
  uint32_t max_read_size;
  uint32_t max_write_size;
} movian_smb2_session_t;


void movian_smb2_pool_init(void);

void movian_smb2_target_fini(movian_smb2_target_t *target);

int movian_smb2_target_parse(const char *url, movian_smb2_target_t *target,
                             char *errbuf, size_t errlen);

/*
 * Acquire a pooled session for (target, share). On a cache miss the TCP +
 * negotiate + session setup + tree connect handshake runs once; subsequent
 * acquisitions reuse the live session. Returns a session with refcount bumped,
 * or NULL on failure (errbuf filled).
 *
 * The returned session has its pool refcount incremented; the caller must pair
 * this with movian_smb2_session_release(). The session's I/O owner thread is
 * running by the time this returns: perform operations by submitting op
 * descriptors with movian_smb2_op_run(), never by calling libsmb2 directly.
 *
 * If auth_needed is non-NULL it is set to 1 when the failure was an auth error
 * the caller may want to surface as FAP_NEED_AUTH.
 */
movian_smb2_session_t *
movian_smb2_session_acquire(const movian_smb2_target_t *target,
                            const char *share, int flags, int timeout,
                            int *auth_needed, char *errbuf, size_t errlen);

void movian_smb2_session_release(movian_smb2_session_t *session);

/*
 * Submit op to the session's owner thread and wait for its completion.
 * name is a static string used for tracing; submit builds the libsmb2
 * request on the owner thread. timeout_sec is this waiter's own deadline
 * (0 = wait forever); it does not affect other in-flight operations on the
 * same session.
 *
 * Returns the op's completion status (libsmb2 conventions: >= 0 success,
 * negative errno on failure) and copies the owner-thread-captured error
 * string into errbuf on failure. -ETIMEDOUT means this waiter's deadline
 * expired; when that happens with the request already in flight the session
 * is invalidated (a PDU with pointers into the op blob is stuck in the
 * context) and the blob is parked on the session's deferred-free list.
 *
 * Ownership: if op->parked is set when this returns, the blob now belongs
 * to the session and the CALLER MUST NOT free or reuse it; results inside
 * it are invalid. Otherwise the caller retains ownership as usual.
 */
int movian_smb2_op_run(movian_smb2_session_t *session, movian_smb2_op_t *op,
                       const char *name, movian_smb2_op_submit_fn_t *submit,
                       int timeout_sec, char *errbuf, size_t errlen);

/*
 * Resolve an op from a libsmb2 completion callback (owner thread only):
 * records status, captures smb2_get_error() into the descriptor and wakes
 * the waiter (or runs abandoned_fini when the waiter has given up).
 */
void movian_smb2_op_completed(movian_smb2_op_t *op, int status);

/*
 * Generic libsmb2 completion callback for ops whose only result is the
 * status code (close/unlink/rmdir/mkdir/rename/ftruncate/echo and the
 * fstat/stat/statvfs family, which fill a caller-provided struct). cb_data
 * must be the movian_smb2_op_t.
 */
void movian_smb2_op_cb(struct smb2_context *smb2, int status,
                       void *command_data, void *cb_data);

/*
 * Mark a session as broken under the pool lock and drop it from the pool so the
 * next acquisition reconnects. Called when an operation fails in a way that
 * implies the context is no longer usable.
 */
void movian_smb2_session_invalidate(movian_smb2_session_t *session);

/*
 * Classify a failed libsmb2 op status and drop the session from the pool if
 * the transport underneath it is suspect. Statuses that prove the server
 * replied (raw NT statuses, application-level errnos like ENOENT) leave the
 * session alone; anything else invalidates it so the pool never hands a dead
 * context to the next acquirer. Transport failures are additionally detected
 * by the owner thread itself (poll/smb2_service errors), which fails every
 * queued op; this classification is the op-level complement.
 */
void movian_smb2_session_error(movian_smb2_session_t *session, int status);

/*
 * Park a malloc'd per-operation state blob that a queued PDU callback may
 * still reference. The blob is freed when the session itself is freed, after
 * smb2_destroy_context() has failed every pending callback. Pair with
 * movian_smb2_session_invalidate() so the session (and with it the blob)
 * does not outlive its brokenness for long. movian_smb2_op_run() does this
 * automatically for timed-out in-flight ops.
 */
void movian_smb2_session_defer_free(movian_smb2_session_t *session, void *ptr);

#endif /* FA_LIBSMB2_POOL_H__ */
