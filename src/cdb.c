/* ===========================================================================
 * cdb.c — the database facts and passes (see cdb.h)
 * =========================================================================== */

#include "cdb.h"

#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <libproc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Bytes per read when hashing. */
#define SHA_CHUNK 65536

/* Month names for the stamp: a table of our own rather than strftime's
 * "%b", whose answer follows the locale while the rest of the sentence is
 * English.                                                                 */
static const char * const month_names[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* The tooltip of a verdict that has no words of its own. */
#define DETAIL_PASSED    "PRAGMA integrity_check and PRAGMA foreign_key_check " \
                         "both passed against this file."
#define DETAIL_SILENT    "The check gave no further detail."
#define DETAIL_UNCHECKED "Nothing has verified this database yet.  Press Update."
#define DETAIL_CHECKING  "PRAGMA integrity_check is running on a worker thread; " \
                         "the verdict lands here."

/*
 * take_message — move a message into the caller's slot, or free it when
 * the caller did not ask for one.
 *
 * Inputs:
 *   slot    — the caller's optional out-param
 *   message — the malloc'd message, or NULL
 */
static void
take_message(char **slot, char *message)
{
    if (slot != NULL) {
        *slot = message;
    } else {
        free(message);
    }
}

/*
 * words — printf into a new string.
 *
 * Inputs:
 *   fmt — the format and its arguments
 *
 * Output: the string, or NULL when memory runs out.
 */
static char *
words(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static char *
words(const char *fmt, ...)
{
    va_list ap;
    char   *out = NULL;
    va_start(ap, fmt);
    if (vasprintf(&out, fmt, ap) < 0) {
        out = NULL;
    }
    va_end(ap);
    return out;
}

/* ── Opening ─────────────────────────────────────────────────────────────── */

/*
 * cdb_open — see cdb.h.
 */
shared_db *
cdb_open(const char *path, cdb_open_kind *kind, char **err)
{
    bool       existed = access(path, F_OK) == 0;
    shared_db *db      = shared_db_open(path, err);
    if (db != NULL && kind != NULL) {
        *kind = existed ? cdb_open_adopted : cdb_open_created;
    }
    return db;
}

/* ── What the file holds ─────────────────────────────────────────────────── */

/*
 * cdb_totals_read — see cdb.h.
 */
int
cdb_totals_read(shared_db *db, cdb_totals *out, char **err)
{
    static const char sql[] =
        "SELECT (SELECT COUNT(*) FROM notes WHERE " SHARED_DB_NOTE_VISIBLE_SQL "),"
        "       (SELECT COUNT(*) FROM tasks WHERE deleted = 0),"
        "       (SELECT COUNT(*) FROM tags)";
    sqlite3      *handle = shared_db_handle(db);
    sqlite3_stmt *stmt   = NULL;
    memset(out, 0, sizeof *out);
    if (sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_ROW) {
        take_message(err, words("cannot count: %s", sqlite3_errmsg(handle)));
        sqlite3_finalize(stmt);
        return -1;
    }
    out->notes = sqlite3_column_int64(stmt, 0);
    out->tasks = sqlite3_column_int64(stmt, 1);
    out->tags  = sqlite3_column_int64(stmt, 2);
    sqlite3_finalize(stmt);
    return 0;
}

/*
 * cdb_file_size — see cdb.h.
 */
int64_t
cdb_file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
}

/*
 * cdb_file_sha256 — see cdb.h.
 */
char *
cdb_file_sha256(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    CC_SHA256_CTX  ctx;
    unsigned char *buf = malloc(SHA_CHUNK);
    size_t         n;                /* bytes in the current chunk          */
    CC_SHA256_Init(&ctx);
    while ((n = fread(buf, 1, SHA_CHUNK, f)) > 0) {
        CC_SHA256_Update(&ctx, buf, (CC_LONG)n);
    }
    bool bad = ferror(f) != 0;       /* a short read that was an error      */
    free(buf);
    fclose(f);
    if (bad) {
        return NULL;
    }
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &ctx);
    char *hex = malloc(2 * CC_SHA256_DIGEST_LENGTH + 1);
    for (size_t i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) {
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    }
    return hex;
}

