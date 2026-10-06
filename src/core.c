#include "app.h"
#include <ctype.h>
#include <arpa/inet.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static void js(json_object *o, const char *k, const char *v) {
    json_object_object_add(o, k, json_object_new_string(v ? v : ""));
}
static void ji(json_object *o, const char *k, int64_t v) {
    json_object_object_add(o, k, json_object_new_int64(v));
}
static Result result(int status, json_object *j) {
    Result r = {0};
    r.status = status;
    r.json = j;
    return r;
}
Result fail(int status, const char *code, const char *message) {
    json_object *o = json_object_new_object();
    js(o, "error", code);
    js(o, "message", message);
    return result(status, o);
}
static Result ok(void) {
    json_object *o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(1));
    return result(200, o);
}
static Result dberr(App *a) {
    fprintf(stderr, "database error: %s\n", sqlite3_errmsg(a->db));
    return fail(500, "database_error", "Database operation failed");
}
static sqlite3_stmt *stmt(App *a, const char *sql) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(a->db, sql, -1, &s, NULL) != SQLITE_OK)
        return NULL;
    return s;
}
static void bind_text(sqlite3_stmt *s, int n, const char *v) {
    sqlite3_bind_text(s, n, v, -1, SQLITE_TRANSIENT);
}
static int run(sqlite3_stmt *s) {
    if (!s)
        return 0;
    int r = sqlite3_step(s);
    sqlite3_finalize(s);
    return r == SQLITE_DONE;
}
static int execsql(App *a, const char *sql) {
    return sqlite3_exec(a->db, sql, NULL, NULL, NULL) == SQLITE_OK;
}
static const char *col(sqlite3_stmt *s, int n) {
    const unsigned char *p = sqlite3_column_text(s, n);
    return p ? (const char *)p : "";
}
static const char *str(json_object *o, const char *k, size_t max) {
    json_object *v = NULL;
    if (!o || !json_object_object_get_ex(o, k, &v) || !json_object_is_type(v, json_type_string))
        return NULL;
    const char *s = json_object_get_string(v);
    size_t n = (size_t)json_object_get_string_len(v);
    if (n > max || strlen(s) != n)
        return NULL;
    return s;
}
static int num(json_object *o, const char *k) {
    json_object *v = NULL;
    if (!o || !json_object_object_get_ex(o, k, &v) || !json_object_is_type(v, json_type_int))
        return -1;
    int64_t n = json_object_get_int64(v);
    return n > 0 && n <= 2147483647 ? (int)n : -1;
}
static int boolean(json_object *o, const char *k) {
    json_object *v = NULL;
    if (!o || !json_object_object_get_ex(o, k, &v) || !json_object_is_type(v, json_type_boolean))
        return -1;
    return json_object_get_boolean(v);
}
static int textvalid(const char *s, int nonempty) {
    if (!s || (nonempty && !*s))
        return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 32 && *p != '\n' && *p != '\t')
            return 0;
    return 1;
}
static int textwithin(const char *s, size_t max) {
    if (!s)
        return 0;
    size_t count = 0;
    /* The HTTP JSON parser validates UTF-8; count each code point's first byte. */
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xc0) != 0x80 && ++count > max)
            return 0;
    return 1;
}
static int loginvalid(const char *s) {
    if (!s || !*s)
        return 0;
    for (const char *p = s; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || isdigit((unsigned char)*p) || *p == '_' || *p == '-' ||
              *p == '.'))
            return 0;
    return 1;
}
static int periodmask(const char *s) {
    if (!s)
        return 0;
    return !strcmp(s, "morning") ? 1 : !strcmp(s, "afternoon") ? 2 : !strcmp(s, "full") ? 3 : 0;
}
static int locationvalid(const char *s, int all) {
    return s && (!strcmp(s, "home") || !strcmp(s, "center") || (all && !strcmp(s, "all")));
}
static int datevalid(const char *s, int *wday) {
    if (!s || strlen(s) != 10 || s[4] != '-' || s[7] != '-')
        return 0;
    for (int i = 0; i < 10; i++)
        if (i != 4 && i != 7 && !isdigit((unsigned char)s[i]))
            return 0;
    int y, m, d;
    if (sscanf(s, "%4d-%2d-%2d", &y, &m, &d) != 3 || y < 2020 || y > 2199 || m < 1 || m > 12 ||
        d < 1 || d > 31)
        return 0;
    struct tm t = {0};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    t.tm_hour = 12;
    t.tm_isdst = -1;
    time_t val = mktime(&t);
    if (val == (time_t)-1 || t.tm_year != y - 1900 || t.tm_mon != m - 1 || t.tm_mday != d)
        return 0;
    if (wday)
        *wday = t.tm_wday;
    return 1;
}
static void today(char out[11]) {
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    strftime(out, 11, "%Y-%m-%d", &t);
}
static int bookdate(const char *s) {
    int day;
    char now[11];
    today(now);
    return datevalid(s, &day) && day <= 4 && strcmp(s, now) >= 0;
}
static int adddays(const char *base, int days, char out[11]) {
    int y, m, d;
    if (!datevalid(base, NULL) || sscanf(base, "%4d-%2d-%2d", &y, &m, &d) != 3)
        return 0;
    struct tm t = {0};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d + days;
    t.tm_hour = 12;
    t.tm_isdst = -1;
    return mktime(&t) != (time_t)-1 && strftime(out, 11, "%Y-%m-%d", &t) == 10 &&
           datevalid(out, NULL);
}
static void hexrandom(char out[65]) {
    unsigned char b[32];
    randombytes_buf(b, sizeof b);
    sodium_bin2hex(out, 65, b, sizeof b);
    sodium_memzero(b, sizeof b);
}
static void digest(const char *s, char out[65]) {
    unsigned char hash[32];
    crypto_generichash(hash, sizeof hash, (const unsigned char *)s, strlen(s), NULL, 0);
    sodium_bin2hex(out, 65, hash, sizeof hash);
}
static void newpassword(char out[33]) {
    unsigned char b[16];
    randombytes_buf(b, sizeof b);
    sodium_bin2hex(out, 33, b, sizeof b);
    sodium_memzero(b, sizeof b);
}
static int hashpassword(const char *pw, char out[crypto_pwhash_STRBYTES]) {
    return crypto_pwhash_str(out, pw, strlen(pw), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                             crypto_pwhash_MEMLIMIT_INTERACTIVE) == 0;
}
int db_open(App *a, const char *path, int create) {
    int flags = SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0);
    if (sqlite3_open_v2(path, &a->db, flags, NULL) != SQLITE_OK)
        return 0;
    if (chmod(path, 0600) != 0)
        return 0;
    sqlite3_busy_timeout(a->db, 5000);
    return execsql(a, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; "
                      "PRAGMA trusted_schema=OFF;");
}
void db_close(App *a) {
    if (a->db)
        sqlite3_close(a->db);
    a->db = NULL;
}
int bootstrap(App *a) {
    const char *schema =
        "BEGIN IMMEDIATE;"
        "CREATE TABLE users(id INTEGER PRIMARY KEY,login TEXT UNIQUE NOT NULL,name TEXT NOT "
        "NULL,role TEXT NOT NULL CHECK(role IN('admin','member')),active INTEGER NOT NULL DEFAULT "
        "1 CHECK(active IN(0,1)),password TEXT NOT NULL,must_change INTEGER NOT NULL DEFAULT "
        "1,fail_count INTEGER NOT NULL DEFAULT 0,fail_since INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE sessions(hash TEXT PRIMARY KEY,user_id INTEGER NOT NULL REFERENCES users(id) "
        "ON DELETE CASCADE,csrf TEXT NOT NULL,expires INTEGER NOT NULL);"
        "CREATE INDEX sessions_user ON sessions(user_id);"
        "CREATE TABLE resources(id INTEGER PRIMARY KEY,name TEXT NOT NULL,room TEXT NOT NULL,kind "
        "TEXT NOT NULL CHECK(kind IN('desk','room')),active INTEGER NOT NULL DEFAULT "
        "1,UNIQUE(room,name));"
        "CREATE TABLE bookings(id INTEGER PRIMARY KEY,user_id INTEGER NOT NULL REFERENCES "
        "users(id),date TEXT NOT NULL,mask INTEGER NOT NULL CHECK(mask BETWEEN 1 AND 3),location "
        "TEXT NOT NULL CHECK(location IN('home','center')),resource_id INTEGER REFERENCES "
        "resources(id),UNIQUE(user_id,date),CHECK(location='center' OR resource_id IS NULL));"
        "CREATE INDEX bookings_date ON bookings(date);"
        "CREATE TABLE closures(id INTEGER PRIMARY KEY,date TEXT NOT NULL,mask INTEGER NOT NULL "
        "CHECK(mask BETWEEN 1 AND 3),location TEXT NOT NULL CHECK(location "
        "IN('all','home','center')),reason TEXT NOT NULL,UNIQUE(date,mask,location));"
        "CREATE TABLE announcements(id INTEGER PRIMARY KEY,date TEXT NOT NULL,end_date TEXT NOT "
        "NULL,title TEXT NOT NULL,body TEXT NOT NULL);"
        "CREATE TABLE audit(id INTEGER PRIMARY KEY,at TEXT NOT NULL DEFAULT "
        "CURRENT_TIMESTAMP,user_id INTEGER,action TEXT NOT NULL,target_id INTEGER);"
        "PRAGMA user_version=1;";
    if (!execsql(a, schema)) {
        execsql(a, "ROLLBACK");
        return 0;
    }
    const char *logins[] = {"oleg2", "oleg1", "shira", "reut"};
    const char *names[] = {"אולג 2", "אולג 1", "שירה", "ראות"};
    json_object *out = json_object_new_object(), *accounts = json_object_new_array();
    json_object_object_add(out, "accounts", accounts);
    for (int i = 0; i < 4; i++) {
        char pw[33], hash[crypto_pwhash_STRBYTES];
        newpassword(pw);
        if (!hashpassword(pw, hash))
            goto bad;
        sqlite3_stmt *s =
            stmt(a, "INSERT INTO users(login,name,role,password) VALUES(?,?,'admin',?)");
        if (!s)
            goto bad;
        bind_text(s, 1, logins[i]);
        bind_text(s, 2, names[i]);
        bind_text(s, 3, hash);
        if (!run(s))
            goto bad;
        json_object *u = json_object_new_object();
        js(u, "login", logins[i]);
        js(u, "name", names[i]);
        js(u, "password", pw);
        json_object_array_add(accounts, u);
        sodium_memzero(pw, sizeof pw);
    }
    if (!execsql(a, "INSERT INTO resources(name,room,kind) VALUES('שולחן 1 — לדוגמה','כיתה "
                    "לדוגמה','desk'),('שולחן 2 — לדוגמה','כיתה לדוגמה','desk'),('כל החדר — "
                    "לדוגמה','כיתה לדוגמה','room'); COMMIT;"))
        goto bad;
    puts(json_object_to_json_string_ext(out, JSON_C_TO_STRING_PRETTY));
    json_object_put(out);
    return 1;
bad:
    execsql(a, "ROLLBACK");
    json_object_put(out);
    return 0;
}
static void audit(App *a, int uid, const char *action, int target) {
    sqlite3_stmt *s = stmt(a, "INSERT INTO audit(user_id,action,target_id) VALUES(?,?,?)");
    if (s) {
        sqlite3_bind_int(s, 1, uid);
        bind_text(s, 2, action);
        sqlite3_bind_int(s, 3, target);
        if (!run(s))
            fprintf(stderr, "audit write failed\n");
    }
}
static int identity(App *a, struct MHD_Connection *c, Identity *u) {
    memset(u, 0, sizeof *u);
    const char *token = MHD_lookup_connection_value(c, MHD_COOKIE_KIND, "mishmeret_session");
    if (!token || strlen(token) != 64)
        return 0;
    for (const char *p = token; *p; p++)
        if (!isxdigit((unsigned char)*p))
            return 0;
    digest(token, u->session_hash);
    sqlite3_stmt *s =
        stmt(a, "SELECT u.id,u.login,u.name,u.role,u.must_change,s.csrf FROM sessions s JOIN users "
                "u ON u.id=s.user_id WHERE s.hash=? AND s.expires>? AND u.active=1");
    if (!s)
        return 0;
    bind_text(s, 1, u->session_hash);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)time(NULL));
    if (sqlite3_step(s) == SQLITE_ROW) {
        u->id = sqlite3_column_int(s, 0);
        snprintf(u->login, sizeof u->login, "%s", col(s, 1));
        snprintf(u->name, sizeof u->name, "%s", col(s, 2));
        u->admin = !strcmp(col(s, 3), "admin");
        u->must_change = sqlite3_column_int(s, 4);
        snprintf(u->csrf, sizeof u->csrf, "%s", col(s, 5));
    }
    sqlite3_finalize(s);
    return u->id > 0;
}
static Result session(const Identity *u) {
    json_object *o = json_object_new_object();
    if (!u->id) {
        json_object_object_add(o, "user", NULL);
        return result(200, o);
    }
    json_object *j = json_object_new_object();
    ji(j, "id", u->id);
    js(j, "login", u->login);
    js(j, "name", u->name);
    js(j, "role", u->admin ? "admin" : "member");
    json_object_object_add(j, "must_change_password", json_object_new_boolean(u->must_change));
    json_object_object_add(o, "user", j);
    js(o, "csrf", u->csrf);
    return result(200, o);
}
/* Single HTTP worker owns these buckets. No sleeps and no account-wide lock.
   The opt-in proxy mode is for Tailscale Serve/Funnel, which REPLACES XFF.
   Never enable it behind a proxy that merely appends an untrusted header. */
