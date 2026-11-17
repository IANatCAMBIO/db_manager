/* ===========================================================================
 * service.c — the daemon (see service.h)
 * =========================================================================== */

#include "service.h"
#include "identity.h"
#include "look.h"

#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

struct cdb_service {
    cdb_service_hooks hooks;
    char             *path;          /* the open database file              */
    shared_db        *db;            /* the main connection                 */
    int               lock_fd;       /* the one-copy lock, or -1            */
    cdb_health        health;        /* the last verdict                    */
    bool              checking;      /* a check is on its worker            */
    bool              backing_up;    /* a backup pass is on its worker      */
    unsigned          probe_gen;     /* the probe whose answer is awaited   */
    bool              probing;
    int64_t           size;          /* the file's size, or -1              */
    char             *sha256;        /* its digest, or NULL                 */
    char             *status;        /* the latest one-line event           */
    cdb_peer         *peers;         /* who has the file open, last scan    */
    size_t            peer_count;
    bool              scanning;      /* a scan is on its worker             */
    dispatch_source_t backup_timer;
    dispatch_source_t check_timer;
    bool              stopped;       /* no hook is called again             */
    unsigned          refs;          /* the owner, plus one per pass in flight */
};

/* ── Lifetime ────────────────────────────────────────────────────────────── */

/*
 * release — drop one reference; the last frees the struct.  A pass in
 * flight holds one, so a service stopped under it is freed when the pass
 * hands back, not before.
 *
 * Inputs:
 *   s — the service
 */
static void
release(cdb_service *s)
{
    if (--s->refs > 0) {
        return;
    }
    cdb_health_clear(&s->health);
    free(s->path);
    free(s->sha256);
    free(s->status);
    free(s->peers);
    free(s);
}

/*
 * lock_take — take the one-copy lock beside the database: a file named
 * CDB_LOCK_FILENAME in the database's directory, flock()ed exclusively.
 *
 * Inputs:
 *   db_path — the database file
 *   err     — receives the reason on failure
 *
 * Output: the locked descriptor (closing it releases the lock), or -1.
 */
static int
lock_take(const char *db_path, char **err)
{
    const char *slash = strrchr(db_path, '/');
    char       *lock  = NULL;
    if (asprintf(&lock, "%.*s/%s", (int)(slash - db_path), db_path, CDB_LOCK_FILENAME) < 0) {
        *err = strdup("out of memory");
        return -1;
    }
    int fd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        asprintf(err, "cannot open the lock %s: %s", lock, strerror(errno));
        free(lock);
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        *err = strdup(errno == EWOULDBLOCK
                      ? "another copy of " CDB_APP_NAME " is already looking after this database"
                      : "cannot take the lock");
        close(fd);
        free(lock);
        return -1;
    }
    free(lock);
    return fd;
}

/*
 * post_status — keep and announce a one-line event.
 *
 * Inputs:
 *   s       — the service
 *   message — the line (copied)
 */
static void
post_status(cdb_service *s, const char *message)
{
    free(s->status);
    s->status = strdup(message);
    if (s->hooks.status != NULL) {
        s->hooks.status(s, message, s->hooks.ctx);
    }
}

/*
 * changed — tell the owner a shown fact moved.
 *
 * Inputs:
 *   s — the service
 */
static void
changed(cdb_service *s)
{
    if (s->hooks.state_changed != NULL) {
        s->hooks.state_changed(s, s->hooks.ctx);
    }
}

/* ── The probe and the check ─────────────────────────────────────────────── */

/*
 * A check or a probe: the file path copied for the worker, what it brings
 * back, and which probe generation it answers.  The worker never touches
 * the service.
 */
typedef struct {
    cdb_service *s;
    char        *path;
    bool         check;              /* run the integrity check first       */
    unsigned     gen;                /* the probe generation it answers     */
    cdb_health   verdict;
    int64_t      size;
    char        *sha256;
} probe_job;

/*
 * probe_landed — on the main queue: record what the worker found, unless
 * the service stopped meanwhile or a newer probe outdates this one's
 * size and digest, and report.
 *
 * Inputs:
 *   job — the probe
 */
static void
probe_landed(probe_job *job)
{
    cdb_service *s = job->s;
    if (!s->stopped) {
        if (job->check) {
            cdb_health_clear(&s->health);
            s->health     = job->verdict;
            job->verdict.detail = NULL;    /* handed over                   */
            s->checking   = false;
            if (s->health.ok) {
                post_status(s, "Database check passed");
            } else if (s->hooks.problem != NULL) {
                char *message = NULL;
                asprintf(&message, "%s\n\n%s",
                         s->health.ran ? "The database integrity check found issues:"
                                       : "The database integrity check could not be completed:",
                         s->health.detail != NULL ? s->health.detail : "no detail reported");
                s->hooks.problem(s, "Database Integrity Check", message, s->hooks.ctx);
                free(message);
            }
        }
        if (job->gen == s->probe_gen) {
            s->probing = false;
            s->size    = job->size;
            free(s->sha256);
            s->sha256   = job->sha256;
            job->sha256 = NULL;            /* handed over                   */
        }
        changed(s);
    }
    cdb_health_clear(&job->verdict);
    free(job->sha256);
    free(job->path);
    free(job);
    release(s);
}

