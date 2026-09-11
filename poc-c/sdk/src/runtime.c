#include "tedge_dot/runtime.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cjson/cJSON.h"
#include "mosquitto.h"

#define TICK_MS 200
#define BACKOFF_INITIAL_S 1.0
#define BACKOFF_MAX_S 60.0

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

typedef struct {
    tdot_connector_t *conn;
    tdot_config_t *cfg;
    struct mosquitto *mosq; /* NULL in stdout mode */
    tdot_output_t output;
} rt_t;

static void logmsg(const char *level, const char *fmt, ...) {
    char ts[40];
    tdot_now_rfc3339(ts, sizeof ts);
    fprintf(stderr, "%s %-5s ", ts, level);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void publish(rt_t *rt, const char *topic, const char *payload,
                    bool retained) {
    if (rt->output == TDOT_OUTPUT_STDOUT) {
        /* samples go to stdout; everything else is log-only */
        return;
    }
    mosquitto_publish(rt->mosq, NULL, topic, (int)strlen(payload), payload, 0,
                      retained);
}

static void emit_sample(rt_t *rt, tdot_device_t *dev, tdot_point_t *pt,
                        const tdot_sample_t *s) {
    pt->seq++;
    char *json = tdot_envelope_sample(rt->cfg, dev, pt, s);
    if (!json)
        return;
    if (rt->output == TDOT_OUTPUT_STDOUT) {
        puts(json);
        fflush(stdout);
    } else {
        char topic[256];
        snprintf(topic, sizeof topic, "te/device/%s/ot/%s/sample/%s",
                 dev->name, rt->cfg->protocol, pt->id);
        mosquitto_publish(rt->mosq, NULL, topic, (int)strlen(json), json, 0,
                          false);
    }
    free(json);
}

static void publish_link(rt_t *rt, tdot_device_t *dev, tdot_link_t status) {
    if (dev->link == status)
        return;
    dev->link = status;
    const char *name = status == TDOT_LINK_CONNECTED      ? "connected"
                       : status == TDOT_LINK_DEGRADED     ? "degraded"
                                                          : "disconnected";
    logmsg("info", "device %s: link %s", dev->name, name);
    char ts[40];
    tdot_now_rfc3339(ts, sizeof ts);
    char topic[256], payload[128];
    snprintf(topic, sizeof topic, "te/device/%s/ot/%s/status/link", dev->name,
             rt->cfg->protocol);
    snprintf(payload, sizeof payload, "{\"status\":\"%s\",\"since\":\"%s\"}",
             name, ts);
    publish(rt, topic, payload, true);
}

static void publish_health(rt_t *rt, const char *status) {
    char ts[40];
    tdot_now_rfc3339(ts, sizeof ts);
    char topic[256], payload[128];
    snprintf(topic, sizeof topic, "te/device/main/service/%s/status/health",
             rt->cfg->service_name);
    snprintf(payload, sizeof payload, "{\"status\":\"%s\",\"time\":\"%s\"}",
             status, ts);
    publish(rt, topic, payload, true);
}

static void connect_device(rt_t *rt, tdot_device_t *dev) {
    char err[TDOT_ERR_MAX];
    if (rt->conn->connect_device(rt->conn, dev, err, sizeof err) == 0) {
        dev->backoff_s = 0;
        publish_link(rt, dev, TDOT_LINK_CONNECTED);
    } else {
        logmsg("warn", "device %s: connect failed: %s", dev->name, err);
        publish_link(rt, dev, TDOT_LINK_DISCONNECTED);
        dev->backoff_s = dev->backoff_s > 0
                             ? (dev->backoff_s * 2 > BACKOFF_MAX_S
                                    ? BACKOFF_MAX_S
                                    : dev->backoff_s * 2)
                             : BACKOFF_INITIAL_S;
        dev->reconnect_at = tdot_mono() + dev->backoff_s;
        logmsg("info", "device %s: retrying in %.0fs", dev->name,
               dev->backoff_s);
    }
}

static void mark_transport_down(rt_t *rt, tdot_device_t *dev) {
    rt->conn->disconnect_device(rt->conn, dev);
    publish_link(rt, dev, TDOT_LINK_DISCONNECTED);
    dev->backoff_s = BACKOFF_INITIAL_S;
    dev->reconnect_at = tdot_mono() + dev->backoff_s;
    logmsg("info", "device %s: reconnecting in %.0fs", dev->name,
           dev->backoff_s);
}

/* ---- command handling ----------------------------------------------------
 * Inbound: te/device/<dev>/ot/<protocol>/cmd/<verb>/<id>
 * payload {"status":"init","point":...,"value":...}
 * Result published retained on the same topic.
 */

static int json_to_value(const cJSON *jv, tdot_value_t *out) {
    memset(out, 0, sizeof *out);
    if (cJSON_IsBool(jv)) {
        out->kind = TDOT_VAL_BOOL;
        out->b = cJSON_IsTrue(jv);
    } else if (cJSON_IsNumber(jv)) {
        out->kind = TDOT_VAL_NUM;
        out->num = jv->valuedouble;
    } else if (cJSON_IsString(jv)) {
        out->kind = TDOT_VAL_STR;
        snprintf(out->str, sizeof out->str, "%s", jv->valuestring);
    } else {
        return -1;
    }
    return 0;
}

static bool is_management_verb(const char *verb);
static void handle_management(rt_t *rt, const char *topic, const char *verb,
                              const cJSON *req);
static char *augmented_capabilities(const char *json);

static void publish_retained(rt_t *rt, const char *topic, cJSON *obj) {
    char *payload = cJSON_PrintUnformatted(obj);
    mosquitto_publish(rt->mosq, NULL, topic, (int)strlen(payload), payload, 0,
                      true);
    free(payload);
}

/* Execute one point write; returns 0 on success, else fills `reason`. */
static int do_write(rt_t *rt, tdot_device_t *dev, const char *dev_name,
                    const char *point_id, const cJSON *jvalue, char *reason,
                    size_t reason_len) {
    tdot_point_t *pt = (dev && point_id) ? tdot_device_point(dev, point_id) : NULL;
    tdot_value_t value;
    if (!dev) {
        snprintf(reason, reason_len, "unknown device: %s", dev_name);
    } else if (!pt) {
        snprintf(reason, reason_len, "unknown point: %s",
                 point_id ? point_id : "(missing)");
    } else if (!(pt->access & TDOT_ACCESS_WRITE)) {
        snprintf(reason, reason_len, "point %s is not writable", pt->id);
    } else if (json_to_value(jvalue, &value) != 0) {
        snprintf(reason, reason_len, "missing or invalid value");
    } else if (rt->conn->write_point(rt->conn, dev, pt, &value, reason,
                                     reason_len) == 0) {
        return 0;
    }
    return -1;
}

/* `write`: {"status":"init","point":...,"value":...} -> executing -> successful|failed */
static void handle_write(rt_t *rt, const char *topic, const char *dev_name,
                         tdot_device_t *dev, const cJSON *req) {
    const cJSON *jpoint = cJSON_GetObjectItem(req, "point");
    const char *point_id = cJSON_IsString(jpoint) ? jpoint->valuestring : NULL;
    const cJSON *jv = cJSON_GetObjectItem(req, "value");

    cJSON *exec = cJSON_CreateObject();
    cJSON_AddStringToObject(exec, "status", "executing");
    if (point_id)
        cJSON_AddStringToObject(exec, "point", point_id);
    publish_retained(rt, topic, exec);
    cJSON_Delete(exec);

    char reason[TDOT_ERR_MAX] = "";
    bool ok = do_write(rt, dev, dev_name, point_id, jv, reason, sizeof reason) == 0;

    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "status", ok ? "successful" : "failed");
    if (point_id)
        cJSON_AddStringToObject(res, "point", point_id);
    if (ok) {
        if (jv)
            cJSON_AddItemToObject(res, "value", cJSON_Duplicate(jv, 1));
        logmsg("info", "cmd write %s/%s: ok", dev_name,
               point_id ? point_id : "?");
    } else {
        cJSON_AddStringToObject(res, "reason", reason);
        logmsg("warn", "cmd write %s/%s: %s", dev_name,
               point_id ? point_id : "?", reason);
    }
    publish_retained(rt, topic, res);
    cJSON_Delete(res);
}