static int hash_budget(App *a, struct MHD_Connection *c, double cost) {
    unsigned char key[17] = {0};
    const union MHD_ConnectionInfo *info =
        MHD_get_connection_info(c, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
    if (!info || !info->client_addr)
        return 0;
    const struct sockaddr *addr = info->client_addr;
    int loopback = 0;
    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)addr;
        key[0] = 4;
        memcpy(key + 1, &v4->sin_addr, 4);
        loopback = key[1] == 127;
    } else if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)addr;
        key[0] = 6;
        memcpy(key + 1, &v6->sin6_addr, 16);
        loopback = IN6_IS_ADDR_LOOPBACK(&v6->sin6_addr);
    } else
        return 0;
    if (a->trust_tailscale_proxy && loopback) {
        const char *forwarded = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "X-Forwarded-For");
        if (forwarded) {
            unsigned char raw[16] = {0};
            int family = inet_pton(AF_INET, forwarded, raw) == 1 ? 4 :
                         inet_pton(AF_INET6, forwarded, raw) == 1 ? 6 : 0;
            if (!family) return 0; /* Lists, ports and malformed values are not Tailscale XFF. */
            memset(key, 0, sizeof key);
            key[0] = (unsigned char)family;
            memcpy(key + 1, raw, family == 4 ? 4u : 16u);
        }
    }
    /* Canonicalize mapped IPv4; group IPv6 clients by /64. */
    if (key[0] == 6) {
        static const unsigned char mapped[12] = {0,0,0,0,0,0,0,0,0,0,255,255};
        if (!memcmp(key + 1, mapped, sizeof mapped)) {
            memmove(key + 1, key + 13, 4);
            memset(key + 5, 0, 12);
            key[0] = 4;
        } else
            memset(key + 9, 0, 8);
    }
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
    HashBucket *bucket = NULL, *reusable = NULL;
    for (size_t i = 0; i < sizeof a->hash_buckets / sizeof a->hash_buckets[0]; i++) {
        HashBucket *candidate = &a->hash_buckets[i];
        if (candidate->key[0] && !memcmp(candidate->key, key, sizeof key)) {
            bucket = candidate;
            break;
        }
        if (!reusable && (!candidate->key[0] || now - candidate->updated >= 100.0))
            reusable = candidate;
    }
    if (!bucket) {
        if (!(bucket = reusable)) return 0; /* Never evict an active client's limit. */
        memcpy(bucket->key, key, sizeof key);
        bucket->tokens = 20.0;
        bucket->updated = now;
    }
    bucket->tokens += (now - bucket->updated) * 0.2;
    if (bucket->tokens > 20.0) bucket->tokens = 20.0;
    bucket->updated = now;
    if (bucket->tokens < cost) return 0;
    bucket->tokens -= cost;
    return 1;
}
static Result login(App *a, struct MHD_Connection *c, json_object *j) {
    const char *name = str(j, "login", 64), *pw = str(j, "password", 128);
    if (!name || !pw || !*pw)
        return fail(400, "invalid_login", "Login and password are required");
    if (!hash_budget(a, c, 1.0))
        return fail(429, "busy", "Too many attempts; retry shortly");
    sqlite3_stmt *s =
        stmt(a, "SELECT id,password,active FROM users WHERE login=?");
    if (!s)
        return dberr(a);
    bind_text(s, 1, name);
    int uid = 0, valid = 0, password_valid = 0, rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
        uid = sqlite3_column_int(s, 0);
        password_valid = crypto_pwhash_str_verify(col(s, 1), pw, strlen(pw)) == 0;
        valid = password_valid && sqlite3_column_int(s, 2);
    } else if (rc == SQLITE_DONE) {
        /* Unknown names consume the same KDF budget but cannot evict counters. */
        char dummy[crypto_pwhash_STRBYTES];
        if (!hashpassword(pw, dummy)) {
            sqlite3_finalize(s);
            return fail(503, "unavailable", "Try again later");
        }
        sodium_memzero(dummy, sizeof dummy);
    } else {
        sqlite3_finalize(s);
        return dberr(a);
    }
    sqlite3_finalize(s);
    if (!valid)
        return fail(401, "invalid_credentials", "Invalid login or password");
    char token[65], hash[65], csrf[65];
    hexrandom(token);
    digest(token, hash);
    hexrandom(csrf);
    s = stmt(a, "DELETE FROM sessions WHERE expires<=?");
    if (!s)
        return dberr(a);
    sqlite3_bind_int64(s, 1, (sqlite3_int64)time(NULL));
    if (!run(s))
        return dberr(a);
    s = stmt(a, "INSERT INTO sessions(hash,user_id,csrf,expires) VALUES(?,?,?,?)");
    if (!s)
        return dberr(a);
    bind_text(s, 1, hash);
    sqlite3_bind_int(s, 2, uid);
    bind_text(s, 3, csrf);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)time(NULL) + 28800);
    if (!run(s))
        return dberr(a);
    Identity u = {0};
    u.id = uid;
    s = stmt(a, "SELECT login,name,role,must_change FROM users WHERE id=?");
    if (!s)
        return dberr(a);
    sqlite3_bind_int(s, 1, uid);
    if (sqlite3_step(s) == SQLITE_ROW) {
        snprintf(u.login, sizeof u.login, "%s", col(s, 0));
        snprintf(u.name, sizeof u.name, "%s", col(s, 1));
        u.admin = !strcmp(col(s, 2), "admin");
        u.must_change = sqlite3_column_int(s, 3);
    }
    sqlite3_finalize(s);
    snprintf(u.csrf, sizeof u.csrf, "%s", csrf);
    Result r = session(&u);
    snprintf(r.cookie, sizeof r.cookie,
             "mishmeret_session=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=28800%s", token,
             a->secure ? "; Secure" : "");
    sodium_memzero(token, sizeof token);
    audit(a, uid, "login", uid);
    return r;
}
static Result password(App *a, struct MHD_Connection *c, const Identity *u, json_object *j) {
    const char *old = str(j, "old_password", 128), *pw = str(j, "new_password", 128);
    if (!old || !pw || textwithin(pw, 11) || !strcmp(old, pw))
        return fail(400, "invalid_password",
                    "Use a new password of at least 12 characters, up to 128 UTF-8 bytes");
    if (!hash_budget(a, c, 2.0))
        return fail(429, "busy", "Too many attempts; retry shortly");
    sqlite3_stmt *s = stmt(a, "SELECT password FROM users WHERE id=?");
    if (!s)
        return dberr(a);
    sqlite3_bind_int(s, 1, u->id);
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        return dberr(a);
    }
    int verified = crypto_pwhash_str_verify(col(s, 0), old, strlen(old)) == 0;
    sqlite3_finalize(s);
    if (!verified)
        return fail(403, "wrong_password", "Current password is incorrect");
    char hash[crypto_pwhash_STRBYTES], token[65], session_hash[65], csrf[65];
    if (!hashpassword(pw, hash))
        return fail(503, "unavailable", "Try again later");
    if (!execsql(a, "BEGIN IMMEDIATE"))
        return dberr(a);
    /* Generate session secrets only after the KDF and BEGIN have succeeded. */
    hexrandom(token);
    digest(token, session_hash);
    hexrandom(csrf);
    s = stmt(a, "UPDATE users SET password=?,must_change=0 WHERE id=?");
    if (!s)
        goto bad;
    bind_text(s, 1, hash);
    sqlite3_bind_int(s, 2, u->id);
    if (!run(s))
        goto bad;
    s = stmt(a, "DELETE FROM sessions WHERE user_id=? AND hash!=?");
    if (!s)
        goto bad;
    sqlite3_bind_int(s, 1, u->id);
    bind_text(s, 2, u->session_hash);
    if (!run(s))
        goto bad;
    s = stmt(a, "UPDATE sessions SET hash=?,csrf=?,expires=? WHERE hash=?");
    if (!s) goto bad;
    bind_text(s, 1, session_hash);
    bind_text(s, 2, csrf);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)time(NULL) + 28800);
    bind_text(s, 4, u->session_hash);
    if (!run(s)) goto bad;
    if (!execsql(a, "COMMIT"))
        goto bad;
    audit(a, u->id, "password_changed", u->id);
    Result r = ok();
    js(r.json, "csrf", csrf);
    snprintf(r.cookie, sizeof r.cookie,
             "mishmeret_session=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=28800%s",
             token, a->secure ? "; Secure" : "");
    sodium_memzero(token, sizeof token);
    return r;