/* ── The integrity check ─────────────────────────────────────────────────── */

/*
 * cdb_health_check_file — see cdb.h.
 */
void
cdb_health_check_file(const char *path, cdb_health *out)
{
    int   ran    = 0;                /* the checks executed                 */
    char *detail = NULL;             /* the findings, one per line          */
    char *err    = NULL;             /* why the checks did not run          */
    int   rc     = shared_db_check_file(path, 1, &ran, &detail, &err);
    cdb_health_clear(out);
    out->ok   = rc == 0;
    out->ran  = ran != 0;
    out->when = (int64_t)time(NULL);
    if (rc == 1) {
        out->detail = detail;
        detail      = NULL;
    } else if (rc < 0) {
        out->detail = words("cannot check %s: %s", path, err != NULL ? err : "unknown error");
    }
    free(detail);
    free(err);
}

/*
 * cdb_health_clear — see cdb.h.
 */
void
cdb_health_clear(cdb_health *health)
{
    free(health->detail);
    memset(health, 0, sizeof *health);
}

/*
 * cdb_health_words — see cdb.h.
 */
int
cdb_health_words(const cdb_health *health, bool checking, int64_t now,
                 char **summary, char **detail)
{
    if (checking) {
        *summary = strdup("Checking\xe2\x80\xa6");
        *detail  = strdup(DETAIL_CHECKING);
        return 0;
    }
    if (health == NULL || health->when == 0) {
        *summary = strdup("Not checked");
        *detail  = strdup(DETAIL_UNCHECKED);
        return 0;
    }
    const char *lead  = health->ok  ? "Healthy \xe2\x80\x94 checked"
                      : health->ran ? "Problems found \xe2\x80\x94"
                                    : "Check did not complete \xe2\x80\x94";
    char       *stamp = cdb_stamp(health->when, now);
    *summary = words("%s %s", lead, stamp);
    free(stamp);
    /* A failed check with no words must not borrow the sentence that says
     * both checks passed.                                                  */
    *detail = strdup(health->detail != NULL ? health->detail
                   : health->ok             ? DETAIL_PASSED
                                            : DETAIL_SILENT);
    return health->ok ? 1 : -1;
}

/* ── Backups ─────────────────────────────────────────────────────────────── */

/*
 * cdb_backup_run — see cdb.h.
 */
cdb_backup_result
cdb_backup_run(const char *path, bool periodic, char **message)
{
    char      *why = NULL;           /* why the connection did not open     */
    shared_db *db  = shared_db_open_existing(path, &why);
    if (db == NULL) {
        take_message(message, words("Backup failed: %s", why != NULL ? why : "unknown error"));
        free(why);
        return cdb_backup_failed;
    }
    char *out = NULL;                /* the new backup's path               */
    char *err = NULL;                /* the module's message                */
    int   rc  = periodic
                ? shared_db_backup_if_due(db, time(NULL), &out, &err)
                : (shared_db_backup(db, &out, &err) == 0 ? 1 : -1);
    shared_db_close(db);

    cdb_backup_result result;
    char             *text = NULL;
    if (rc > 0) {
        const char *slash = out != NULL ? strrchr(out, '/') : NULL;
        result = cdb_backup_made;
        text   = words("Backed up to %s", slash != NULL ? slash + 1 : "?");
    } else if (rc < 0 && out != NULL) {
        /* The copy was made and verified; what came after it (pruning the
         * oldest backups) failed — the module's message says both.        */
        result = cdb_backup_made_with_problem;
        text   = words("Backup made with a problem: %s", err != NULL ? err : "unknown error");
    } else if (rc < 0) {
        result = cdb_backup_failed;
        text   = words("Backup failed: %s", err != NULL ? err : "unknown error");
    } else {
        result = cdb_backup_none;
    }
    free(out);
    free(err);
    take_message(message, text);
    return result;
}

/*
 * cdb_policy_value — see cdb.h.
 */
int64_t
cdb_policy_value(shared_db *db, const char *key)
{
    int64_t value = 0;
    shared_backup_policy_get(db, key, &value, NULL);
    return value;
}

/*
 * cdb_policy_store — see cdb.h.
 */
