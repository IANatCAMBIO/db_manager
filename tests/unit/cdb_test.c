/*
 * cdb_test.c — the core (src/cdb.h) on a scratch database: creating a
 * database where none is and adopting one that is; the counts; size and
 * SHA-256; the integrity check on a sound, a foreign and a missing file;
 * the words of every verdict; a backup pass and the retention; the policy
 * values and their clamps; path containment; the stamp.
 */

#include "check.h"
#include "cdb.h"

#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "look.h"

/* The SHA-256 of "abc", from the standard's test vectors. */
static const char sha_abc[] =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

/*
 * write_file — write bytes to a file.
 *
 * Inputs:
 *   path  — the file
 *   bytes — what to write
 *
 * Output: true when written.
 */
static bool
write_file(const char *path, const char *bytes)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    bool ok = fputs(bytes, f) >= 0;
    return fclose(f) == 0 && ok;
}

/*
 * count_backups — how many files named like a backup a folder holds.
 *
 * Inputs:
 *   dir — the folder
 *
 * Output: the count; -1 when the folder cannot be read.
 */
static int
count_backups(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        return -1;
    }
    int            n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, SHARED_DB_BACKUP_PREFIX, strlen(SHARED_DB_BACKUP_PREFIX)) == 0) {
            n++;
        }
    }
    closedir(d);
    return n;
}

/*
 * local_time — a local wall-clock moment as unix time, so the stamp tests
 * read the same in every timezone.
 *
 * Inputs:
 *   year, month, day, hour, minute — the local date and time (month 1-12)
 *
 * Output: the unix time.
 */
static int64_t
local_time(int year, int month, int day, int hour, int minute)
{
    struct tm tm = { .tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day,
                     .tm_hour = hour, .tm_min = minute, .tm_isdst = -1 };
    return (int64_t)mktime(&tm);
}

/*
 * starts_with — whether a string begins with a prefix.
 *
 * Inputs:
 *   s      — the string (NULL is "no")
 *   prefix — the prefix
 *
 * Output: true when it does.
 */
static bool
starts_with(const char *s, const char *prefix)
{
    return s != NULL && strncmp(s, prefix, strlen(prefix)) == 0;
}

/*
 * test_open — a missing file is created complete; an existing one is
 * adopted; a foreign file is refused untouched.
 *
 * Inputs:
 *   path — the scratch database
 */
static void
test_open(const char *path)
{
    cdb_open_kind kind = cdb_open_adopted;
    char         *err  = NULL;
    shared_db    *db   = cdb_open(path, &kind, &err);
    check(db != NULL && kind == cdb_open_created, "open: a missing database is created");
    shared_db_close(db);
    check(cdb_file_size(path) > 0, "open: the created file is complete, not empty");

    db = cdb_open(path, &kind, &err);
    check(db != NULL && kind == cdb_open_adopted, "open: an existing database is adopted");
    shared_db_close(db);

    char foreign[1024];
    snprintf(foreign, sizeof foreign, "%s.foreign", path);
    check(write_file(foreign, "not a database"), "open: a foreign file is staged");
    db = cdb_open(foreign, &kind, &err);
    check(db == NULL && err != NULL, "open: a file that is not a shared database is refused with a reason");
    free(err);
    check(cdb_file_size(foreign) == (int64_t)strlen("not a database"), "open: and left untouched");
    unlink(foreign);
}

/*
 * test_health — the check on the sound file, a foreign file and a missing
 * one, and the words of each verdict.
 *
 * Inputs:
 *   path — the scratch database
 */