/*
 * probe_start — start a probe, with or without the integrity check, on a
 * worker.  The check goes first so the size and digest describe the file
 * as the check left it (unchanged: its connection is read-only).
 *
 * Inputs:
 *   s     — the service
 *   check — also run the integrity check
 */
static void
probe_start(cdb_service *s, bool check)
{
    probe_job *job = calloc(1, sizeof *job);
    job->s     = s;
    job->path  = strdup(s->path);
    job->check = check;
    job->gen   = ++s->probe_gen;
    job->size  = -1;
    s->probing = true;
    if (check) {
        s->checking = true;
    }
    s->refs++;
    changed(s);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        if (job->check) {
            cdb_health_check_file(job->path, &job->verdict);
        }
        job->size   = cdb_file_size(job->path);
        job->sha256 = cdb_file_sha256(job->path);
        dispatch_async(dispatch_get_main_queue(), ^{ probe_landed(job); });
    });
}

/* ── Backups ─────────────────────────────────────────────────────────────── */

/*
 * A backup pass: the file path copied for the worker and what it says
 * when it is over.
 */
typedef struct {
    cdb_service *s;
    char        *path;
    bool         periodic;
    char        *message;            /* NULL: nothing to say                */
} backup_job;

/*
 * backup_landed — on the main queue: release the in-flight guard, report
 * the outcome, let the window repaint its backup line.
 *
 * Inputs:
 *   job — the pass
 */
static void
backup_landed(backup_job *job)
{
    cdb_service *s = job->s;
    if (!s->stopped) {
        s->backing_up = false;
        if (job->message != NULL) {
            post_status(s, job->message);
        }
        changed(s);
    }
    free(job->message);
    free(job->path);
    free(job);
    release(s);
}

/*
 * backup_launch — start one pass on a worker unless one is in flight or
 * there is no usable folder.
 *
 * Inputs:
 *   s        — the service
 *   periodic — the poll's due-check (silent when refused) or Back Up Now
 *              (the refusal is posted as a status line)
 */
static void
backup_launch(cdb_service *s, bool periodic)
{
    if (s->backing_up) {
        return;
    }
    char *why = NULL;                /* why there is no usable folder       */
    char *dir = shared_db_backup_dir(s->db, &why);
    if (dir == NULL) {
        if (!periodic) {
            post_status(s, why != NULL ? why : "no backup folder");
        }
        free(why);
        return;
    }
    free(dir);
    backup_job *job = calloc(1, sizeof *job);
    job->s        = s;
    job->path     = strdup(s->path);
    job->periodic = periodic;
    s->backing_up = true;
    s->refs++;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        cdb_backup_run(job->path, job->periodic, &job->message);
        dispatch_async(dispatch_get_main_queue(), ^{ backup_landed(job); });
    });
}

/* ── Who has the file open ───────────────────────────────────────────────── */

/*
 * A peer scan: the file path copied for the worker, and the list it
 * brings back.
 */
typedef struct {
    cdb_service *s;
    char        *path;
    cdb_peer    *peers;
    size_t       count;
} scan_job;

/*
 * scan_landed — on the main queue: take the list, unless the service
 * stopped meanwhile, and report.
 *
 * Inputs:
 *   job — the scan
 */
static void
scan_landed(scan_job *job)
{
    cdb_service *s = job->s;
    if (!s->stopped) {
        s->scanning = false;
        free(s->peers);
        s->peers      = job->peers;  /* handed over                         */
        s->peer_count = job->count;
        job->peers    = NULL;
        changed(s);
    }
    free(job->peers);
    free(job->path);
    free(job);
    release(s);
}

/*
 * cdb_service_scan_peers — see service.h.
 */
void
cdb_service_scan_peers(cdb_service *s)
{
    if (s->scanning) {
        return;
    }
    scan_job *job = calloc(1, sizeof *job);
    job->s      = s;
    job->path   = strdup(s->path);
    s->scanning = true;
    s->refs++;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        cdb_peers_read(job->path, &job->peers, &job->count);
        dispatch_async(dispatch_get_main_queue(), ^{ scan_landed(job); });
    });
}

/*
 * cdb_service_peers — see service.h.
 */
const cdb_peer *
cdb_service_peers(cdb_service *s, size_t *count)
{
    *count = s->peer_count;
    return s->peers;
}

/*
 * cdb_service_scanning — see service.h.
 */
bool
cdb_service_scanning(cdb_service *s)
{
    return s->scanning;
}

/* ── Timers ──────────────────────────────────────────────────────────────── */

/*
 * timer_arm — a repeating timer on the main queue.
 *
 * Inputs:
 *   first    — seconds until the first firing
 *   interval — seconds between firings
 *   handler  — what runs
 *
 * Output: the source, resumed; cancel and release it to stop it.
 */
