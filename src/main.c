#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include "app.h"
#include <errno.h>
#include <fcntl.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void usage(void) {
    fprintf(stderr, "mishmeret " APP_VERSION "\n"
                    "  --init --db FILE                  create four initial admin accounts\n"
                    "  --db FILE --web DIR --port PORT [--origin URL]   run loopback server\n"
                    "  --backup NEW_FILE --db FILE       consistent online SQLite backup\n"
                    "  --restore BACKUP --db FILE        restore while server is stopped\n");
}
static int lockdb(const char *path) {
    char *canonical = realpath(path, NULL);
    char newpath[4352];
    if (!canonical) {
        if (errno != ENOENT)
            return -1;
        struct stat linkinfo;
        /* A dangling final symlink must not choose a different lock for init. */
        if (lstat(path, &linkinfo) == 0 || errno != ENOENT)
            return -1;
        char *parts = strdup(path);
        if (!parts)
            return -1;
        char *slash = strrchr(parts, '/');
        const char *name = slash ? slash + 1 : parts;
        char *parent = NULL;
        if (slash) {
            *slash = '\0';
            parent = realpath(slash == parts ? "/" : parts, NULL);
        } else
            parent = realpath(".", NULL);
        int length = parent ? snprintf(newpath, sizeof newpath, "%s/%s", parent, name) : -1;
        int valid = parent && *name && strcmp(name, ".") && strcmp(name, "..") && length >= 0 &&
                    (size_t)length < sizeof newpath;
        free(parent);
        free(parts);
        if (!valid)
            return -1;
        canonical = strdup(newpath);
        if (!canonical)
            return -1;
    }
    struct stat info;
    if (stat(canonical, &info) == 0) {
        if (!S_ISREG(info.st_mode) || info.st_nlink > 1) {
            fprintf(stderr, "Database must be a regular file with one hard link\n");
            free(canonical);
            return -1;
        }
    } else if (errno != ENOENT) {
        free(canonical);
        return -1;
    }
    char lockpath[4352];
    int length = snprintf(lockpath, sizeof lockpath, "%s.lock", canonical);
    free(canonical);
    if (length < 0 || (size_t)length >= sizeof lockpath)
        return -1;
    int fd = open(lockpath, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1) {
        close(fd);
        return -1;
    }
    struct flock lock = {0};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &lock) < 0) {
        close(fd);
        fprintf(stderr, "Database is in use; stop its server first\n");
        return -1;
    }
    return fd;
}
static int valid_db(sqlite3 *db) {
    sqlite3_stmt *s = NULL;
    int valid = 0;
    sqlite3_exec(db, "PRAGMA trusted_schema=OFF", NULL, NULL, NULL);
    if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &s, NULL) == SQLITE_OK &&
        sqlite3_step(s) == SQLITE_ROW)
        valid = !strcmp((const char *)sqlite3_column_text(s, 0), "ok");
    sqlite3_finalize(s);
    s = NULL;
    if (!valid)
        return 0;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &s, NULL) != SQLITE_OK)
        return 0;
    valid = sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) == 1;
    sqlite3_finalize(s);
    if (!valid)
        return 0;
    s = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT 1 FROM sqlite_schema WHERE type IN('trigger','view') LIMIT 1",
                           -1, &s, NULL) != SQLITE_OK)
        return 0;
    valid = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    if (!valid)
        return 0;
    const char *queries[] = {
        "SELECT id,login,name,role,active,password,must_change,fail_count,fail_since FROM users "
        "LIMIT 0",
        "SELECT hash,user_id,csrf,expires FROM sessions LIMIT 0",
        "SELECT id,user_id,date,mask,location,resource_id FROM bookings LIMIT 0",
        "SELECT id,name,room,kind,active FROM resources LIMIT 0",
        "SELECT id,date,mask,location,reason FROM closures LIMIT 0",
        "SELECT id,date,end_date,title,body FROM announcements LIMIT 0",
        "SELECT id,at,user_id,action,target_id FROM audit LIMIT 0"};
    for (size_t i = 0; i < sizeof queries / sizeof queries[0]; i++) {
        s = NULL;
        if (sqlite3_prepare_v2(db, queries[i], -1, &s, NULL) != SQLITE_OK)
            return 0;
        valid = sqlite3_step(s) == SQLITE_DONE;
        sqlite3_finalize(s);
        if (!valid)
            return 0;
    }
    s = NULL;
    if (sqlite3_prepare_v2(db, "PRAGMA foreign_key_check", -1, &s, NULL) != SQLITE_OK)
        return 0;
    valid = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return valid;
}
static int backup_connection(sqlite3 *dst, sqlite3 *src) {
    sqlite3_backup *b = sqlite3_backup_init(dst, "main", src, "main");
    if (!b)
        return 0;
    int rc = SQLITE_OK, tries = 0;
    do {
        rc = sqlite3_backup_step(b, 256);
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            sqlite3_sleep(100);
            tries++;
        }
    } while ((rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED) && tries < 50);
    int fin = sqlite3_backup_finish(b);
    return rc == SQLITE_DONE && fin == SQLITE_OK;
}
static int copydb(const char *source, const char *target, int restore) {
    struct stat ss, ts;
    if (stat(source, &ss) != 0 || !S_ISREG(ss.st_mode) || ss.st_nlink > 1) {
        fprintf(stderr, "Source must be a regular database file with one hard link\n");
        return 0;
    }
    if (stat(target, &ts) == 0) {
        if (!S_ISREG(ts.st_mode) || ts.st_nlink > 1) {
            fprintf(stderr, "Target must be a regular database file with one hard link\n");
            return 0;
        }
        if (ss.st_dev == ts.st_dev && ss.st_ino == ts.st_ino) {
            fprintf(stderr, "Source and target are identical\n");
            return 0;
        }
        if (!restore) {
            fprintf(stderr, "Backup target already exists\n");
            return 0;
        }
    } else if (restore) {
        fprintf(stderr, "Restore target must exist\n");
        return 0;
    }
    int lock = -1;
    if (restore && (lock = lockdb(target)) < 0)
        return 0;
    sqlite3 *src = NULL, *dst = NULL, *staged = NULL;
    int success = 0, created = 0;
    if (sqlite3_open_v2(source, &src, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK || !valid_db(src))
        goto done;
    /* Validate and revoke restored sessions in a private temporary DB before
       touching the destination. SQLite backup commits the final copy atomically. */
    if (restore) {
        if (sqlite3_open("", &staged) != SQLITE_OK || !backup_connection(staged, src))
            goto done;
        if (!valid_db(staged) ||
            sqlite3_exec(staged, "DELETE FROM sessions", NULL, NULL, NULL) != SQLITE_OK ||
            !valid_db(staged))
            goto done;
    }
    if (!restore) {
        int fd = open(target, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd < 0)
            goto done;
        close(fd);
        created = 1;
    }
    if (sqlite3_open_v2(target, &dst, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK)
        goto done;
    sqlite3_busy_timeout(src, 1000);
    sqlite3_busy_timeout(dst, 1000);
    if (!backup_connection(dst, restore ? staged : src))
        goto done;
    success = valid_db(dst) && chmod(target, 0600) == 0;
done:
    if (!success)
        fprintf(stderr, "Database copy failed: source/schema/integrity or SQLite error\n");
    if (dst)
        sqlite3_close(dst);
    if (src)
        sqlite3_close(src);
    if (staged)
        sqlite3_close(staged);
    if (lock >= 0)
        close(lock);
    if (!success && created)
        unlink(target);
    return success;
}
int main(int argc, char **argv) {
    umask(0077);
    setenv("TZ", "Asia/Jerusalem", 1);
    tzset();
    if (sodium_init() < 0) {
        fprintf(stderr, "Cryptography initialization failed\n");
        return 1;
    }
    App a = {0};
    a.port = 8080;
    snprintf(a.web, sizeof a.web, "web");
    const char *db = "data/mishmeret.db", *backup = NULL, *restore = NULL;
    int init = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) {
            usage();
            return 0;
        }
        if (!strcmp(argv[i], "--version")) {
            puts(APP_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--init")) {
            init = 1;
            continue;
        }
        if (i + 1 >= argc) {
            usage();
            return 2;
        }
        const char *key = argv[i], *val = argv[++i];
        if (!strcmp(key, "--db"))
            db = val;
        else if (!strcmp(key, "--web")) {
            if (strlen(val) >= sizeof a.web)
                return 2;
            snprintf(a.web, sizeof a.web, "%s", val);
        } else if (!strcmp(key, "--origin")) {
            if (strlen(val) >= sizeof a.origin)
                return 2;
            snprintf(a.origin, sizeof a.origin, "%s", val);
        } else if (!strcmp(key, "--backup"))
            backup = val;
        else if (!strcmp(key, "--restore"))
            restore = val;
        else if (!strcmp(key, "--port")) {
            char *end;
            long p = strtol(val, &end, 10);
            if (*end || p < 1024 || p > 65535) {
                fprintf(stderr, "Port must be 1024–65535\n");
                return 2;
            }
            a.port = (unsigned short)p;
        } else {
            usage();
            return 2;
        }
    }
    if ((init ? 1 : 0) + (backup ? 1 : 0) + (restore ? 1 : 0) > 1) {
        usage();
        return 2;
    }
    if (backup)
        return copydb(db, backup, 0) ? 0 : 1;
    if (restore)
        return copydb(restore, db, 1) ? 0 : 1;
    if (!*a.origin)
        snprintf(a.origin, sizeof a.origin, "http://127.0.0.1:%u", (unsigned)a.port);
    a.secure = !strncmp(a.origin, "https://", 8);
    const char *host = a.origin + (a.secure ? 8 : 7);
    if ((!a.secure && strncmp(a.origin, "http://", 7)) || !*host ||
        strpbrk(host, "/\\@?# \t\r\n")) {
        fprintf(stderr, "Origin must be a complete http(s) origin without a trailing slash\n");
        return 2;
    }
    if (!a.secure) {
        char allowed[80];
        snprintf(allowed, sizeof allowed, "http://127.0.0.1:%u", (unsigned)a.port);
        if (strcmp(a.origin, allowed)) {
            snprintf(allowed, sizeof allowed, "http://localhost:%u", (unsigned)a.port);
            if (strcmp(a.origin, allowed)) {
                fprintf(stderr, "Remote origins require HTTPS\n");
                return 2;
            }
        }
    }
    int lock = lockdb(db);
    if (lock < 0)
        return 1;
    if (!db_open(&a, db, init)) {
        fprintf(stderr, "Cannot open database: initialize it first with --init --db FILE\n");
        db_close(&a);
        close(lock);
        return 1;
    }
    int success;
    if (init)
        success = bootstrap(&a);
    else if (!valid_db(a.db)) {
        fprintf(stderr, "Unsupported or damaged database\n");
        success = 0;
    } else {
        char tools[4352], examples[4352], error[256];
        int nt = snprintf(tools, sizeof tools, "%s/../wolfe/tools.json", a.web);
        int ne = snprintf(examples, sizeof examples, "%s/../wolfe/examples.he.jsonl", a.web);
        if (nt < 0 || ne < 0 || (size_t)nt >= sizeof tools || (size_t)ne >= sizeof examples) {
            fprintf(stderr, "Wolfe configuration path is too long\n");
            success = 0;
        } else {
            a.wolf = wolfe_load(tools, examples, NULL, error, sizeof error);
            if (!a.wolf) {
                fprintf(stderr, "Cannot load Wolfe: %s\n", error);
                success = 0;
            } else {
                /* libmicrohttpd uses one request thread; Wolfe calls are serialized. */
                success = serve(&a);
                wolfe_free(a.wolf);
            }
        }
    }
    if (!success && init)
        fprintf(stderr, "Initialization failed or database already initialized: %s\n",
                sqlite3_errmsg(a.db));
    db_close(&a);
    close(lock);
    return success ? 0 : 1;
}