/* `write-batch` (contract §6.4): {"status":"init","writes":[{point,value},...]}.
 * Writes run sequentially in request order and stop at the first failure; the
 * terminal message lists one result per attempted write. Implemented once
 * here on top of the connector's write_point, like the Rust SDK runtime. */
static void handle_write_batch(rt_t *rt, const char *topic,
                               const char *dev_name, tdot_device_t *dev,
                               const cJSON *req) {
    const cJSON *writes = cJSON_GetObjectItem(req, "writes");
    cJSON *results = cJSON_CreateArray();
    char reason[TDOT_ERR_MAX] = "";
    bool failed = false;

    if (!cJSON_IsArray(writes)) {
        snprintf(reason, sizeof reason,
                 "write-batch request needs a `writes` array");
        failed = true;
    } else if (cJSON_GetArraySize(writes) == 0) {
        snprintf(reason, sizeof reason, "write-batch request has no writes");
        failed = true;
    }

    if (!failed) {
        cJSON *exec = cJSON_CreateObject();
        cJSON_AddStringToObject(exec, "status", "executing");
        cJSON *points = cJSON_AddArrayToObject(exec, "points");
        const cJSON *w;
        cJSON_ArrayForEach(w, writes) {
            const cJSON *jp = cJSON_GetObjectItem(w, "point");
            cJSON_AddItemToArray(points, cJSON_CreateString(
                                             cJSON_IsString(jp) ? jp->valuestring
                                                                : ""));
        }
        publish_retained(rt, topic, exec);
        cJSON_Delete(exec);

        cJSON_ArrayForEach(w, writes) {
            const cJSON *jp = cJSON_GetObjectItem(w, "point");
            const char *point_id = cJSON_IsString(jp) ? jp->valuestring : NULL;
            const cJSON *jv = cJSON_GetObjectItem(w, "value");
            char why[TDOT_ERR_MAX] = "";
            cJSON *r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "point", point_id ? point_id : "");
            if (do_write(rt, dev, dev_name, point_id, jv, why, sizeof why) == 0) {
                cJSON_AddStringToObject(r, "status", "successful");
                if (jv)
                    cJSON_AddItemToObject(r, "value", cJSON_Duplicate(jv, 1));
                cJSON_AddItemToArray(results, r);
            } else {
                snprintf(reason, sizeof reason, "write to %s failed: %s",
                         point_id ? point_id : "(missing)", why);
                cJSON_AddStringToObject(r, "status", "failed");
                cJSON_AddStringToObject(r, "reason", reason);
                cJSON_AddItemToArray(results, r);
                failed = true;
                break;
            }
        }
    }

    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "status", failed ? "failed" : "successful");
    if (failed)
        cJSON_AddStringToObject(res, "reason", reason);
    cJSON_AddItemToObject(res, "results", results);
    logmsg(failed ? "warn" : "info", "cmd write-batch %s: %s", dev_name,
           failed ? reason : "ok");
    publish_retained(rt, topic, res);
    cJSON_Delete(res);
}

