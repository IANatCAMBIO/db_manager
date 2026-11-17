/* ===========================================================================
 * check.h — the test harness: a failure counter, the sandbox guard and a
 * run-loop pump
 *
 * Each test is a plain main() that calls check() for every expectation
 * and returns check_result().  Every test runs with CAMBIO_SHARED_DB
 * naming a file under dev/test/ (the Makefile sets it): check_sandbox()
 * refuses to run otherwise, so a test started by hand can never touch the
 * per-user database in ~/Library/Application Support/camb.io.
 * =========================================================================== */

#ifndef CDB_TEST_CHECK_H
#define CDB_TEST_CHECK_H

#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shared_db.h"

static int check_failures;           /* expectations that did not hold       */

/*
 * check — record one expectation.
 *
 * Inputs:
 *   ok   — whether it held
 *   what — what was expected, for the log
 */
static inline void
check(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        check_failures++;
    }
}

/*
 * check_result — the program's exit status.
 *
 * Output: 0 when every expectation held, 1 otherwise.
 */
static inline int
check_result(void)
{
    if (check_failures > 0) {
        printf("%d check(s) failed\n", check_failures);
    }
    return check_failures > 0 ? 1 : 0;
}

/*
 * check_sandbox — the database file the test may use, from the
 * environment.
 *
 * Output: the path `make test` set in CAMBIO_SHARED_DB.  Exits with status
 *         2 when it is unset, is not an absolute path under a dev/test/
 *         folder, or climbs out of one with "..".
 */
static inline const char *
check_sandbox(void)
{
    const char *db = getenv(SHARED_DB_ENV_VAR);
    if (db == NULL || db[0] != '/' || strstr(db, "/dev/test/") == NULL || strstr(db, "..") != NULL) {
        fprintf(stderr, "refusing to run: %s must name a file under dev/test/ "
                        "(run the tests with `make test`)\n", SHARED_DB_ENV_VAR);
        exit(2);
    }
    return db;
}

/*
 * check_pump — run the main run loop, which drains the main dispatch
 * queue, until *done becomes true or the time runs out: how a test waits
 * for a pass that hands back to the main thread.
 *
 * Inputs:
 *   done    — the flag a callback sets; NULL to simply run for `seconds`
 *   seconds — the longest to wait
 *
 * Output: true when *done was set in time.
 */
static inline bool
check_pump(const bool *done, double seconds)
{
    CFAbsoluteTime end = CFAbsoluteTimeGetCurrent() + seconds;
    while ((done == NULL || !*done) && CFAbsoluteTimeGetCurrent() < end) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
    }
    return done != NULL && *done;
}

#endif /* CDB_TEST_CHECK_H */