int
cdb_policy_store(shared_db *db, const char *key, int64_t value, char **err)
{
    char text[32];                   /* the value as shared_settings holds it */
    snprintf(text, sizeof text, "%lld", (long long)value);
    return shared_settings_set(db, key, text, err);
}

/*
 * normalize_path — an absolute path with "." and ".." resolved and
 * repeated or trailing slashes dropped, lexically.
 *
 * Inputs:
 *   path — an absolute path
 *
 * Output: the normalised path (free it); "/" for the root.
 */
static char *
normalize_path(const char *path)
{
    size_t  len = strlen(path);
    char   *out = malloc(len + 2);
    size_t  n   = 0;                 /* characters written to out           */
    out[n++] = '/';
    const char *p = path;
    while (*p != '\0') {
        while (*p == '/') {
            p++;
        }
        const char *end = p;
        while (*end != '\0' && *end != '/') {
            end++;
        }
        size_t part = (size_t)(end - p);
        if (part == 0 || (part == 1 && p[0] == '.')) {
            /* an empty or "." component: nothing                           */
        } else if (part == 2 && p[0] == '.' && p[1] == '.') {
            while (n > 1 && out[n - 1] != '/') {
                n--;
            }
            if (n > 1) {
                n--;                 /* the slash before the dropped part   */
            }
        } else {
            if (n > 1) {
                out[n++] = '/';
            }
            memcpy(out + n, p, part);
            n += part;
        }
        p = end;
    }
    out[n] = '\0';
    return out;
}

/*
 * cdb_path_inside — see cdb.h.
 */
bool
cdb_path_inside(const char *dir, const char *other)
{
    char  *a      = normalize_path(dir);
    char  *b      = normalize_path(other);
    size_t blen   = strlen(b);
    bool   inside = strcmp(a, b) == 0 ||
                    (strncmp(a, b, blen) == 0 && (blen == 1 || a[blen] == '/'));
    free(a);
    free(b);
    return inside;
}

/* ── Who has the file open, and what a crash leaves ──────────────────────── */

/*
 * pid_holds — whether a process has `path` or its -wal / -shm open.
 *
 * Inputs:
 *   pid  — the process
 *   path — the database file, as the kernel spells it (realpath)
 *
 * Output: true when it does; false also for a process that cannot be
 *         inspected (another user's).
 */
static bool
pid_holds(pid_t pid, const char *path)
{
    int size = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, NULL, 0);
    if (size <= 0) {
        return false;
    }
    struct proc_fdinfo *fds = malloc((size_t)size);
    size = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds, size);
    bool   holds = false;
    size_t len   = strlen(path);
    for (int i = 0; !holds && i < size / (int)sizeof fds[0]; i++) {
        if (fds[i].proc_fdtype != PROX_FDTYPE_VNODE) {
            continue;
        }
        struct vnode_fdinfowithpath info;
        if (proc_pidfdinfo(pid, fds[i].proc_fd, PROC_PIDFDVNODEPATHINFO, &info, sizeof info)
                != (int)sizeof info) {
            continue;
        }
        const char *open_path = info.pvip.vip_path;
        holds = strncmp(open_path, path, len) == 0 &&
                (open_path[len] == '\0' || strcmp(open_path + len, "-wal") == 0 ||
                 strcmp(open_path + len, "-shm") == 0);
    }
    free(fds);
    return holds;
}

/*
 * cdb_peers_read — see cdb.h.
 */
int
cdb_peers_read(const char *path, cdb_peer **out, size_t *count)
{
    *out   = NULL;
    *count = 0;
    char real[PATH_MAX];
    if (realpath(path, real) == NULL) {
        snprintf(real, sizeof real, "%s", path);
    }
    int n = proc_listallpids(NULL, 0);
    if (n <= 0) {
        return -1;
    }
    n += 16;                         /* processes started since the count   */
    pid_t *pids = calloc((size_t)n, sizeof *pids);
    n = proc_listallpids(pids, n * (int)sizeof *pids);
    if (n <= 0) {
        free(pids);
        return -1;
    }
    cdb_peer *peers = NULL;
    size_t    found = 0;
    for (int i = 0; i < n; i++) {
        if (pids[i] <= 0 || !pid_holds(pids[i], real)) {
            continue;
        }
        peers = realloc(peers, (found + 1) * sizeof *peers);
        peers[found].pid = pids[i];
        if (proc_name(pids[i], peers[found].name, sizeof peers[found].name) <= 0) {
            snprintf(peers[found].name, sizeof peers[found].name, "pid %d", (int)pids[i]);
        }
        found++;
    }
    free(pids);
    *out   = peers;
    *count = found;
    return 0;
}