static void on_message(struct mosquitto *mosq, void *ud,
                       const struct mosquitto_message *msg) {
    (void)mosq;
    rt_t *rt = ud;
    if (!msg->payload || msg->payloadlen == 0)
        return;

    /* Parse topic segments. */
    char topic[256];
    snprintf(topic, sizeof topic, "%s", msg->topic);
    char *seg[8] = {0};
    int nseg = 0;
    for (char *p = strtok(topic, "/"); p && nseg < 8; p = strtok(NULL, "/"))
        seg[nseg++] = p;
    /* te device <dev> ot <proto> cmd <verb> <id> */
    if (nseg != 8 || strcmp(seg[5], "cmd") != 0)
        return;
    const char *dev_name = seg[2], *verb = seg[6];

    cJSON *req = cJSON_ParseWithLength(msg->payload, (size_t)msg->payloadlen);
    if (!req)
        return;
    const cJSON *status = cJSON_GetObjectItem(req, "status");
    if (!cJSON_IsString(status) || strcmp(status->valuestring, "init") != 0) {
        cJSON_Delete(req); /* our own result echo, or already-processed */
        return;
    }

    tdot_device_t *dev = tdot_config_device(rt->cfg, dev_name);
    if (strcmp(verb, "write") == 0 || strcmp(verb, "write-coil") == 0) {
        /* write-coil is c8y_SetCoil's alias for write (see the Rust module) */
        handle_write(rt, msg->topic, dev_name, dev, req);
    } else if (strcmp(verb, "write-batch") == 0) {
        handle_write_batch(rt, msg->topic, dev_name, dev, req);
    } else if (is_management_verb(verb)) {
        handle_management(rt, msg->topic, verb, req);
    } else {
        cJSON *res = cJSON_CreateObject();
        cJSON_AddStringToObject(res, "status", "failed");
        char reason[TDOT_ERR_MAX];
        snprintf(reason, sizeof reason, "unsupported verb: %s", verb);
        cJSON_AddStringToObject(res, "reason", reason);
        logmsg("warn", "cmd %s %s: unsupported verb", verb, dev_name);
        publish_retained(rt, msg->topic, res);
        cJSON_Delete(res);
    }
    cJSON_Delete(req);
}

/* ---- capability augmentation ---------------------------------------------
 * Like the Rust SDK runtime, the verbs implemented here (management + batch)
 * are added to every module's descriptor at publish time. */
static void add_unique(cJSON *arr, const char *item) {
    const cJSON *x;
    cJSON_ArrayForEach(x, arr) {
        if (cJSON_IsString(x) && strcmp(x->valuestring, item) == 0)
            return;
    }
    cJSON_AddItemToArray(arr, cJSON_CreateString(item));
}

