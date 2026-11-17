/*
 * service_test.c — the daemon (src/service.h) on a scratch database: the
 * start creates the file and runs the launch check, whose verdict and the
 * probe's size and digest land on the main thread; the one-copy lock; the
 * first poll's backup; Back Up Now; a check that cannot run reaching the
 * problem hook; a stop under a running pass that reports to no one.
 */

#include "check.h"
#include "service.h"

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

/* What the hooks saw. */
static int   changes;                /* state_changed calls                 */
static char  last_status[512];
static char  last_problem[512];      /* the title                           */
static bool  on_main;                /* every hook ran on the main thread   */

/*
 * on_changed — the state_changed hook.
 *
 * Inputs:
 *   service — the service (unused)
 *   ctx     — unused
 */
static void
on_changed(cdb_service *service, void *ctx)
{
    (void)service;
    (void)ctx;
    on_main = on_main && pthread_main_np() != 0;
    changes++;
}

/*
 * on_status — the status hook.
 *
 * Inputs:
 *   service — the service (unused)
 *   message — the line
 *   ctx     — unused
 */
static void
on_status(cdb_service *service, const char *message, void *ctx)
{
    (void)service;
    (void)ctx;
    on_main = on_main && pthread_main_np() != 0;
    snprintf(last_status, sizeof last_status, "%s", message);
}

/*
 * on_problem — the problem hook.
 *
 * Inputs:
 *   service — the service (unused)
 *   title   — the headline
 *   message — the detail (unused)
 *   ctx     — unused
 */
static void
on_problem(cdb_service *service, const char *title, const char *message, void *ctx)
{
    (void)service;
    (void)message;
    (void)ctx;
    on_main = on_main && pthread_main_np() != 0;
    snprintf(last_problem, sizeof last_problem, "%s", title);
}

static const cdb_service_hooks hooks = { on_changed, on_status, on_problem, NULL };

/*
 * wait_check — pump until the running check lands.
 *
 * Inputs:
 *   s — the service
 *
 * Output: true when it landed in time.
 */
static bool
wait_check(cdb_service *s)
{
    CFAbsoluteTime end = CFAbsoluteTimeGetCurrent() + 20.0;
    while (cdb_service_checking(s) && CFAbsoluteTimeGetCurrent() < end) {
        check_pump(NULL, 0.05);
    }
    return !cdb_service_checking(s);
}

/*
 * wait_backup — pump until the running backup pass lands.
 *
 * Inputs:
 *   s — the service
 *
 * Output: true when it landed in time.
 */
static bool
wait_backup(cdb_service *s)
{
    CFAbsoluteTime end = CFAbsoluteTimeGetCurrent() + 20.0;
    while (cdb_service_backing_up(s) && CFAbsoluteTimeGetCurrent() < end) {
        check_pump(NULL, 0.05);
    }
    return !cdb_service_backing_up(s);
}

int
main(void)
{
    const char *path = check_sandbox();
    on_main = true;

    cdb_open_kind kind = cdb_open_adopted;
    char         *err  = NULL;
    cdb_service  *s    = cdb_service_start(&hooks, &kind, &err);
    check(s != NULL, "start: the service starts");
    if (s == NULL) {
        printf("  %s\n", err != NULL ? err : "?");
        return check_result();
    }
    check(kind == cdb_open_created && strcmp(cdb_service_path(s), path) == 0,
          "start: the database was created at the sandbox path");
    check(strncmp(last_status, "Created a new database at ", 26) == 0, "start: the status says so");
    check(cdb_service_checking(s) && cdb_service_probing(s), "start: the launch check and probe are running");
    check(cdb_service_health(s)->when == 0, "start: no verdict yet");

    check(wait_check(s), "check: the launch check lands");
    check(cdb_service_health(s)->ok && cdb_service_health(s)->when > 0, "check: a fresh database passes");
    check(strcmp(last_status, "Database check passed") == 0, "check: the pass is posted as a status line");
    check(!cdb_service_probing(s) && cdb_service_file_size(s) > 0 &&
          cdb_service_sha256(s) != NULL && strlen(cdb_service_sha256(s)) == 64,
          "probe: the size and digest landed with the check");
    check(on_main && changes >= 2, "hooks: every hook ran on the main thread");

    cdb_service *second = cdb_service_start(&hooks, NULL, &err);
    check(second == NULL && err != NULL && strstr(err, "already") != NULL,
          "lock: a second copy on the same database is refused as already running");
    free(err);
    err = NULL;

    /* The first poll, a few seconds after launch: a backup is due (none
     * was ever made), so one is made.                                     */
    CFAbsoluteTime end = CFAbsoluteTimeGetCurrent() + 15.0;
    while (cdb_policy_value(cdb_service_db(s), SHARED_SETTING_BACKUP_LAST_AT) == 0 &&
           CFAbsoluteTimeGetCurrent() < end) {
        check_pump(NULL, 0.1);
    }
    check(wait_backup(s) && cdb_policy_value(cdb_service_db(s), SHARED_SETTING_BACKUP_LAST_AT) > 0,
          "poll: the first poll after launch makes the backup that was due");
    check(strncmp(last_status, "Backed up to " SHARED_DB_BACKUP_PREFIX, 25) == 0,
          "poll: and posts its name");

    last_status[0] = '\0';
    cdb_service_backup_now(s);
    check(cdb_service_backing_up(s), "back up now: a pass is in flight");
    check(wait_backup(s) && strncmp(last_status, "Backed up to ", 13) == 0, "back up now: a second backup is made");

    /* A check that cannot run: the file moved aside, as a restore does
     * (the main connection keeps the open inode; the check's own
     * connection finds nothing at the path).                             */
    char aside[1100];
    snprintf(aside, sizeof aside, "%s.aside", path);
    check(rename(path, aside) == 0, "update: the file is moved aside");
    last_problem[0] = '\0';
    cdb_service_check_now(s);
    check(cdb_service_checking(s), "update: a check is in flight");
    cdb_service_check_now(s);
    check(wait_check(s), "update: it lands");
    check(rename(aside, path) == 0, "update: the file is put back");
    check(!cdb_service_health(s)->ok && !cdb_service_health(s)->ran,
          "update: a check that found no file did not run");
    check(strcmp(last_problem, "Database Integrity Check") == 0, "update: and reaches the problem hook");

    cdb_service_check_now(s);
    check(wait_check(s) && cdb_service_health(s)->ok, "update: the next check passes again");

    size_t count = 0;
    check(cdb_service_peers(s, &count) == NULL && count == 0, "peers: no list before the first scan");
    cdb_service_scan_peers(s);
    check(cdb_service_scanning(s), "peers: a scan is in flight");
    end = CFAbsoluteTimeGetCurrent() + 20.0;
    while (cdb_service_scanning(s) && CFAbsoluteTimeGetCurrent() < end) {
        check_pump(NULL, 0.05);
    }
    const cdb_peer *peers = cdb_service_peers(s, &count);
    bool me = false;
    for (size_t i = 0; i < count; i++) {
        me = me || peers[i].pid == getpid();
    }
    check(!cdb_service_scanning(s) && me, "peers: the scan lands with this process in the list");

    /* Stop under a running check: nothing reports after the stop.       */
    cdb_service_check_now(s);
    int seen = changes;              /* the start of the check is announced */
    cdb_service_stop(s);
    check_pump(NULL, 3.0);
    check(changes == seen, "stop: a pass still running reports to no one");
    return check_result();
}