static void
test_health(const char *path)
{
    cdb_health h = { 0 };
    cdb_health_check_file(path, &h);
    check(h.ok && h.ran && h.when > 0 && h.detail == NULL, "health: a fresh database passes the check");

    char *summary = NULL, *detail = NULL;
    int   light = cdb_health_words(&h, false, h.when, &summary, &detail);
    check(light == 1 && starts_with(summary, "Healthy \xe2\x80\x94 checked ") && strstr(summary, "today") != NULL,
          "health words: a pass reads 'Healthy — checked … today' with a green light");
    check(strstr(detail, "both passed") != NULL, "health words: its tooltip says both checks passed");
    free(summary);
    free(detail);

    light = cdb_health_words(&h, true, h.when, &summary, &detail);
    check(light == 0 && strcmp(summary, "Checking\xe2\x80\xa6") == 0, "health words: a running check says Checking…");
    free(summary);
    free(detail);

    light = cdb_health_words(NULL, false, h.when, &summary, &detail);
    check(light == 0 && strcmp(summary, "Not checked") == 0, "health words: no verdict reads Not checked");
    free(summary);
    free(detail);

    char foreign[1024];
    snprintf(foreign, sizeof foreign, "%s.foreign", path);
    write_file(foreign, "not a database");
    cdb_health_check_file(foreign, &h);
    check(!h.ok && !h.ran && h.detail != NULL, "health: a foreign file is refused, so the check did not run");
    light = cdb_health_words(&h, false, h.when, &summary, &detail);
    check(light == -1 && starts_with(summary, "Check did not complete \xe2\x80\x94"),
          "health words: a check that could not run says so, red");
    check(strcmp(detail, h.detail) == 0, "health words: the tooltip carries the reason");
    free(summary);
    free(detail);
    unlink(foreign);

    cdb_health_check_file("/nonexistent/shared_data.db", &h);
    check(!h.ok && !h.ran, "health: a missing file cannot be checked");

    cdb_health found = { .ok = false, .ran = true, .when = h.when, .detail = strdup("row 3 missing") };
    light = cdb_health_words(&found, false, h.when, &summary, &detail);
    check(light == -1 && starts_with(summary, "Problems found \xe2\x80\x94") && strcmp(detail, "row 3 missing") == 0,
          "health words: findings read 'Problems found' with the findings as the tooltip");
    free(summary);
    free(detail);
    cdb_health_clear(&found);
    cdb_health_clear(&h);
    check(h.when == 0 && h.detail == NULL, "health: clear resets a verdict to never");
}

/*
 * test_totals — the counts: notes not in the Trash, tasks not deleted,
 * tags.
 *
 * Inputs:
 *   path — the scratch database
 */
static void
test_totals(const char *path)
{
    shared_db *db = cdb_open(path, NULL, NULL);
    cdb_totals totals;
    check(cdb_totals_read(db, &totals, NULL) == 0 && totals.notes == 0 && totals.tasks == 0 && totals.tags == 0,
          "totals: an empty database counts nothing");
    sqlite3_exec(shared_db_handle(db),
        "INSERT INTO notes (title, created_at, updated_at) VALUES ('kept', 1, 1);"
        "INSERT INTO notes (title, created_at, updated_at, trashed) VALUES ('binned', 1, 1, 1);"
        "INSERT INTO tasks (title) VALUES ('live');"
        "INSERT INTO tasks (title, deleted) VALUES ('gone', 1);"
        "INSERT INTO tags (name) VALUES ('Work');"
        "INSERT INTO task_tags (task_id, tag_id) VALUES (1, 1);",
        NULL, NULL, NULL);
    check(cdb_totals_read(db, &totals, NULL) == 0 && totals.notes == 1 && totals.tasks == 1 && totals.tags == 1,
          "totals: a trashed note and a deleted task are not counted; a carried tag is");
    shared_db_close(db);
}

/*
 * test_file — the size and the digest of a file.
 *
 * Inputs:
 *   path — the scratch database
 */
static void
test_file(const char *path)
{
    char small[1024];
    snprintf(small, sizeof small, "%s.abc", path);
    write_file(small, "abc");
    check(cdb_file_size(small) == 3, "file: the size is the file's");
    char *sha = cdb_file_sha256(small);
    check(sha != NULL && strcmp(sha, sha_abc) == 0, "file: the SHA-256 of \"abc\" is the standard's");
    free(sha);
    unlink(small);
    check(cdb_file_size("/nonexistent/x") == -1, "file: a missing file has no size");
    check(cdb_file_sha256("/nonexistent/x") == NULL, "file: and no digest");
    struct stat st;
    sha = cdb_file_sha256(path);
    check(sha != NULL && strlen(sha) == 64 && stat(path, &st) == 0 && cdb_file_size(path) == (int64_t)st.st_size,
          "file: the database has a 64-character digest and its stat size");
    free(sha);
}

/*
 * test_backup — a backup pass: the poll makes one when due and none when
 * not; Back Up Now always; the UTC name; retention; a missing file.
 *
 * Inputs:
 *   path — the scratch database
 */