static char *augmented_capabilities(const char *json) {
    cJSON *caps = cJSON_Parse(json);
    if (!caps)
        return NULL;
    cJSON *verbs = cJSON_GetObjectItem(caps, "command_verbs");
    if (!cJSON_IsArray(verbs))
        verbs = cJSON_AddArrayToObject(caps, "command_verbs");
    bool has_write = false;
    const cJSON *v;
    cJSON_ArrayForEach(v, verbs) {
        if (cJSON_IsString(v) && strcmp(v->valuestring, "write") == 0)
            has_write = true;
    }
    if (has_write)
        add_unique(verbs, "write-batch");
    add_unique(verbs, "set-config");
    add_unique(verbs, "define-device");
    add_unique(verbs, "remove-device");
    cJSON *features = cJSON_GetObjectItem(caps, "features");
    if (!cJSON_IsArray(features))
        features = cJSON_AddArrayToObject(caps, "features");
    add_unique(features, "management");
    char *out = cJSON_PrintUnformatted(caps);
    cJSON_Delete(caps);
    return out;
}

/* ---- TOML emitter (cJSON document -> TOML text) ---------------------------
 * The management verbs patch the configuration as JSON and write it back as
 * TOML. Layout: top-level objects become [sections] (nested objects become
 * [dotted.sections]); arrays of objects become [[array]] entries whose nested
 * objects are inline tables and whose nested arrays of objects become
 * [[array.sub]] entries — i.e. the layout of the shipped config files
 * ([connector], [connection.serial], [[device]] with inline protocol_address,
 * [[device.point]] with inline address/transform/meta). Comments are not
 * preserved (tomlc99 is read-only), unlike the Rust runtime's toml_edit.
 */
typedef struct {
    char *buf;
    size_t len, cap;
} sb_t;

static void sb_put(sb_t *sb, const char *s) {
    size_t n = strlen(s);
    if (sb->len + n + 1 > sb->cap) {
        size_t cap = sb->cap ? sb->cap * 2 : 1024;
        while (cap < sb->len + n + 1)
            cap *= 2;
        sb->buf = realloc(sb->buf, cap);
        sb->cap = cap;
    }
    memcpy(sb->buf + sb->len, s, n + 1);
    sb->len += n;
}

static void sb_putf(sb_t *sb, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    sb_put(sb, tmp);
}

static bool bare_key(const char *k) {
    if (!*k)
        return false;
    for (const char *p = k; *p; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
            return false;
    }
    return true;
}

static void emit_string(sb_t *sb, const char *str) {
    sb_put(sb, "\"");
    for (const char *p = str; *p; p++) {
        switch (*p) {
        case '"': sb_put(sb, "\\\""); break;
        case '\\': sb_put(sb, "\\\\"); break;
        case '\n': sb_put(sb, "\\n"); break;
        case '\r': sb_put(sb, "\\r"); break;
        case '\t': sb_put(sb, "\\t"); break;
        default:
            if ((unsigned char)*p < 0x20)
                sb_putf(sb, "\\u%04x", (unsigned)(unsigned char)*p);
            else {
                char c[2] = {*p, 0};
                sb_put(sb, c);
            }
        }
    }
    sb_put(sb, "\"");
}

static void emit_key(sb_t *sb, const char *k) {
    if (bare_key(k))
        sb_put(sb, k);
    else
        emit_string(sb, k);
}

static bool is_object_array(const cJSON *v) {
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) == 0)
        return false;
    const cJSON *x;
    cJSON_ArrayForEach(x, v) {
        if (!cJSON_IsObject(x))
            return false;
    }
    return true;
}

static void emit_inline(sb_t *sb, const cJSON *v) {
    if (cJSON_IsString(v)) {
        emit_string(sb, v->valuestring);
    } else if (cJSON_IsBool(v)) {
        sb_put(sb, cJSON_IsTrue(v) ? "true" : "false");
    } else if (cJSON_IsNumber(v)) {
        double d = v->valuedouble;
        if (d == (double)(long long)d && d < 9.2e18 && d > -9.2e18)
            sb_putf(sb, "%lld", (long long)d);
        else
            sb_putf(sb, "%.17g", d);
    } else if (cJSON_IsArray(v)) {
        sb_put(sb, "[");
        const cJSON *x;
        bool first = true;
        cJSON_ArrayForEach(x, v) {
            if (!first)
                sb_put(sb, ", ");
            first = false;
            emit_inline(sb, x);
        }
        sb_put(sb, "]");
    } else if (cJSON_IsObject(v)) {
        sb_put(sb, "{ ");
        const cJSON *x;
        bool first = true;
        cJSON_ArrayForEach(x, v) {
            if (cJSON_IsNull(x))
                continue;
            if (!first)
                sb_put(sb, ", ");
            first = false;
            emit_key(sb, x->string);
            sb_put(sb, " = ");
            emit_inline(sb, x);
        }
        sb_put(sb, " }");
    } else {
        sb_put(sb, "\"\""); /* null: should have been skipped */
    }
}

