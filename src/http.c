#include "app.h"
#include <arpa/inet.h>
#include <signal.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    size_t size;
    int overflow;
    char body[MAX_BODY + 1];
} Request;
static volatile sig_atomic_t stopped;
static void stop_signal(int sig) {
    (void)sig;
    stopped = 1;
}

static enum MHD_Result send_response(struct MHD_Connection *c, int status, char *data, size_t size,
                                     const char *type, const char *cookie) {
    struct MHD_Response *r = MHD_create_response_from_buffer(size, data, MHD_RESPMEM_MUST_FREE);
    if (!r) {
        free(data);
        return MHD_NO;
    }
    MHD_add_response_header(r, "Content-Type", type);
    MHD_add_response_header(r, "Cache-Control", "no-store");
    MHD_add_response_header(r, "X-Content-Type-Options", "nosniff");
    MHD_add_response_header(r, "X-Frame-Options", "DENY");
    MHD_add_response_header(r, "Referrer-Policy", "no-referrer");
    MHD_add_response_header(r, "Permissions-Policy", "camera=(), microphone=(), geolocation=()");
    MHD_add_response_header(
        r, "Content-Security-Policy",
        "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; font-src "
        "'self'; connect-src 'self'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'");
    if (cookie && *cookie)
        MHD_add_response_header(r, "Set-Cookie", cookie);
    enum MHD_Result rc = MHD_queue_response(c, (unsigned)status, r);
    MHD_destroy_response(r);
    return rc;
}
static enum MHD_Result send_json(struct MHD_Connection *c, Result r) {
    const char *s = json_object_to_json_string_ext(r.json, JSON_C_TO_STRING_PLAIN);
    char *copy = strdup(s ? s : "{}");
    json_object_put(r.json);
    if (!copy)
        return MHD_NO;
    return send_response(c, r.status, copy, strlen(copy), "application/json; charset=utf-8",
                         r.cookie);
}
static enum MHD_Result asset(App *a, struct MHD_Connection *c, const char *url,
                             const char *method) {
    if (strcmp(method, "GET"))
        return send_json(c, fail(405, "method_not_allowed", "GET required"));
    const char *name, *type;
    if (!strcmp(url, "/")) {
        name = "index.html";
        type = "text/html; charset=utf-8";
    } else if (!strcmp(url, "/app.js")) {
        name = "app.js";
        type = "text/javascript; charset=utf-8";
    } else if (!strcmp(url, "/itzik.svg")) {
        name = "itzik.svg";
        type = "image/svg+xml";
    } else if (!strcmp(url, "/style.css")) {
        name = "style.css";
        type = "text/css; charset=utf-8";
    } else
        return send_json(c, fail(404, "not_found", "Not found"));
    char path[4352];
    int n = snprintf(path, sizeof path, "%s/%s", a->web, name);
    if (n < 0 || (size_t)n >= sizeof path)
        return MHD_NO;
    FILE *f = fopen(path, "rb");
    if (!f)
        return send_json(c, fail(404, "not_found", "Asset not found"));
    if (fseek(f, 0, SEEK_END)) {
        fclose(f);
        return MHD_NO;
    }
    long len = ftell(f);
    if (len < 0 || len > 2 * 1024 * 1024 || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return MHD_NO;
    }
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return MHD_NO;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    int err = ferror(f);
    fclose(f);
    if (err || got != (size_t)len) {
        free(buf);
        return MHD_NO;
    }
    return send_response(c, 200, buf, got, type, NULL);
}
static enum MHD_Result request(void *cls, struct MHD_Connection *c, const char *url,
                               const char *method, const char *version, const char *upload,
                               size_t *upload_size, void **con_cls) {
    (void)version;
    App *a = cls;
    if (!*con_cls) {
        Request *r = calloc(1, sizeof *r);
        if (!r)
            return MHD_NO;
        *con_cls = r;
        const char *length = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Content-Length");
        if (length) {
            size_t declared = 0;
            for (const char *p = length; *p >= '0' && *p <= '9'; p++) {
                declared = declared * 10 + (size_t)(*p - '0');
                if (declared > MAX_BODY)
                    return send_json(c, fail(413, "body_too_large", "Request body exceeds 16 KiB"));
            }
        }
        return MHD_YES;
    }
    Request *r = *con_cls;
    if (*upload_size) {
        if (*upload_size > MAX_BODY - r->size)
            r->overflow = 1;
        if (!r->overflow) {
            memcpy(r->body + r->size, upload, *upload_size);
            r->size += *upload_size;
            r->body[r->size] = 0;
        }
        *upload_size = 0;
        return MHD_YES;
    }
    /* libmicrohttpd permits replies at the header/final callback, not while
       receiving a body chunk. Discard oversized chunked uploads without growing memory. */
    if (r->overflow)
        return send_json(c, fail(413, "body_too_large", "Request body exceeds 16 KiB"));
    if (strlen(url) > 512)
        return send_json(c, fail(414, "uri_too_long", "URL too long"));
    if (!strcmp(url, "/healthz") && !strcmp(method, "GET")) {
        json_object *o = json_object_new_object();
        json_object_object_add(o, "ok", json_object_new_boolean(1));
        json_object_object_add(o, "version", json_object_new_string(APP_VERSION));
        Result rr = {.status = 200, .json = o};
        return send_json(c, rr);
    }
    if (strncmp(url, "/api/", 5))
        return asset(a, c, url, method);
    json_object *j = NULL;
    if (r->size) {
        const char *ct = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Content-Type");
        if (!ct || strncmp(ct, "application/json", 16) ||
            (ct[16] && ct[16] != ';' && ct[16] != ' '))
            return send_json(c, fail(415, "invalid_content_type", "Use application/json"));
        struct json_tokener *tok = json_tokener_new_ex(16);
        if (!tok)
            return MHD_NO;
        json_tokener_set_flags(tok, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
        j = json_tokener_parse_ex(tok, r->body, (int)r->size);
        enum json_tokener_error err = json_tokener_get_error(tok);
        size_t end = json_tokener_get_parse_end(tok);
        while (end < r->size && (r->body[end] == ' ' || r->body[end] == '\n' ||
                                 r->body[end] == '\r' || r->body[end] == '\t'))
            end++;
        json_tokener_free(tok);
        if (err != json_tokener_success || !j || !json_object_is_type(j, json_type_object) ||
            end != r->size) {
            if (j)
                json_object_put(j);
            return send_json(c, fail(400, "invalid_json", "Expected one valid JSON object"));
        }
    } else
        j = json_object_new_object();
    Result res = api(a, c, url, method, j);
    json_object_put(j);
    return send_json(c, res);
}
static void completed(void *cls, struct MHD_Connection *c, void **con_cls,
                      enum MHD_RequestTerminationCode toe) {
    (void)cls;
    (void)c;
    (void)toe;
    if (*con_cls) {
        sodium_memzero(*con_cls, sizeof(Request));
        free(*con_cls);
        *con_cls = NULL;
    }
}
int serve(App *a) {
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(a->port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);
    signal(SIGPIPE, SIG_IGN);
    struct MHD_Daemon *d = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG, a->port, NULL, NULL, request, a,
        MHD_OPTION_SOCK_ADDR, &addr, MHD_OPTION_CONNECTION_LIMIT, (unsigned)64,
        MHD_OPTION_PER_IP_CONNECTION_LIMIT, (unsigned)32, MHD_OPTION_CONNECTION_TIMEOUT,
        (unsigned)15, MHD_OPTION_CONNECTION_MEMORY_LIMIT, (size_t)65536,
        MHD_OPTION_NOTIFY_COMPLETED, completed, NULL, MHD_OPTION_END);
    if (!d) {
        fprintf(stderr, "Could not start loopback HTTP server\n");
        return 0;
    }
    fprintf(stderr, "mishmeret %s — %s (listening on 127.0.0.1:%u)\n", APP_VERSION, a->origin,
            (unsigned)a->port);
    while (!stopped) {
        struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
        nanosleep(&ts, NULL);
    }
    MHD_stop_daemon(d);
    return 1;
}
