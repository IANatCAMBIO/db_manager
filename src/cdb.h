/* ===========================================================================
 * cdb.h — the database facts and passes, as plain C
 *
 * Everything camb.io Database Manager knows or does to the shared file, with no
 * threads and no AppKit, so a test can call it directly: adopting or
 * creating the file, what it holds, its size and fingerprint, the
 * integrity check, one backup pass, the backup policy, and the words
 * the window puts to a moment and a verdict.
 *
 * The file itself — its schema, location, connections, the copy and its
 * verification — is the shared module's (shared_db.h, compiled in from
 * ../shared_data); this is the daemon's use of it.  Nothing here writes
 * the database except the policy setters and the backup pass (which
 * records its outcome in shared_settings, as the module does).
 *
 * Strings returned here are malloc'd and freed with free().  Every
 * function is safe on any thread with a connection of that thread's own.
 * =========================================================================== */

#ifndef CDB_H
#define CDB_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "shared_db.h"

/* ── Opening ─────────────────────────────────────────────────────────────── */

/* Whether cdb_open found the file or made it. */
typedef enum {
    cdb_open_adopted = 0,            /* a shared database was already there */
    cdb_open_created                 /* none was: a complete one was created */
} cdb_open_kind;

/*
 * cdb_open — the daemon's main connection: adopt the shared database at
 * `path` when one exists, create it there when none does.  The module
 * does the creating (shared_db_open: atomically, complete, stamped) and
 * refuses anything that is not a shared database untouched.
 *
 * Inputs:
 *   path — the database file (shared_db_path's answer)
 *   kind — optional; receives whether the file was adopted or created
 *   err  — optional; receives the reason on failure
 *
 * Output: the open connection (shared_db_close it), or NULL.
 */
shared_db *cdb_open(const char *path, cdb_open_kind *kind, char **err);

/* ── What the file holds ─────────────────────────────────────────────────── */

/* The counts the plate's Data line shows. */
typedef struct {
    int64_t notes;                   /* notes not in the Trash              */
    int64_t tasks;                   /* tasks not tombstoned                */
    int64_t tags;                    /* tags (a tag exists while a note or a
                                        live task carries it)              */
} cdb_totals;

/*
 * cdb_totals_read — count the live notes, tasks and tags in one snapshot.
 *
 * Inputs:
 *   db  — an open connection
 *   out — receives the counts (zeroed on failure)
 *   err — optional; receives the reason on failure
 *
 * Output: 0 on success, -1 when the query failed.
 */
int cdb_totals_read(shared_db *db, cdb_totals *out, char **err);

/*
 * cdb_file_size — how much room the database file takes on disk (free
 * pages included: the question is the file's size, not the data's).
 *
 * Inputs:
 *   path — the file
 *
 * Output: its size in bytes, or -1 when it cannot be read.
 */
int64_t cdb_file_size(const char *path);

/*
 * cdb_file_sha256 — hex SHA-256 of the file as it stands, read in chunks
 * so a large database is never held in memory twice.  Never stored: a
 * live database changes with the next edit, so the digest is a
 * fingerprint of this moment, comparable with `shasum -a 256` or a backup.
 *
 * Inputs:
 *   path — the file
 *
 * Output: the 64-character hex digest, or NULL when it cannot be read.
 */
char *cdb_file_sha256(const char *path);

/* ── The integrity check ─────────────────────────────────────────────────── */

/*
 * The outcome of one check.  `ok` is what a caller acts on; `ran` tells
 * "the checks found problems" from "the checks could not be run", which
 * are different sentences to put in front of a user.  `detail` is NULL
 * when there is nothing to say; `when` is 0 before any check.
 */
typedef struct {
    bool     ok;                     /* the checks ran AND passed           */
    bool     ran;                    /* the checks executed                 */
    int64_t  when;                   /* unix time of the pass, 0 = never    */
    char    *detail;                 /* the findings, or why it did not run */
} cdb_health;

/*
 * cdb_health_check_file — the check over a read-only connection of its
 * own (shared_db_check_file: integrity_check, foreign_key_check, the
 * stamps, WAL, the cross-table invariants), which writes nothing — a
 * verdict stamped into the file would make the check change the file it
 * measured, and the digest beside it move.
 *
 * Inputs:
 *   path — the database file
 *   out  — receives the verdict, its previous detail freed; `when` is now
 */
void cdb_health_check_file(const char *path, cdb_health *out);

/*
 * cdb_health_clear — free a verdict's detail and reset it to "never".
 *
 * Inputs:
 *   health — the verdict
 */
void cdb_health_clear(cdb_health *health);

/*
 * cdb_health_words — the plate's health line and its tooltip.  Three
 * failures are three sentences on purpose: "Problems found" and "Check
 * did not complete" send someone looking for different things.
 *
 * Inputs:
 *   health   — the verdict, or NULL when no check has been made
 *   checking — a check is running now (shown over the previous verdict)
 *   now      — unix time to judge "today" against
 *   summary  — receives "Healthy — checked 13:24 today", "Problems found —
 *              …", "Check did not complete — …", "Checking…" or "Not checked"
 *   detail   — receives the tooltip: the findings when there are any,
 *              else what the verdict means
 *
 * Output: 1 for a check that ran and passed (a green light), -1 for one
 *         that failed or could not run (red), 0 otherwise (no light).
 */
int cdb_health_words(const cdb_health *health, bool checking, int64_t now,
                     char **summary, char **detail);