static void
test_backup(const char *path)
{
    shared_db *db  = cdb_open(path, NULL, NULL);
    char      *dir = shared_db_backup_dir(db, NULL);
    char       expect[1024];
    snprintf(expect, sizeof expect, "%.*s/" SHARED_DB_BACKUP_DIR_NAME, (int)(strrchr(path, '/') - path), path);
    check(dir != NULL && strcmp(dir, expect) == 0, "backup: with no folder chosen, backups go beside the database");
    shared_db_close(db);

    char *message = NULL;
    cdb_backup_result r = cdb_backup_run(path, true, &message);
    check(r == cdb_backup_made && starts_with(message, "Backed up to " SHARED_DB_BACKUP_PREFIX),
          "backup: the first poll finds a backup due and makes it");
    check(message != NULL && strstr(message, "Z.db") != NULL, "backup: its name carries the time in UTC");
    free(message);
    message = NULL;
    r = cdb_backup_run(path, true, &message);
    check(r == cdb_backup_none && message == NULL, "backup: the next poll finds nothing due and says nothing");
    r = cdb_backup_run(path, false, &message);
    check(r == cdb_backup_made, "backup: Back Up Now backs up regardless");
    free(message);
    message = NULL;
    check(count_backups(dir) == 2, "backup: two files so far");

    db = cdb_open(path, NULL, NULL);
    check(cdb_policy_store(db, SHARED_SETTING_BACKUP_KEEP, 1, NULL) == 0, "policy: keep 1 is stored");
    shared_db_close(db);
    r = cdb_backup_run(path, false, &message);
    check(r == cdb_backup_made && count_backups(dir) == 1, "backup: retention leaves the newest one");
    free(message);
    message = NULL;
    free(dir);

    r = cdb_backup_run("/nonexistent/shared_data.db", false, &message);
    check(r == cdb_backup_failed && starts_with(message, "Backup failed: "), "backup: a missing file fails with a reason");
    free(message);
}

/*
 * test_policy — the values the module applies, and their clamps.
 *
 * Inputs:
 *   path — the scratch database
 */
static void
test_policy(const char *path)
{
    shared_db *db = cdb_open(path, NULL, NULL);
    check(cdb_policy_value(db, SHARED_SETTING_BACKUP_ENABLED) == SHARED_BACKUP_ENABLED_DEFAULT &&
          cdb_policy_value(db, SHARED_SETTING_BACKUP_INTERVAL_MIN) == SHARED_BACKUP_INTERVAL_MIN_DEFAULT,
          "policy: unset keys read as the module's defaults");
    check(cdb_policy_store(db, SHARED_SETTING_BACKUP_INTERVAL_MIN, 30, NULL) == 0 &&
          cdb_policy_value(db, SHARED_SETTING_BACKUP_INTERVAL_MIN) == 30, "policy: an interval is stored and read back");
    check(cdb_policy_store(db, SHARED_SETTING_BACKUP_KEEP, 99999, NULL) == 0 &&
          cdb_policy_value(db, SHARED_SETTING_BACKUP_KEEP) == SHARED_BACKUP_KEEP_MAX,
          "policy: a value past the range reads back held to it");
    check(cdb_policy_store(db, SHARED_SETTING_BACKUP_ENABLED, 0, NULL) == 0 &&
          cdb_policy_value(db, SHARED_SETTING_BACKUP_ENABLED) == 0, "policy: the switch is stored");
    check(cdb_policy_value(db, SHARED_SETTING_BACKUP_LAST_AT) > 0, "policy: the last backup's time is there");
    shared_db_close(db);
}

/*
 * has_pid — whether a peer list names a process.
 *
 * Inputs:
 *   peers — the list
 *   count — its length
 *   pid   — the process
 *
 * Output: true when it does.
 */
static bool
has_pid(const cdb_peer *peers, size_t count, pid_t pid)
{
    for (size_t i = 0; i < count; i++) {
        if (peers[i].pid == pid) {
            return true;
        }
    }
    return false;
}

/*
 * test_peers — this process is listed while it holds a connection and
 * not after it closes it; a connection does not outlive its process.
 *
 * Inputs:
 *   path — the scratch database
 */
static void
test_peers(const char *path)
{
    shared_db *db    = cdb_open(path, NULL, NULL);
    cdb_peer  *peers = NULL;
    size_t     count = 0;
    check(cdb_peers_read(path, &peers, &count) == 0 && has_pid(peers, count, getpid()),
          "peers: this process is listed while its connection is open");
    check(count >= 1 && peers[0].name[0] != '\0', "peers: with its name");
    free(peers);
    shared_db_close(db);
    check(cdb_peers_read(path, &peers, &count) == 0 && !has_pid(peers, count, getpid()),
          "peers: and not once the connection is closed");
    free(peers);
}

/*
 * test_leftovers — nothing on a fresh file; log content, a backup claim
 * (live, then expired) and an unfinished backup folder, each with its
 * words.
 *
 * Inputs:
 *   path — the scratch database
 */