/*
 * cdb_leftovers_read — see cdb.h.
 */
void
cdb_leftovers_read(shared_db *db, const char *path, int64_t now, cdb_leftovers *out)
{
    memset(out, 0, sizeof *out);
    char *wal = words("%s-wal", path);
    out->wal_bytes = cdb_file_size(wal);
    if (out->wal_bytes < 0) {
        out->wal_bytes = 0;
    }
    free(wal);
    out->claim_at   = cdb_policy_value(db, SHARED_SETTING_BACKUP_CLAIM_AT);
    out->claim_live = out->claim_at > 0 && out->claim_at <= now &&
                      now - out->claim_at < SHARED_BACKUP_CLAIM_LEASE_SEC;
    char *dir = shared_db_backup_dir(db, NULL);
    DIR  *d   = dir != NULL ? opendir(dir) : NULL;
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strncmp(e->d_name, SHARED_DB_BACKUP_TMP_PREFIX, strlen(SHARED_DB_BACKUP_TMP_PREFIX)) == 0) {
                out->unfinished_backups++;
            }
        }
        closedir(d);
    }
    free(dir);
}

/*
 * cdb_leftovers_words — see cdb.h.
 */
char *
cdb_leftovers_words(const cdb_leftovers *left, bool backing_up, int64_t now)
{
    char *text = strdup("");
    if (left->wal_bytes > 0) {
        char *line = words("The write-ahead log holds %lld KB not yet folded into the file; the next "
                           "writer folds it in.", (long long)((left->wal_bytes + 1023) / 1024));
        char *both = words("%s%s", text, line);
        free(text);
        free(line);
        text = both;
    }
    if (left->claim_at > 0 && !backing_up) {
        char *stamp = cdb_stamp(left->claim_at, now);
        char *line  = left->claim_live
            ? words("%sA backup claim from %s is held by no pass here: another process's, or one "
                    "that crashed; it expires after %d minutes.", text[0] ? "\n" : "", stamp,
                    SHARED_BACKUP_CLAIM_LEASE_SEC / 60)
            : words("%sAn expired backup claim from %s, left by a pass that never finished; the "
                    "next poll ignores it.", text[0] ? "\n" : "", stamp);
        char *both = words("%s%s", text, line);
        free(stamp);
        free(text);
        free(line);
        text = both;
    }
    if (left->unfinished_backups > 0) {
        char *line = words("%s%d unfinished backup folder%s left in the backup folder, swept once an hour old.",
                           text[0] ? "\n" : "", left->unfinished_backups,
                           left->unfinished_backups == 1 ? "" : "s");
        char *both = words("%s%s", text, line);
        free(text);
        free(line);
        text = both;
    }
    if (text[0] == '\0') {
        free(text);
        text = strdup("Nothing left behind: a crashed app's connection dies with it, its files closed "
                      "and its locks released by macOS.");
    }
    return text;
}

/* ── Words ───────────────────────────────────────────────────────────────── */

/*
 * cdb_stamp — see cdb.h.
 */
char *
cdb_stamp(int64_t when, int64_t now)
{
    time_t    when_t = (time_t)when;
    time_t    now_t  = (time_t)now;
    struct tm when_tm;
    struct tm now_tm;
    if (localtime_r(&when_t, &when_tm) == NULL) {
        return strdup("an unknown time");
    }
    bool today = localtime_r(&now_t, &now_tm) != NULL &&
                 when_tm.tm_year == now_tm.tm_year &&
                 when_tm.tm_yday == now_tm.tm_yday;
    return today ? words("%02d:%02d today", when_tm.tm_hour, when_tm.tm_min)
                 : words("%s %d at %02d:%02d", month_names[when_tm.tm_mon],
                         when_tm.tm_mday, when_tm.tm_hour, when_tm.tm_min);
}