static dispatch_source_t
timer_arm(double first, double interval, dispatch_block_t handler)
{
    dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                                 dispatch_get_main_queue());
    dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(first * NSEC_PER_SEC)),
                              (uint64_t)(interval * NSEC_PER_SEC), NSEC_PER_SEC);
    dispatch_source_set_event_handler(t, handler);
    dispatch_resume(t);
    return t;
}

/*
 * timer_stop — cancel a timer, if there is one.
 *
 * Inputs:
 *   timer — the source, or NULL
 */
static void
timer_stop(dispatch_source_t timer)
{
    if (timer != NULL) {
        dispatch_source_cancel(timer);
    }
}

/* ── Start and stop ──────────────────────────────────────────────────────── */

/*
 * cdb_service_start — see service.h.
 */
cdb_service *
cdb_service_start(const cdb_service_hooks *hooks, cdb_open_kind *kind, char **err)
{
    cdb_service *s = calloc(1, sizeof *s);
    s->hooks   = *hooks;
    s->lock_fd = -1;
    s->size    = -1;
    s->refs    = 1;
    char *why = NULL;                /* the reason a step failed            */
    s->path = shared_db_path(&why);
    if (s->path == NULL) {
        asprintf(err, "the database's location cannot be resolved: %s",
                 why != NULL ? why : "unknown error");
        free(why);
        release(s);
        return NULL;
    }
    s->lock_fd = lock_take(s->path, &why);
    if (s->lock_fd < 0) {
        *err = why;
        release(s);
        return NULL;
    }
    cdb_open_kind opened = cdb_open_adopted;
    s->db = cdb_open(s->path, &opened, &why);
    if (s->db == NULL) {
        *err = why;
        close(s->lock_fd);
        release(s);
        return NULL;
    }
    if (kind != NULL) {
        *kind = opened;
    }
    char *line = NULL;
    asprintf(&line, "%s %s", opened == cdb_open_created ? "Created a new database at"
                                                        : "Looking after the database at", s->path);
    post_status(s, line);
    free(line);

    probe_start(s, true);            /* every launch checks the file        */
    s->backup_timer = timer_arm(cdb_look_first_poll_s, SHARED_BACKUP_POLL_MIN * 60.0,
                                ^{ backup_launch(s, true); });
    s->check_timer  = timer_arm(cdb_look_check_interval_s, cdb_look_check_interval_s,
                                ^{ cdb_service_check_now(s); });
    return s;
}

/*
 * cdb_service_stop — see service.h.
 */
void
cdb_service_stop(cdb_service *s)
{
    if (s == NULL) {
        return;
    }
    s->stopped = true;
    timer_stop(s->backup_timer);
    timer_stop(s->check_timer);
    shared_db_close(s->db);
    s->db = NULL;
    if (s->lock_fd >= 0) {           /* last: the database is closed        */
        close(s->lock_fd);
        s->lock_fd = -1;
    }
    release(s);
}

/* ── The facts ───────────────────────────────────────────────────────────── */

/*
 * cdb_service_db — see service.h.
 */
shared_db *
cdb_service_db(cdb_service *s)
{
    return s->db;
}

/*
 * cdb_service_path — see service.h.
 */
const char *
cdb_service_path(cdb_service *s)
{
    return s->path;
}

/*
 * cdb_service_health — see service.h.
 */
const cdb_health *
cdb_service_health(cdb_service *s)
{
    return &s->health;
}

/*
 * cdb_service_checking — see service.h.
 */
bool
cdb_service_checking(cdb_service *s)
{
    return s->checking;
}

/*
 * cdb_service_backing_up — see service.h.
 */
bool
cdb_service_backing_up(cdb_service *s)
{
    return s->backing_up;
}

/*
 * cdb_service_file_size — see service.h.
 */
int64_t
cdb_service_file_size(cdb_service *s)
{
    return s->size;
}

/*
 * cdb_service_sha256 — see service.h.
 */
const char *
cdb_service_sha256(cdb_service *s)
{
    return s->sha256;
}

/*
 * cdb_service_probing — see service.h.
 */
bool
cdb_service_probing(cdb_service *s)
{
    return s->probing;
}

/*
 * cdb_service_status — see service.h.
 */
const char *
cdb_service_status(cdb_service *s)
{
    return s->status;
}

/* ── The passes on request ───────────────────────────────────────────────── */

/*
 * cdb_service_probe — see service.h.
 */
void
cdb_service_probe(cdb_service *s)
{
    probe_start(s, false);
}

/*
 * cdb_service_check_now — see service.h.
 */
void
cdb_service_check_now(cdb_service *s)
{
    if (s->checking) {
        return;                      /* one check at a time                 */
    }
    probe_start(s, true);
}

/*
 * cdb_service_backup_now — see service.h.
 */
void
cdb_service_backup_now(cdb_service *s)
{
    backup_launch(s, false);
}