static void emit_table(sb_t *sb, const cJSON *obj, const char *path,
                       bool in_array_item);

/* Scalars and inline values first, then the deferred sub-tables. */
static void emit_table_body(sb_t *sb, const cJSON *obj, const char *path,
                            bool in_array_item) {
    const cJSON *x;
    cJSON_ArrayForEach(x, obj) {
        if (cJSON_IsNull(x))
            continue;
        bool deferred = is_object_array(x) || (cJSON_IsObject(x) && !in_array_item);
        if (deferred)
            continue;
        emit_key(sb, x->string);
        sb_put(sb, " = ");
        emit_inline(sb, x);
        sb_put(sb, "\n");
    }
    cJSON_ArrayForEach(x, obj) {
        if (cJSON_IsNull(x))
            continue;
        char sub[512];
        if (*path)
            snprintf(sub, sizeof sub, "%s.%s", path, x->string);
        else
            snprintf(sub, sizeof sub, "%s", x->string);
        if (is_object_array(x)) {
            const cJSON *item;
            cJSON_ArrayForEach(item, x) {
                sb_putf(sb, "\n[[%s]]\n", sub);
                emit_table_body(sb, item, sub, true);
            }
        } else if (cJSON_IsObject(x) && !in_array_item) {
            emit_table(sb, x, sub, false);
        }
    }
}

static void emit_table(sb_t *sb, const cJSON *obj, const char *path,
                       bool in_array_item) {
    if (*path)
        sb_putf(sb, "\n[%s]\n", path);
    emit_table_body(sb, obj, path, in_array_item);
}

static char *toml_emit(const cJSON *doc) {
    sb_t sb = {0};
    sb_put(&sb, "# Written by tedge-dot (management command); comments are not preserved.\n");
    emit_table_body(&sb, doc, "", false);
    return sb.buf;
}

/* ---- management verbs (contract §6.3) -------------------------------------
 * set-config / define-device / remove-device patch the configuration document,
 * validate the result (config loader + connector configure), persist it to
 * the config file, and live-reload the connector — the same behaviour as the
 * Rust SDK runtime. */
static bool is_management_verb(const char *verb) {
    return strcmp(verb, "set-config") == 0 ||
           strcmp(verb, "define-device") == 0 ||
           strcmp(verb, "remove-device") == 0;
}

/* Deep-merge `patch` into `target`: objects merge recursively, everything
 * else replaces. */
static void deep_merge(cJSON *target, const cJSON *patch) {
    const cJSON *x;
    cJSON_ArrayForEach(x, patch) {
        cJSON *existing = cJSON_GetObjectItemCaseSensitive(target, x->string);
        if (existing && cJSON_IsObject(existing) && cJSON_IsObject(x)) {
            deep_merge(existing, x);
        } else if (existing) {
            cJSON_ReplaceItemInObjectCaseSensitive(target, x->string,
                                                   cJSON_Duplicate(x, 1));
        } else {
            cJSON_AddItemToObject(target, x->string, cJSON_Duplicate(x, 1));
        }
    }
}

static cJSON *find_device(cJSON *devices, const char *name, int *index) {
    int i = 0;
    cJSON *d;
    cJSON_ArrayForEach(d, devices) {
        const cJSON *n = cJSON_GetObjectItem(d, "name");
        if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0) {
            if (index)
                *index = i;
            return d;
        }
        i++;
    }
    return NULL;
}

/* Apply one management verb to the JSON form of the config document.
 * Returns 0, or -1 with `reason` filled. */