/* ── Backups ─────────────────────────────────────────────────────────────── */

/* What one backup pass did. */
typedef enum {
    cdb_backup_none = 0,             /* the poll: nothing was due           */
    cdb_backup_made,                 /* a verified copy, old ones pruned    */
    cdb_backup_made_with_problem,    /* a verified copy whose pruning failed */
    cdb_backup_failed                /* no copy                             */
} cdb_backup_result;

/*
 * cdb_backup_run — one backup pass on a connection of its own (a
 * connection never crosses threads): the module's copy for Back Up Now,
 * or its due-check-and-claim for the poll, which backs up only when the
 * policy in the file says one is due and no other process holds the claim.
 *
 * Inputs:
 *   path     — the database file
 *   periodic — the poll (shared_db_backup_if_due) rather than Back Up Now
 *   message  — receives the one-line outcome to show; NULL when there is
 *              nothing to say (a poll that found nothing due)
 *
 * Output: what happened.
 */
cdb_backup_result cdb_backup_run(const char *path, bool periodic, char **message);

/*
 * cdb_policy_value — one backup-policy integer as the module applies it
 * (shared_backup_policy_get: the stored value or its default, held to its
 * range), so the window shows exactly what the backups obey.
 *
 * Inputs:
 *   db  — an open connection
 *   key — a SHARED_SETTING_BACKUP_* integer key
 *
 * Output: the value (the module's default when it cannot be read).
 */
int64_t cdb_policy_value(shared_db *db, const char *key);

/*
 * cdb_policy_store — write one backup-policy integer to shared_settings,
 * where the backups read it.
 *
 * Inputs:
 *   db    — an open read-write connection
 *   key   — SHARED_SETTING_BACKUP_ENABLED, _INTERVAL_MIN or _KEEP
 *   value — the new value
 *   err   — optional; receives the reason on failure
 *
 * Output: 0 on success, -1 on failure.
 */
int cdb_policy_store(shared_db *db, const char *key, int64_t value, char **err);

/*
 * cdb_path_inside — is `dir` the folder `other`, or inside it?  The test
 * behind the backup folder's warning: the default backups folder is
 * INSIDE the database's folder, so losing that folder loses both.
 * Lexical ("." and ".." resolved, repeated and trailing slashes dropped),
 * no symlinks followed.
 *
 * Inputs:
 *   dir, other — two absolute folder paths
 *
 * Output: true when losing `other` loses `dir`.
 */
bool cdb_path_inside(const char *dir, const char *other);

/* ── Who has the file open, and what a crash leaves ──────────────────────── */

/* One process holding the database file (or its -wal / -shm) open. */
typedef struct {
    pid_t pid;
    char  name[64];                  /* the process name (proc_name)        */
} cdb_peer;

/*
 * cdb_peers_read — every process of this user with the database file, or
 * its -wal or -shm, open, by scanning their file descriptors (libproc).
 * This process is included when it holds a connection.  Other users'
 * processes cannot be seen and are not listed.  A connection does not
 * outlive its process — macOS closes the files and releases the locks of
 * one that crashed — so this list is the whole truth about who is
 * connected now.
 *
 * Inputs:
 *   path  — the database file
 *   out   — receives a malloc'd array (free it), NULL when empty
 *   count — receives how many
 *
 * Output: 0 on success, -1 when the processes could not be listed.
 */
int cdb_peers_read(const char *path, cdb_peer **out, size_t *count);

/* What a crashed or interrupted process can leave behind. */
typedef struct {
    int64_t wal_bytes;               /* the -wal file's size, 0 when none   */
    int64_t claim_at;                /* the backup claim's time, 0 for none */
    bool    claim_live;              /* ... and younger than the lease      */
    int     unfinished_backups;      /* .in-progress-* folders left in the
                                        backup folder                      */
} cdb_leftovers;

/*
 * cdb_leftovers_read — what is left behind: write-ahead-log content not
 * yet folded into the file (harmless: the next writer folds it in), a
 * backup claim (a pass that never finished keeps it until the lease
 * expires), and unfinished backup folders (swept once an hour old).
 *
 * Inputs:
 *   db   — an open connection (for the claim)
 *   path — the database file
 *   now  — the current unix time (passed in so it can be tested)
 *   out  — receives the findings
 */
void cdb_leftovers_read(shared_db *db, const char *path, int64_t now, cdb_leftovers *out);

/*
 * cdb_leftovers_words — the leftovers as the window says them, one line
 * each; a claim held by this process's own running pass is not a
 * leftover.
 *
 * Inputs:
 *   left       — the findings
 *   backing_up — this process has a backup pass running now
 *   now        — the current unix time
 *
 * Output: the text (free it); the "nothing left behind" line when there
 *         is nothing.
 */
char *cdb_leftovers_words(const cdb_leftovers *left, bool backing_up, int64_t now);

/* ── Words ───────────────────────────────────────────────────────────────── */

/*
 * cdb_stamp — a moment in as few words as carry it: "13:24 today" on the
 * same local day, otherwise "Sep 8 at 09:02".
 *
 * Inputs:
 *   when — unix time of the moment
 *   now  — unix time to judge "today" against (passed in so it can be tested)
 *
 * Output: the text; "an unknown time" when `when` cannot be shown.
 */
char *cdb_stamp(int64_t when, int64_t now);

#endif /* CDB_H */