bad:
    sodium_memzero(token, sizeof token);
    execsql(a, "ROLLBACK");
    return dberr(a);
}

static json_object *rows(sqlite3_stmt *s) {
    if (!s)
        return NULL;
    json_object *array = json_object_new_array();
    int rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        json_object *o = json_object_new_object();
        for (int i = 0; i < sqlite3_column_count(s); i++) {
            const char *key = sqlite3_column_name(s, i);
            json_object *v = NULL;
            int t = sqlite3_column_type(s, i);
            if (t == SQLITE_INTEGER) {
                if (!strcmp(key, "active") || !strcmp(key, "must_change_password") ||
                    !strcmp(key, "blocked"))
                    v = json_object_new_boolean(sqlite3_column_int(s, i));
                else
                    v = json_object_new_int64(sqlite3_column_int64(s, i));
            } else if (t != SQLITE_NULL)
                v = json_object_new_string(col(s, i));
            json_object_object_add(o, key, v);
        }
        json_object_array_add(array, o);
    }
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) {
        json_object_put(array);
        return NULL;
    }
    return array;
}
static Result wolfe_command(App *a, json_object *j) {
    const char *text = str(j, "text", 512), *start = str(j, "week_start", 10);
    int weekday, has_text = 0;
    if (text)
        for (const unsigned char *p = (const unsigned char *)text; *p; p++)
            if (*p > 32)
                has_text = 1;
    if (!has_text || !textvalid(text, 1) || !datevalid(start, &weekday) || weekday != 0)
        return fail(400, "invalid_command", "כתבו בקשה של עד 512 בתים ובחרו שבוע תקין.");

    char buffer[4096];
    if (wolfe_call(a->wolf, text, WOLFE_NEURAL, WOLFE_REASONING_OFF, buffer, sizeof buffer, NULL) !=
        WOLFE_OK)
        return fail(400, "wolfe_input", "נסו בקשה קצרה יותר עם יום, זמן ומקום.");
    json_object *decision = json_tokener_parse(buffer);
    const char *status = str(decision, "status", 32);
    if (!status || (strcmp(status, "call") && strcmp(status, "no_call") &&
                    strcmp(status, "ambiguous") && strcmp(status, "missing_arguments"))) {
        json_object_put(decision);
        return fail(500, "wolfe_error", "לא התקבלה תשובה תקינה מאיציק.");
    }
    json_object *out = json_object_new_object();
    js(out, "engine", "wolfe");
    js(out, "status", status);
    json_object_object_add(out, "action", NULL);
    json_object_object_add(out, "proposal", NULL);
    js(out, "message",
       !strcmp(status, "ambiguous")           ? "יש כמה אפשרויות. נסו לפרט את הבקשה."
       : !strcmp(status, "missing_arguments") ? "צריך לציין יום, זמן והאם לומדים מהבית או בחממה."
                                              : "לא נבחרה פעולה. אפשר לנסח בקשה חדשה.");
    if (strcmp(status, "call")) {
        json_object *missing = NULL;
        if (json_object_object_get_ex(decision, "missing", &missing) &&
            json_object_is_type(missing, json_type_array)) {
            json_object_object_add(out, "missing", json_object_get(missing));
            if (json_object_array_length(missing) == 1) {
                const char *field = json_object_get_string(json_object_array_get_idx(missing, 0));
                if (field && !strcmp(field, "location"))
                    js(out, "message", "איפה לומדים — מהבית או בחממה?");
                else if (field && !strcmp(field, "period"))
                    js(out, "message", "מתי — בבוקר, אחר הצהריים או יום מלא?");
                else if (field && !strcmp(field, "day"))
                    js(out, "message", "באיזה יום — ראשון עד חמישי, היום או מחר?");
            }
        }
        json_object_put(decision);
        return result(200, out);
    }
    json_object *calls = NULL, *args = NULL;
    if (!json_object_object_get_ex(decision, "calls", &calls) ||
        !json_object_is_type(calls, json_type_array) || json_object_array_length(calls) != 1)
        goto invalid;
    json_object *call = json_object_array_get_idx(calls, 0);
    const char *name = str(call, "name", 40);
    if (!name || !json_object_object_get_ex(call, "arguments", &args) ||
        !json_object_is_type(args, json_type_object))
        goto invalid;
    if (!strcmp(name, "show_schedule") || !strcmp(name, "show_my_bookings")) {
        if (json_object_object_length(args) != 0)
            goto invalid;
        js(out, "action", name);
        js(out, "message", !strcmp(name, "show_schedule") ? "הלוח של כולם." : "המשמרות שלך.");
    } else if (!strcmp(name, "propose_booking")) {
        const char *day = str(args, "day", 16), *period = str(args, "period", 12),
                   *loc = str(args, "location", 8), *which = str(args, "week", 8);
        int mask = periodmask(period);
        if (!day || !mask || !locationvalid(loc, 0) ||
            (which && strcmp(which, "selected") && strcmp(which, "next")))
            goto invalid;
        int next = which && !strcmp(which, "next");
        char date[11], now[11];
        today(now);
        if (!strcmp(day, "today") || !strcmp(day, "tomorrow")) {
            if (next) {
                js(out, "status", "ambiguous");
                js(out, "message", "לבקשה בשבוע הבא צריך לציין את היום בשבוע.");
                json_object_put(decision);
                return result(200, out);
            }
            if (!adddays(now, !strcmp(day, "tomorrow"), date))
                goto invalid;
        } else {
            const char *days[] = {"sunday", "monday", "tuesday", "wednesday", "thursday"};
            int offset = -1;
            for (int i = 0; i < 5; i++)
                if (!strcmp(day, days[i]))
                    offset = i;
            if (offset < 0 || !adddays(start, offset + (next ? 7 : 0), date))
                goto invalid;
        }
        if (!bookdate(date)) {
            json_object_put(out);
            json_object_put(decision);
            return fail(409, "date_unavailable", "בחרו יום ראשון עד חמישי, מהיום והלאה.");
        }
        sqlite3_stmt *s = stmt(a, "SELECT id FROM closures WHERE date=? AND (mask & ?)!=0 "
                                  "AND (location='all' OR location=?) LIMIT 1");
        if (!s) {
            json_object_put(out);
            json_object_put(decision);
            return dberr(a);
        }
        bind_text(s, 1, date);
        sqlite3_bind_int(s, 2, mask);
        bind_text(s, 3, loc);
        int rc = sqlite3_step(s);
        sqlite3_finalize(s);
        if (rc != SQLITE_DONE) {
            json_object_put(out);
            json_object_put(decision);
            return rc == SQLITE_ROW ? fail(409, "shift_closed", "המשמרת הזו סגורה להרשמה.")
                                    : dberr(a);
        }
        json_object *proposal = json_object_new_object();
        js(proposal, "date", date);
        js(proposal, "period", period);
        js(proposal, "location", loc);
        json_object_object_add(proposal, "resource_id", NULL);
        js(out, "action", name);
        json_object_object_add(out, "proposal", proposal);
        js(out, "message", "בדקו את התאריך והמשמרת ואשרו כדי לשמור.");
    } else
        goto invalid;
    json_object_put(decision);
    return result(200, out);
invalid:
    json_object_put(out);
    json_object_put(decision);
    return fail(500, "wolfe_error", "לא התקבלה פעולה תקינה מאיציק.");
}