static int apply_management(cJSON *doc, const char *verb, const cJSON *req,
                            char *reason, size_t rlen) {
    cJSON *devices = cJSON_GetObjectItem(doc, "device");
    if (!cJSON_IsArray(devices))
        devices = cJSON_AddArrayToObject(doc, "device");

    if (strcmp(verb, "set-config") == 0) {
        const cJSON *target = cJSON_GetObjectItem(req, "target");
        const cJSON *config = cJSON_GetObjectItem(req, "config");
        if (!cJSON_IsString(target) || !cJSON_IsObject(config)) {
            snprintf(reason, rlen, "set-config needs `target` (string) and `config` (object)");
            return -1;
        }
        const char *t = target->valuestring;
        cJSON *section = NULL;
        if (strcmp(t, "connector") == 0 || strcmp(t, "mqtt") == 0 ||
            strcmp(t, "connection") == 0) {
            section = cJSON_GetObjectItem(doc, t);
            if (!cJSON_IsObject(section))
                section = cJSON_AddObjectToObject(doc, t);
        } else if (strncmp(t, "device:", 7) == 0) {
            section = find_device(devices, t + 7, NULL);
            if (!section) {
                snprintf(reason, rlen, "unknown device '%s'", t + 7);
                return -1;
            }
        } else {
            snprintf(reason, rlen,
                     "unknown target '%s' (expected connector|mqtt|connection|device:<name>)", t);
            return -1;
        }
        deep_merge(section, config);
        return 0;
    }
    if (strcmp(verb, "define-device") == 0) {
        const cJSON *device = cJSON_GetObjectItem(req, "device");
        const cJSON *name = cJSON_IsObject(device) ? cJSON_GetObjectItem(device, "name") : NULL;
        if (!cJSON_IsString(name)) {
            snprintf(reason, rlen, "define-device needs a `device` object with a `name`");
            return -1;
        }
        int idx = -1;
        if (find_device(devices, name->valuestring, &idx))
            cJSON_ReplaceItemInArray(devices, idx, cJSON_Duplicate(device, 1));
        else
            cJSON_AddItemToArray(devices, cJSON_Duplicate(device, 1));
        return 0;
    }
    /* remove-device */
    const cJSON *name = cJSON_GetObjectItem(req, "device");
    if (!cJSON_IsString(name)) {
        snprintf(reason, rlen, "remove-device needs `device` (the device name)");
        return -1;
    }
    int idx = -1;
    if (!find_device(devices, name->valuestring, &idx)) {
        snprintf(reason, rlen, "unknown device '%s'", name->valuestring);
        return -1;
    }
    cJSON_DeleteItemFromArray(devices, idx);
    return 0;
}

static void publish_status(rt_t *rt, const char *topic, const char *status,
                           const char *reason) {
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "status", status);
    if (reason)
        cJSON_AddStringToObject(res, "reason", reason);
    publish_retained(rt, topic, res);
    cJSON_Delete(res);
}

/* Validate the candidate config end to end, persist it, and live-reload:
 * disconnect -> configure(new) -> replace in place -> reconnect. On any
 * failure the running configuration is kept (and re-configured). */
static void handle_management(rt_t *rt, const char *topic, const char *verb,
                              const cJSON *req) {
    publish_status(rt, topic, "executing", NULL);
    char reason[TDOT_ERR_MAX] = "";
    tdot_config_t *cfg = rt->cfg;

    if (!cfg->path) {
        publish_status(rt, topic, "failed", "configuration has no file path to persist to");
        return;
    }
    char *json = tdot_config_root_json(cfg);
    cJSON *doc = json ? cJSON_Parse(json) : NULL;
    free(json);
    if (!doc) {
        publish_status(rt, topic, "failed", "cannot read the running configuration document");
        return;
    }
    if (apply_management(doc, verb, req, reason, sizeof reason) != 0) {
        cJSON_Delete(doc);
        publish_status(rt, topic, "failed", reason);
        logmsg("warn", "cmd %s: %s", verb, reason);
        return;
    }
    char *text = toml_emit(doc);
    cJSON_Delete(doc);

    /* Write the candidate next to the config and validate it with the real
     * loader before it replaces anything. */
    char tmp_path[1024];
    snprintf(tmp_path, sizeof tmp_path, "%s.tmp", cfg->path);
    FILE *f = fopen(tmp_path, "w");
    if (!f || fputs(text, f) == EOF || fclose(f) != 0) {
        free(text);
        snprintf(reason, sizeof reason, "cannot write %s: %s", tmp_path, strerror(errno));
        publish_status(rt, topic, "failed", reason);
        return;
    }
    free(text);
    char err[256];
    tdot_config_t *candidate = tdot_config_load(tmp_path, err, sizeof err);
    if (!candidate) {
        unlink(tmp_path);
        snprintf(reason, sizeof reason, "resulting config is invalid: %s", err);
        publish_status(rt, topic, "failed", reason);
        logmsg("warn", "cmd %s: %s", verb, reason);
        return;
    }

    /* Swap: release the running transports and connector state, try the
     * candidate on the protocol module, fall back to the old config if the
     * module rejects it. */
    for (size_t i = 0; i < cfg->ndevices; i++)
        rt->conn->disconnect_device(rt->conn, &cfg->devices[i]);
    tdot_config_release_protos(cfg);
    if (rt->conn->configure(rt->conn, candidate, err, sizeof err) != 0) {
        tdot_config_free(candidate);
        unlink(tmp_path);
        snprintf(reason, sizeof reason, "configure failed: %s", err);
        char err2[256];
        if (rt->conn->configure(rt->conn, cfg, err2, sizeof err2) != 0)
            logmsg("error", "re-configure of the previous config failed: %s", err2);
        for (size_t i = 0; i < cfg->ndevices; i++) {
            cfg->devices[i].link = TDOT_LINK_UNKNOWN;
            connect_device(rt, &cfg->devices[i]);
        }
        publish_status(rt, topic, "failed", reason);
        logmsg("warn", "cmd %s: %s", verb, reason);
        return;
    }
    if (rename(tmp_path, cfg->path) != 0)
        logmsg("warn", "failed to persist config to %s: %s", cfg->path, strerror(errno));
    tdot_config_replace(cfg, candidate); /* cfg pointer stays valid */
    for (size_t i = 0; i < cfg->ndevices; i++)
        connect_device(rt, &cfg->devices[i]);
    publish_status(rt, topic, "successful", NULL);
    logmsg("info", "cmd %s: applied and persisted to %s", verb, cfg->path);
}