static void
test_leftovers(const char *path)
{
    shared_db    *db  = cdb_open(path, NULL, NULL);
    int64_t       now = (int64_t)time(NULL);
    cdb_leftovers left;
    sqlite3_exec(shared_db_handle(db), "PRAGMA wal_checkpoint(TRUNCATE)", NULL, NULL, NULL);
    cdb_leftovers_read(db, path, now, &left);
    check(left.wal_bytes == 0 && left.claim_at == 0 && left.unfinished_backups == 0,
          "leftovers: a checkpointed file with no claim has none");
    char *text = cdb_leftovers_words(&left, false, now);
    check(strncmp(text, "Nothing left behind", 19) == 0, "leftovers words: and says so");
    free(text);

    sqlite3_exec(shared_db_handle(db), "INSERT INTO tasks (title) VALUES ('x')", NULL, NULL, NULL);
    cdb_leftovers_read(db, path, now, &left);
    check(left.wal_bytes > 0, "leftovers: a write leaves log content until the next checkpoint");
    text = cdb_leftovers_words(&left, false, now);
    check(strstr(text, "write-ahead log holds") != NULL, "leftovers words: the log content is named");
    free(text);

    char claim[32];
    snprintf(claim, sizeof claim, "%lld", (long long)(now - 60));
    shared_settings_set(db, SHARED_SETTING_BACKUP_CLAIM_AT, claim, NULL);
    cdb_leftovers_read(db, path, now, &left);
    check(left.claim_at == now - 60 && left.claim_live, "leftovers: a minute-old claim is live");
    text = cdb_leftovers_words(&left, false, now);
    check(strstr(text, "A backup claim from") != NULL && strstr(text, "expires after") != NULL,
          "leftovers words: a live claim held by no pass here is reported");
    free(text);
    text = cdb_leftovers_words(&left, true, now);
    check(strstr(text, "backup claim") == NULL, "leftovers words: not while this process's own pass runs");
    free(text);
    cdb_leftovers_read(db, path, now + SHARED_BACKUP_CLAIM_LEASE_SEC, &left);
    check(!left.claim_live, "leftovers: a claim older than the lease has expired");
    text = cdb_leftovers_words(&left, false, now + SHARED_BACKUP_CLAIM_LEASE_SEC);
    check(strstr(text, "An expired backup claim") != NULL, "leftovers words: and is said to be ignored");
    free(text);
    shared_settings_set(db, SHARED_SETTING_BACKUP_CLAIM_AT, NULL, NULL);

    char *dir = shared_db_backup_dir(db, NULL);
    char  unfinished[1100];
    snprintf(unfinished, sizeof unfinished, "%s/" SHARED_DB_BACKUP_TMP_PREFIX "abc123", dir);
    mkdir(unfinished, 0700);
    cdb_leftovers_read(db, path, now, &left);
    check(left.unfinished_backups == 1, "leftovers: an unfinished backup folder is counted");
    text = cdb_leftovers_words(&left, false, now);
    check(strstr(text, "1 unfinished backup folder left") != NULL, "leftovers words: and named");
    free(text);
    rmdir(unfinished);
    free(dir);
    shared_db_close(db);
}

/*
 * test_paths — the containment test behind the folder warning.
 */
static void
test_paths(void)
{
    check(cdb_path_inside("/a/b/backups", "/a/b"), "inside: a subdirectory is inside its parent");
    check(cdb_path_inside("/a/b", "/a/b"), "inside: a folder is inside itself");
    check(!cdb_path_inside("/a/b", "/a/b/backups"), "inside: a parent is not inside its child");
    check(!cdb_path_inside("/a/backups-old", "/a/backups"), "inside: a sibling with the same prefix is not");
    check(cdb_path_inside("/a/./b//backups/", "/a/b"), "inside: dots, doubled and trailing slashes are ignored");
    check(cdb_path_inside("/a/x/../b/backups", "/a/b"), "inside: .. is resolved");
    check(cdb_path_inside("/a", "/"), "inside: everything is inside the root");
}

/*
 * test_stamp — "13:24 today" and "Sep 8 at 09:02".
 */
static void
test_stamp(void)
{
    int64_t now = local_time(2026, 9, 8, 13, 24);
    char *s = cdb_stamp(local_time(2026, 9, 8, 9, 2), now);
    check(strcmp(s, "09:02 today") == 0, "stamp: the same day reads as a time today");
    free(s);
    s = cdb_stamp(local_time(2026, 9, 7, 9, 2), now);
    check(strcmp(s, "Sep 7 at 09:02") == 0, "stamp: another day reads as month, day and time");
    free(s);
}

int
main(void)
{
    const char *path = check_sandbox();
    test_open(path);
    test_health(path);
    test_totals(path);
    test_file(path);
    test_backup(path);
    test_policy(path);
    test_peers(path);
    test_leftovers(path);
    test_paths();
    test_stamp();
    return check_result();
}