static Result week(App *a, struct MHD_Connection *c) {
    const char *start = MHD_lookup_connection_value(c, MHD_GET_ARGUMENT_KIND, "start");
    int weekday;
    if (!datevalid(start, &weekday) || weekday != 0)
        return fail(400, "invalid_date", "Week must start on Sunday (YYYY-MM-DD)");
    json_object *out = json_object_new_object();
    js(out, "start", start);
    const char *sql[] = {
        "SELECT b.id,b.user_id,u.name,b.date,CASE b.mask WHEN 1 THEN 'morning' WHEN 2 THEN "
        "'afternoon' ELSE 'full' END AS period,b.location,b.resource_id,r.name AS "
        "resource_name,r.room,EXISTS(SELECT 1 FROM closures c WHERE c.date=b.date AND (c.mask & "
        "b.mask)!=0 AND (c.location='all' OR c.location=b.location)) AS blocked FROM bookings b "
        "JOIN users u ON u.id=b.user_id LEFT JOIN resources r ON r.id=b.resource_id WHERE "
        "b.date>=? AND b.date<=date(?,'+6 days') ORDER BY b.date,b.mask,u.name",
        "SELECT id,date,CASE mask WHEN 1 THEN 'morning' WHEN 2 THEN 'afternoon' ELSE 'full' END AS "
        "period,location,reason FROM closures WHERE date>=? AND date<=date(?,'+6 days') ORDER BY "
        "date,id",
        ("SELECT id,date,end_date,title,body FROM announcements WHERE end_date>=? AND "
         "date<=date(?,'+6 days') ORDER BY date,id"),
        "SELECT id,name,room,kind,active FROM resources ORDER BY room,id"};
    const char *keys[] = {"bookings", "closures", "announcements", "resources"};
    for (int i = 0; i < 4; i++) {
        sqlite3_stmt *s = stmt(a, sql[i]);
        if (!s)
            goto bad;
        if (i < 3) {
            bind_text(s, 1, start);
            bind_text(s, 2, start);
        }
        json_object *array = rows(s);
        if (!array)
            goto bad;
        json_object_object_add(out, keys[i], array);
    }
    return result(200, out);
bad:
    json_object_put(out);
    return dberr(a);
}
static int queryid(struct MHD_Connection *c) {
    const char *s = MHD_lookup_connection_value(c, MHD_GET_ARGUMENT_KIND, "id");
    if (!s || !*s || strlen(s) > 10)
        return -1;
    for (const char *p = s; *p; p++)
        if (!isdigit((unsigned char)*p))
            return -1;
    char *end;
    long n = strtol(s, &end, 10);
    return *end == 0 && n > 0 && n <= 2147483647 ? (int)n : -1;
}
static Result book(App *a, const Identity *u, json_object *j) {
    const char *date = str(j, "date", 10), *period = str(j, "period", 12),
               *loc = str(j, "location", 8);
    int mask = periodmask(period), rid = 0;
    json_object *resource = NULL;
    if (json_object_object_get_ex(j, "resource_id", &resource) && resource) {
        rid = num(j, "resource_id");
        if (rid < 1)
            return fail(400, "invalid_resource", "Invalid resource");
    }
    if (!bookdate(date) || !mask || !locationvalid(loc, 0))
        return fail(400, "invalid_booking",
                    "Choose a future Sunday–Thursday date, period and location");
    if (!strcmp(loc, "home") && rid)
        return fail(400, "home_resource", "Home attendance cannot reserve a room or desk");
    if (!execsql(a, "BEGIN IMMEDIATE"))
        return dberr(a);
    sqlite3_stmt *s = stmt(a, "SELECT id FROM closures WHERE date=? AND (mask & ?)!=0 AND "
                              "(location='all' OR location=?) LIMIT 1");
    if (!s)
        goto bad;
    bind_text(s, 1, date);
    sqlite3_bind_int(s, 2, mask);
    bind_text(s, 3, loc);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (rc == SQLITE_ROW) {
        execsql(a, "ROLLBACK");
        return fail(409, "shift_closed", "This shift is closed");
    }
    if (rc != SQLITE_DONE)
        goto bad;
    if (rid) {
        s = stmt(a, "SELECT id FROM resources WHERE id=? AND active=1");
        if (!s)
            goto bad;
        sqlite3_bind_int(s, 1, rid);
        rc = sqlite3_step(s);
        sqlite3_finalize(s);
        if (rc != SQLITE_ROW) {
            execsql(a, "ROLLBACK");
            return fail(400, "invalid_resource", "Resource is unavailable");
        }
        s = stmt(a, "SELECT b.id FROM bookings b JOIN resources old ON old.id=b.resource_id JOIN "
                    "resources wanted ON wanted.id=? WHERE b.date=? AND b.user_id!=? AND (b.mask & "
                    "?)!=0 AND (old.id=wanted.id OR (old.room=wanted.room AND (old.kind='room' OR "
                    "wanted.kind='room'))) LIMIT 1");
        if (!s)
            goto bad;
        sqlite3_bind_int(s, 1, rid);
        bind_text(s, 2, date);
        sqlite3_bind_int(s, 3, u->id);
        sqlite3_bind_int(s, 4, mask);
        rc = sqlite3_step(s);
        sqlite3_finalize(s);
        if (rc == SQLITE_ROW) {
            execsql(a, "ROLLBACK");
            return fail(409, "resource_busy", "Room or desk is already booked for that period");
        }
        if (rc != SQLITE_DONE)
            goto bad;
    }
    s = stmt(a, "INSERT INTO bookings(user_id,date,mask,location,resource_id) VALUES(?,?,?,?,?) ON "
                "CONFLICT(user_id,date) DO UPDATE SET "
                "mask=excluded.mask,location=excluded.location,resource_id=excluded.resource_id");
    if (!s)
        goto bad;
    sqlite3_bind_int(s, 1, u->id);
    bind_text(s, 2, date);
    sqlite3_bind_int(s, 3, mask);
    bind_text(s, 4, loc);
    if (rid)
        sqlite3_bind_int(s, 5, rid);
    else
        sqlite3_bind_null(s, 5);
    if (!run(s) || !execsql(a, "COMMIT"))
        goto bad;
    audit(a, u->id, "booking_saved", u->id);
    return ok();
bad:
    execsql(a, "ROLLBACK");
    return dberr(a);
}
static Result unbook(App *a, const Identity *u, int id) {
    if (id < 1)
        return fail(400, "invalid_id", "Invalid booking id");
    sqlite3_stmt *s = stmt(a, "SELECT user_id FROM bookings WHERE id=?");
    if (!s)
        return dberr(a);
    sqlite3_bind_int(s, 1, id);
    int rc = sqlite3_step(s), owner = rc == SQLITE_ROW ? sqlite3_column_int(s, 0) : 0;
    sqlite3_finalize(s);
    if (!owner)
        return fail(404, "not_found", "Booking not found");
    if (owner != u->id && !u->admin)
        return fail(403, "forbidden", "You can only change your own booking");
    s = stmt(a, "DELETE FROM bookings WHERE id=?");
    if (!s)
        return dberr(a);
    sqlite3_bind_int(s, 1, id);
    if (!run(s))
        return dberr(a);
    audit(a, u->id, "booking_deleted", id);
    return ok();
}
static Result users(App *a) {
    json_object *array = rows(stmt(a, "SELECT id,login,name,role,active,must_change AS "
                                      "must_change_password FROM users ORDER BY id"));
    if (!array)
        return dberr(a);
    json_object *o = json_object_new_object();
    json_object_object_add(o, "users", array);
    return result(200, o);
}
static Result adduser(App *a, const Identity *u, json_object *j) {
    const char *login_name = str(j, "login", 64), *name = str(j, "name", 256),
               *role = str(j, "role", 8);
    if (!loginvalid(login_name) || !textvalid(name, 1) || !role ||
        (strcmp(role, "admin") && strcmp(role, "member")))
        return fail(400, "invalid_user", "Use a lowercase login, name and valid role");
    char pw[33], hash[crypto_pwhash_STRBYTES];
    newpassword(pw);
    if (!hashpassword(pw, hash))
        return fail(503, "unavailable", "Try again later");
    sqlite3_stmt *s = stmt(a, "INSERT INTO users(login,name,role,password) VALUES(?,?,?,?)");
    if (!s)
        return dberr(a);
    bind_text(s, 1, login_name);
    bind_text(s, 2, name);
    bind_text(s, 3, role);
    bind_text(s, 4, hash);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (rc == SQLITE_CONSTRAINT) {
        sodium_memzero(pw, sizeof pw);
        return fail(409, "login_exists", "This login is already used");
    }
    if (rc != SQLITE_DONE) {
        sodium_memzero(pw, sizeof pw);
        return dberr(a);
    }
    int id = (int)sqlite3_last_insert_rowid(a->db);
    json_object *o = json_object_new_object(), *user = json_object_new_object();
    ji(user, "id", id);
    js(user, "login", login_name);
    js(user, "name", name);
    js(user, "role", role);
    json_object_object_add(o, "user", user);
    js(o, "temporary_password", pw);
    sodium_memzero(pw, sizeof pw);
    audit(a, u->id, "user_added", id);
    return result(201, o);
}
static Result edituser(App *a, const Identity *u, json_object *j) {
    int id = num(j, "id"), active = boolean(j, "active");
    const char *name = str(j, "name", 256), *role = str(j, "role", 8);
    if (id < 1 || active < 0 || !textvalid(name, 1) || !role ||
        (strcmp(role, "admin") && strcmp(role, "member")))
        return fail(400, "invalid_user", "Invalid user fields");
    if (!execsql(a, "BEGIN IMMEDIATE"))
        return dberr(a);
    sqlite3_stmt *s = stmt(a, "SELECT role,active,(SELECT count(*) FROM users WHERE active=1 AND "
                              "role='admin') FROM users WHERE id=?");
    if (!s)
        goto bad;
    sqlite3_bind_int(s, 1, id);
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        execsql(a, "ROLLBACK");
        return fail(404, "not_found", "User not found");
    }
    int last =
        !strcmp(col(s, 0), "admin") && sqlite3_column_int(s, 1) && sqlite3_column_int(s, 2) == 1;
    sqlite3_finalize(s);
    if (last && (!active || strcmp(role, "admin"))) {
        execsql(a, "ROLLBACK");
        return fail(409, "last_admin", "Keep at least one active administrator");
    }
    s = stmt(a, "UPDATE users SET name=?,role=?,active=? WHERE id=?");
    if (!s)
        goto bad;
    bind_text(s, 1, name);
    bind_text(s, 2, role);
    sqlite3_bind_int(s, 3, active);
    sqlite3_bind_int(s, 4, id);
    if (!run(s))
        goto bad;
    /* Permissions are read fresh on every request; disabling also revokes sessions. */
    if (!active) {
        s = stmt(a, "DELETE FROM sessions WHERE user_id=?");
        if (!s)
            goto bad;
        sqlite3_bind_int(s, 1, id);
        if (!run(s))
            goto bad;
    }
    if (!execsql(a, "COMMIT"))
        goto bad;
    audit(a, u->id, "user_updated", id);
    return ok();