/* ---- main loop ------------------------------------------------------------ */

/* Run one connector to completion. Assumes the mosquitto library is already
 * initialised and the SIGINT/SIGTERM handlers are installed by the caller, so
 * it is safe to call from one of several worker threads (each owns its own
 * connector, config and mosquitto client). */
static int run_connector(tdot_connector_t *conn, tdot_config_t *cfg,
                         const tdot_run_opts_t *opts) {
    rt_t rt = {.conn = conn, .cfg = cfg, .output = opts->output};
    char err[256];

    if (conn->configure(conn, cfg, err, sizeof err) != 0) {
        logmsg("error", "configure failed: %s", err);
        return -1;
    }

    if (rt.output == TDOT_OUTPUT_MQTT) {
        char client_id[128];
        snprintf(client_id, sizeof client_id, "%s-%s", cfg->service_name,
                 cfg->protocol);
        rt.mosq = mosquitto_new(client_id, true, &rt);
        mosquitto_message_callback_set(rt.mosq, on_message);
        /* last will: health "down" */
        char will_topic[256];
        snprintf(will_topic, sizeof will_topic,
                 "te/device/main/service/%s/status/health", cfg->service_name);
        mosquitto_will_set(rt.mosq, will_topic, 17, "{\"status\":\"down\"}",
                           0, true);
        if (mosquitto_connect(rt.mosq, cfg->mqtt_host, cfg->mqtt_port, 60) !=
            MOSQ_ERR_SUCCESS) {
            logmsg("error", "cannot connect to MQTT broker %s:%d",
                   cfg->mqtt_host, cfg->mqtt_port);
            mosquitto_destroy(rt.mosq);
            return -1;
        }
        char cmd_topic[256];
        snprintf(cmd_topic, sizeof cmd_topic, "te/device/+/ot/%s/cmd/+/+",
                 cfg->protocol);
        mosquitto_subscribe(rt.mosq, NULL, cmd_topic, 0);
        logmsg("info", "connected to MQTT broker %s:%d", cfg->mqtt_host,
               cfg->mqtt_port);

        publish_health(&rt, "up");
        if (conn->capabilities_json) {
            char cap_topic[256];
            snprintf(cap_topic, sizeof cap_topic,
                     "te/device/main/service/%s/ot/capabilities",
                     cfg->service_name);
            char *caps = augmented_capabilities(conn->capabilities_json);
            publish(&rt, cap_topic, caps ? caps : conn->capabilities_json,
                    true);
            free(caps);
        }
    }

    /* Initial connect for all devices. */
    for (size_t i = 0; i < cfg->ndevices; i++)
        connect_device(&rt, &cfg->devices[i]);

    double deadline =
        opts->duration_s > 0 ? tdot_mono() + opts->duration_s : 0;

    while (!g_stop) {
        double now = tdot_mono();
        if (deadline > 0 && now >= deadline)
            break;

        for (size_t i = 0; i < cfg->ndevices && !g_stop; i++) {
            tdot_device_t *dev = &cfg->devices[i];
            if (dev->link == TDOT_LINK_DISCONNECTED) {
                if (now < dev->reconnect_at)
                    continue;
                connect_device(&rt, dev);
                if (dev->link != TDOT_LINK_CONNECTED)
                    continue;
            }
            bool transport_down = false;
            size_t bad = 0, polled = 0;
            for (size_t j = 0; j < dev->npoints && !transport_down; j++) {
                tdot_point_t *pt = &dev->points[j];
                if (!(pt->access & TDOT_ACCESS_READ) || now < pt->next_due)
                    continue;
                tdot_sample_t s;
                tdot_sample_init(&s);
                int rc = conn->read_point(conn, dev, pt, &s);
                if (pt->mode == TDOT_MODE_RAW)
                    s.value.kind = TDOT_VAL_NONE; /* raw: bytes only */
                emit_sample(&rt, dev, pt, &s);
                pt->next_due = now + pt->poll_interval_s;
                polled++;
                if (s.quality == TDOT_Q_BAD)
                    bad++;
                if (rc != 0)
                    transport_down = true;
            }
            if (transport_down) {
                mark_transport_down(&rt, dev);
            } else if (polled > 0) {
                /* whole batch failing degrades the link; any success is
                 * connected */
                publish_link(&rt, dev,
                             bad == polled ? TDOT_LINK_DEGRADED
                                           : TDOT_LINK_CONNECTED);
            }
        }

        if (rt.output == TDOT_OUTPUT_MQTT)
            mosquitto_loop(rt.mosq, TICK_MS, 1);
        else {
            struct timespec ts = {.tv_sec = 0, .tv_nsec = TICK_MS * 1000000L};
            nanosleep(&ts, NULL);
        }
    }

    logmsg("info", "shutting down");
    for (size_t i = 0; i < cfg->ndevices; i++)
        conn->disconnect_device(conn, &cfg->devices[i]);
    if (rt.output == TDOT_OUTPUT_MQTT) {
        publish_health(&rt, "down");
        mosquitto_loop(rt.mosq, 100, 1); /* flush */
        mosquitto_disconnect(rt.mosq);
        mosquitto_destroy(rt.mosq);
    }
    return 0;
}

