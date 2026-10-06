#ifndef MISHMERET_APP_H
#define MISHMERET_APP_H
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#include "wolfe_api.h"
#include <json-c/json.h>
#include <microhttpd.h>
#include <sqlite3.h>
#include <stdint.h>
#define APP_VERSION "0.2.0"
#define MAX_BODY 16384
typedef struct {
    unsigned char key[17];
    double tokens, updated;
} HashBucket;
typedef struct {
    HashBucket hash_buckets[1024];
    int trust_tailscale_proxy;
    sqlite3 *db;
    Wolfe *wolf;
    char origin[512], web[4096];
    unsigned short port;
    int secure;
} App;
typedef struct {
    int id, admin, must_change;
    char login[65], name[257], csrf[65], session_hash[65];
} Identity;
typedef struct {
    int status;
    json_object *json;
    char cookie[256];
} Result;
Result api(App *, struct MHD_Connection *, const char *, const char *, json_object *);
Result fail(int, const char *, const char *);
int db_open(App *, const char *, int);
int bootstrap(App *);
void db_close(App *);
int serve(App *);
#endif
