/* ===========================================================================
 * service.h — the daemon: the open database, its verdict, and the passes
 *
 * One cdb_service is the whole of what camb.io Database Manager does while it
 * sits in the menu bar.  Starting it resolves where the shared database
 * is (shared_db_path), takes the one-copy lock beside it, adopts the file
 * or creates it, runs the integrity check, and arms two timers: the
 * backup poll (every SHARED_BACKUP_POLL_MIN minutes, the first soon after
 * launch, so a backup that fell due while nothing was running is made at
 * once) and the daily integrity check.  The window asks it for the facts
 * it shows and tells it to check or back up now.
 *
 * Threading: MAIN THREAD ONLY, apart from the passes, which run on a
 * global dispatch queue with a connection of their own (a connection
 * never crosses threads) and hand their outcome back to the main queue.
 * The hooks are called on the main thread.  A pass still running when the
 * service stops finishes on its own and reports to no one.
 *
 * Plain C: strings are malloc'd and freed with free().
 * =========================================================================== */

#ifndef CDB_SERVICE_H
#define CDB_SERVICE_H

#include "cdb.h"

typedef struct cdb_service cdb_service;

/*
 * What the service tells its owner.  Each may be NULL.
 *
 *   state_changed — a fact the window shows moved: a check started or
 *                   landed, a probe of the file's size and digest landed,
 *                   a backup pass ended.  Repaint.
 *   status        — a one-line event: "Backed up to …", "Database check
 *                   passed", the reason Back Up Now was refused.  The
 *                   latest is also kept (cdb_service_status).
 *   problem       — something the user must see: the integrity check
 *                   found damage, or could not run.
 */
typedef struct {
    void (*state_changed)(cdb_service *service, void *ctx);
    void (*status)(cdb_service *service, const char *message, void *ctx);
    void (*problem)(cdb_service *service, const char *title, const char *message,
                    void *ctx);
    void *ctx;
} cdb_service_hooks;

/*
 * cdb_service_start — bring the daemon up (see the header above).  The
 * one place the database may be CREATED: no other process does.
 *
 * Inputs:
 *   hooks — how to reach the owner; copied
 *   kind  — optional; receives whether the file was adopted or created
 *   err   — optional; receives the reason on failure: the location could
 *           not be resolved, another copy holds the lock ("already
 *           running"), or the file could not be opened or created
 *
 * Output: the service (cdb_service_stop it), or NULL.
 */
cdb_service *cdb_service_start(const cdb_service_hooks *hooks, cdb_open_kind *kind,
                               char **err);

/*
 * cdb_service_stop — end the daemon's work: the timers go, the database
 * is closed, the lock released, and no hook is called again.  Safe with
 * NULL.
 *
 * Inputs:
 *   service — the service
 */
void cdb_service_stop(cdb_service *service);

/*
 * cdb_service_db — the main connection, for the window's reads and the
 * policy writes.
 *
 * Inputs:
 *   service — the service
 *
 * Output: the connection (borrowed).
 */
shared_db *cdb_service_db(cdb_service *service);

/*
 * cdb_service_path — the open database file.
 *
 * Inputs:
 *   service — the service
 *
 * Output: the absolute path (borrowed).
 */
const char *cdb_service_path(cdb_service *service);

/*
 * cdb_service_health — the last verdict, held in memory for the life of
 * the service and never written down (cdb.h says why).
 *
 * Inputs:
 *   service — the service
 *
 * Output: the verdict (borrowed; `when` is 0 before the first lands).
 */
const cdb_health *cdb_service_health(cdb_service *service);

/*
 * cdb_service_checking — whether an integrity check is running now.
 *
 * Inputs:
 *   service — the service
 *
 * Output: true while one is.
 */
bool cdb_service_checking(cdb_service *service);

/*
 * cdb_service_backing_up — whether a backup pass is running now.
 *
 * Inputs:
 *   service — the service
 *
 * Output: true while one is.
 */
bool cdb_service_backing_up(cdb_service *service);

/*
 * cdb_service_file_size — the file's size as the last probe read it.
 *
 * Inputs:
 *   service — the service
 *
 * Output: bytes; -1 before the first probe, or when the file could not be
 *         read.
 */
int64_t cdb_service_file_size(cdb_service *service);

/*
 * cdb_service_sha256 — the file's digest as the last probe read it.
 *
 * Inputs:
 *   service — the service
 *
 * Output: the hex digest (borrowed), or NULL before the first probe or
 *         when the file could not be read.
 */
const char *cdb_service_sha256(cdb_service *service);

/*
 * cdb_service_probing — whether the size and digest are being read now.
 *
 * Inputs:
 *   service — the service
 *
 * Output: true while they are.
 */
bool cdb_service_probing(cdb_service *service);

/*
 * cdb_service_peers — the processes holding the database open, as the
 * last peer scan found them (cdb_peers_read).
 *
 * Inputs:
 *   service — the service
 *   count   — receives how many
 *
 * Output: the array (borrowed; NULL before the first scan or when empty).
 */
const cdb_peer *cdb_service_peers(cdb_service *service, size_t *count);

/*
 * cdb_service_scanning — whether a peer scan is running now.
 *
 * Inputs:
 *   service — the service
 *
 * Output: true while one is.
 */
bool cdb_service_scanning(cdb_service *service);

/*
 * cdb_service_scan_peers — scan for the processes holding the database
 * open, on a worker (the scan walks every process's descriptors);
 * state_changed when the list lands.  Ignored while a scan is running.
 *
 * Inputs:
 *   service — the service
 */
void cdb_service_scan_peers(cdb_service *service);

/*
 * cdb_service_status — the latest one-line event, for a window that
 * opens after it was posted.
 *
 * Inputs:
 *   service — the service
 *
 * Output: the message (borrowed), or NULL.
 */
const char *cdb_service_status(cdb_service *service);

/*
 * cdb_service_probe — read the file's size and digest again on a worker
 * (the window opening, or Update's plate refresh); state_changed when
 * they land.  A probe already running is left to finish; a newer one
 * outdates it.
 *
 * Inputs:
 *   service — the service
 */
void cdb_service_probe(cdb_service *service);

/*
 * cdb_service_check_now — Update: the integrity check on a worker, then
 * the probe.  Ignored while a check is running.  Its verdict lands as a
 * status line, or as a problem when anything is wrong.
 *
 * Inputs:
 *   service — the service
 */
void cdb_service_check_now(cdb_service *service);

/*
 * cdb_service_backup_now — Back Up Now: one backup pass on a worker
 * regardless of the schedule, its outcome posted as a status line.
 * Ignored while a pass is running; refused with a status line when there
 * is no usable backup folder.
 *
 * Inputs:
 *   service — the service
 */
void cdb_service_backup_now(cdb_service *service);

#endif /* CDB_SERVICE_H */