static void install_signal_handlers(void) {
    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

int tdot_runtime_run(tdot_connector_t *conn, tdot_config_t *cfg,
                     const tdot_run_opts_t *opts) {
    if (opts->output == TDOT_OUTPUT_MQTT)
        mosquitto_lib_init();
    install_signal_handlers();
    int rc = run_connector(conn, cfg, opts);
    if (opts->output == TDOT_OUTPUT_MQTT)
        mosquitto_lib_cleanup();
    return rc;
}

/* ---- multi-connector supervisor ------------------------------------------- */

/* One process runs every connector config found in a directory, each in its
 * own thread (mirrors the Rust runtime's single-service model). */

typedef struct {
    tdot_connector_t *conn;
    tdot_config_t *cfg;
    const tdot_run_opts_t *opts;
    int rc;
} worker_t;

static void *worker_main(void *arg) {
    worker_t *w = arg;
    w->rc = run_connector(w->conn, w->cfg, w->opts);
    return NULL;
}

int tdot_runtime_run_configs(const char *const *paths, size_t npaths,
                             const tdot_run_opts_t *opts) {
    if (npaths == 0) {
        logmsg("error", "no connector configs to run");
        return -1;
    }

    worker_t *workers = calloc(npaths, sizeof *workers);
    pthread_t *threads = calloc(npaths, sizeof *threads);
    size_t started = 0;

    /* Load every config and build its connector up front, so a bad config is
     * reported before anything starts publishing. */
    for (size_t i = 0; i < npaths; i++) {
        char err[256];
        tdot_config_t *cfg = tdot_config_load(paths[i], err, sizeof err);
        if (!cfg) {
            logmsg("error", "%s", err);
            continue;
        }
        tdot_connector_t *conn = tdot_connector_factory(cfg->protocol);
        if (!conn) {
            logmsg("error", "%s: unknown protocol '%s'", paths[i],
                   cfg->protocol);
            tdot_config_free(cfg);
            continue;
        }
        workers[started].conn = conn;
        workers[started].cfg = cfg;
        workers[started].opts = opts;
        logmsg("info", "loaded %s (%s)", paths[i], cfg->protocol);
        started++;
    }
    if (started == 0) {
        free(workers);
        free(threads);
        logmsg("error", "no valid connector configs");
        return -1;
    }

    if (opts->output == TDOT_OUTPUT_MQTT)
        mosquitto_lib_init();
    install_signal_handlers();

    for (size_t i = 0; i < started; i++)
        pthread_create(&threads[i], NULL, worker_main, &workers[i]);
    for (size_t i = 0; i < started; i++)
        pthread_join(threads[i], NULL);

    int rc = 0;
    for (size_t i = 0; i < started; i++) {
        if (workers[i].rc != 0)
            rc = -1;
        workers[i].conn->destroy(workers[i].conn);
        tdot_config_free(workers[i].cfg);
    }
    if (opts->output == TDOT_OUTPUT_MQTT)
        mosquitto_lib_cleanup();
    free(workers);
    free(threads);
    return rc;
}