bad:
    execsql(a, "ROLLBACK");
    return dberr(a);
}
static Result resetpassword(App *a, const Identity *u, json_object *j) {
    int id = num(j, "id");
    if (id < 1)
        return fail(400, "invalid_id", "Invalid user id");
    char pw[33], hash[crypto_pwhash_STRBYTES];
    newpassword(pw);
    if (!hashpassword(pw, hash))
        return fail(503, "unavailable", "Try again later");
    if (!execsql(a, "BEGIN IMMEDIATE"))
        return dberr(a);
    sqlite3_stmt *s =
        stmt(a, "UPDATE users SET password=?,must_change=1 WHERE id=?");
    if (!s)
        goto bad;
    bind_text(s, 1, hash);
    sqlite3_bind_int(s, 2, id);
    if (!run(s))
        goto bad;
    if (!sqlite3_changes(a->db)) {
        execsql(a, "ROLLBACK");
        sodium_memzero(pw, sizeof pw);
        return fail(404, "not_found", "User not found");
    }
    s = stmt(a, "DELETE FROM sessions WHERE user_id=?");
    if (!s)
        goto bad;
    sqlite3_bind_int(s, 1, id);
    if (!run(s) || !execsql(a, "COMMIT"))
        goto bad;
    json_object *o = json_object_new_object();
    js(o, "temporary_password", pw);
    sodium_memzero(pw, sizeof pw);
    audit(a, u->id, "password_reset", id);
    return result(200, o);
bad:
    sodium_memzero(pw, sizeof pw);
    execsql(a, "ROLLBACK");
    return dberr(a);
}
static Result closure(App *a, const Identity *u, json_object *j) {
    const char *date = str(j, "date", 10), *period = str(j, "period", 12),
               *loc = str(j, "location", 8), *reason = str(j, "reason", 1000);
    int mask = periodmask(period);
    if (!datevalid(date, NULL) || !mask || !locationvalid(loc, 1) || !textvalid(reason, 0))
        return fail(400, "invalid_closure", "Invalid closure fields");
    sqlite3_stmt *s = stmt(a, "INSERT INTO closures(date,mask,location,reason) VALUES(?,?,?,?) ON "
                              "CONFLICT(date,mask,location) DO UPDATE SET reason=excluded.reason");
    if (!s)
        return dberr(a);
    bind_text(s, 1, date);
    sqlite3_bind_int(s, 2, mask);
    bind_text(s, 3, loc);
    bind_text(s, 4, reason);
    if (!run(s))
        return dberr(a);
    audit(a, u->id, "closure_saved", 0);
    return ok();
}
static Result announcement(App *a, const Identity *u, json_object *j) {
    const char *date = str(j, "date", 10), *end = str(j, "end_date", 10),
               *title = str(j, "title", 480), *body = str(j, "body", 4000);
    if (!datevalid(date, NULL) || !datevalid(end, NULL) || strcmp(end, date) < 0 ||
        !textvalid(title, 1) || !textvalid(body, 0) || !textwithin(title, 120) ||
        !textwithin(body, 1000))
        return fail(400, "invalid_announcement", "Invalid dates, title or text");
    sqlite3_stmt *s =
        stmt(a, "INSERT INTO announcements(date,end_date,title,body) VALUES(?,?,?,?)");
    if (!s)
        return dberr(a);
    bind_text(s, 1, date);
    bind_text(s, 2, end);
    bind_text(s, 3, title);
    bind_text(s, 4, body);
    if (!run(s))
        return dberr(a);
    audit(a, u->id, "announcement_added", (int)sqlite3_last_insert_rowid(a->db));
    return ok();
}
static Result admin_delete(App *a, const Identity *u, const char *table, int id) {
    if (id < 1)
        return fail(400, "invalid_id", "Invalid id");
    const char *sql = !strcmp(table, "closures") ? "DELETE FROM closures WHERE id=?"
                                                 : "DELETE FROM announcements WHERE id=?";
    sqlite3_stmt *s = stmt(a, sql);
    if (!s)
        return dberr(a);
    sqlite3_bind_int(s, 1, id);
    if (!run(s))
        return dberr(a);
    if (!sqlite3_changes(a->db))
        return fail(404, "not_found", "Record not found");
    audit(a, u->id, !strcmp(table, "closures") ? "closure_deleted" : "announcement_deleted", id);
    return ok();
}
static Result resource(App *a, const Identity *u, json_object *j, int edit) {
    sqlite3_stmt *s;
    if (edit) {
        int id = num(j, "id"), active = boolean(j, "active");
        if (id < 1 || active < 0)
            return fail(400, "invalid_resource", "Invalid resource fields");
        if (!execsql(a, "BEGIN IMMEDIATE"))
            return dberr(a);
        if (!active) {
            char now[11];
            today(now);
            s = stmt(a, "SELECT id FROM bookings WHERE resource_id=? AND date>=? LIMIT 1");
            if (!s)
                goto bad;
            sqlite3_bind_int(s, 1, id);
            bind_text(s, 2, now);
            int rc = sqlite3_step(s);
            sqlite3_finalize(s);
            if (rc == SQLITE_ROW) {
                execsql(a, "ROLLBACK");
                return fail(409, "resource_booked",
                            "Cancel future bookings before disabling this resource");
            }
            if (rc != SQLITE_DONE)
                goto bad;
        }
        s = stmt(a, "UPDATE resources SET active=? WHERE id=?");
        if (!s)
            goto bad;
        sqlite3_bind_int(s, 1, active);
        sqlite3_bind_int(s, 2, id);
        if (!run(s))
            goto bad;
        if (!sqlite3_changes(a->db)) {
            execsql(a, "ROLLBACK");
            return fail(404, "not_found", "Resource not found");
        }
        if (!execsql(a, "COMMIT"))
            goto bad;
        audit(a, u->id, "resource_updated", id);
        return ok();
    }
    const char *name = str(j, "name", 256), *room = str(j, "room", 256), *kind = str(j, "kind", 8);
    if (!textvalid(name, 1) || !textvalid(room, 1) || !kind ||
        (strcmp(kind, "desk") && strcmp(kind, "room")))
        return fail(400, "invalid_resource", "Provide a resource name, room and kind");
    s = stmt(a, "INSERT INTO resources(name,room,kind) VALUES(?,?,?)");
    if (!s)
        return dberr(a);
    bind_text(s, 1, name);
    bind_text(s, 2, room);
    bind_text(s, 3, kind);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (rc == SQLITE_CONSTRAINT)
        return fail(409, "resource_exists", "This resource already exists");
    if (rc != SQLITE_DONE)
        return dberr(a);
    audit(a, u->id, "resource_added", (int)sqlite3_last_insert_rowid(a->db));
    return ok();
bad:
    execsql(a, "ROLLBACK");
    return dberr(a);
}
Result api(App *a, struct MHD_Connection *c, const char *url, const char *method, json_object *j) {
    int get = !strcmp(method, "GET"), post = !strcmp(method, "POST"),
        patch = !strcmp(method, "PATCH"), del = !strcmp(method, "DELETE");
    if (!get && !post && !patch && !del)
        return fail(405, "method_not_allowed", "Method not allowed");
    if (!get) {
        const char *origin = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Origin");
        if (!origin || strcmp(origin, a->origin))
            return fail(403, "invalid_origin", "Request origin is not allowed");
    }
    if (!strcmp(url, "/api/login"))
        return post ? login(a, c, j) : fail(405, "method_not_allowed", "POST required");
    Identity u;
    identity(a, c, &u);
    if (!strcmp(url, "/api/session") && get)
        return session(&u);
    if (!u.id)
        return fail(401, "unauthorized", "Please sign in");
    if (!get) {
        const char *csrf = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "X-CSRF-Token");
        if (!csrf || strlen(csrf) != 64 || sodium_memcmp(csrf, u.csrf, 64))
            return fail(403, "invalid_csrf", "Refresh the page and try again");
    }
    if (!strcmp(url, "/api/logout") && post) {
        sqlite3_stmt *s = stmt(a, "DELETE FROM sessions WHERE hash=?");
        if (!s)
            return dberr(a);
        bind_text(s, 1, u.session_hash);
        if (!run(s))
            return dberr(a);
        Result r = ok();
        snprintf(r.cookie, sizeof r.cookie,
                 "mishmeret_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0%s",
                 a->secure ? "; Secure" : "");
        return r;
    }
    if (!strcmp(url, "/api/password") && post)
        return password(a, c, &u, j);
    if (u.must_change)
        return fail(403, "password_change_required", "Change your temporary password first");
    if (!strcmp(url, "/api/wolfe"))
        return post ? wolfe_command(a, j) : fail(405, "method_not_allowed", "POST required");
    if (!strcmp(url, "/api/week") && get)
        return week(a, c);
    if (!strcmp(url, "/api/bookings")) {
        if (post)
            return book(a, &u, j);
        if (del)
            return unbook(a, &u, queryid(c));
        return fail(405, "method_not_allowed", "POST or DELETE required");
    }
    if (!strncmp(url, "/api/admin/", 11)) {
        if (!u.admin)
            return fail(403, "forbidden", "Administrator access required");
        if (!strcmp(url, "/api/admin/users")) {
            if (get)
                return users(a);
            if (post)
                return adduser(a, &u, j);
            if (patch)
                return edituser(a, &u, j);
        }
        if (!strcmp(url, "/api/admin/reset-password") && post)
            return resetpassword(a, &u, j);
        if (!strcmp(url, "/api/admin/resources")) {
            if (post)
                return resource(a, &u, j, 0);
            if (patch)
                return resource(a, &u, j, 1);
        }
        if (!strcmp(url, "/api/admin/closures")) {
            if (post)
                return closure(a, &u, j);
            if (del)
                return admin_delete(a, &u, "closures", queryid(c));
        }
        if (!strcmp(url, "/api/admin/announcements")) {
            if (post)
                return announcement(a, &u, j);
            if (del)
                return admin_delete(a, &u, "announcements", queryid(c));
        }
    }
    return fail(404, "not_found", "Endpoint not found");
}
