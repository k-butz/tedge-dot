/* tedge-dot — CLI entry point (C implementation).
 *
 *   tedge-dot read  -c <config> [-d <device-glob>] [-p <point-glob>]...
 *                   [--poll] [--interval 1s] [--count N] [--json]
 *   tedge-dot write -c <config> -d <device> -p <point> --value <v>
 *   tedge-dot run   -c <config> [--output stdout|mqtt] [--duration 10s]
 *   tedge-dot describe [-c <config>] [-d <device-glob>] [--set <name>]
 *                      [--format c8y-dtm] [--compact]
 */
#include <ctype.h>
#include <dirent.h>
#include <fnmatch.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "tedge_dot/connector.h"
#include "tedge_dot/decode.h"
#include "tedge_dot/descriptor.h"
#include "tedge_dot/runtime.h"

static void usage(void) {
    fputs(
        "tedge-dot (C) — OT protocol connectors for thin-edge.io\n"
        "\n"
        "USAGE:\n"
        "  tedge-dot read  -c <config> [-d <device>] [-p <point>]... "
        "[--poll] [--interval <dur>] [--count <n>] [--json]\n"
        "  tedge-dot write -c <config> -d <device> -p <point> --value <v>\n"
        "  tedge-dot run   -c <config> [--output stdout|mqtt] "
        "[--duration <dur>]\n"
        "  tedge-dot describe [-c <config>] [-d <device>] [--set <name>] "
        "[--format c8y-dtm] [--compact]\n"
        "  tedge-dot <config-or-dir> [run options]      (same as run)\n",
        stderr);
}

/* Same default config file as the Rust binary. */
#define DEFAULT_CONFIG "/etc/tedge/plugins/ot/modbus.toml"

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

typedef struct {
    const char *config;
    const char *device;
    const char *points[16];
    int npoints;
    const char *value;
    const char *output;
    const char *format;
    const char *set;
    double interval_s;
    double duration_s;
    int count;
    bool poll;
    bool json;
    bool compact;
} args_t;

static int parse_args(int argc, char **argv, args_t *a) {
    memset(a, 0, sizeof *a);
    a->interval_s = 1.0;
    a->output = "mqtt";
    a->format = "c8y-dtm";
    for (int i = 2; i < argc; i++) {
        const char *arg = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;
        if ((!strcmp(arg, "-c") || !strcmp(arg, "--config")) && next)
            a->config = argv[++i];
        else if ((!strcmp(arg, "-d") || !strcmp(arg, "--device")) && next)
            a->device = argv[++i];
        else if ((!strcmp(arg, "-p") || !strcmp(arg, "--point")) && next) {
            if (a->npoints < 16)
                a->points[a->npoints++] = argv[++i];
        } else if (!strcmp(arg, "--value") && next)
            a->value = argv[++i];
        else if (!strcmp(arg, "--output") && next)
            a->output = argv[++i];
        else if (!strcmp(arg, "--format") && next)
            a->format = argv[++i];
        else if (!strcmp(arg, "--set") && next)
            a->set = argv[++i];
        else if (!strcmp(arg, "--compact"))
            a->compact = true;
        else if (!strcmp(arg, "--interval") && next)
            a->interval_s = tdot_duration_parse(argv[++i]);
        else if (!strcmp(arg, "--duration") && next)
            a->duration_s = tdot_duration_parse(argv[++i]);
        else if (!strcmp(arg, "--count") && next)
            a->count = atoi(argv[++i]);
        else if (!strcmp(arg, "--poll"))
            a->poll = true;
        else if (!strcmp(arg, "--json"))
            a->json = true;
        else if (arg[0] != '-' && !a->config)
            a->config = arg; /* positional config path */
        else {
            fprintf(stderr, "unknown argument: %s\n", arg);
            return -1;
        }
    }
    if (!a->config) {
        /* `describe` needs neither a device nor a broker, so — like the Rust
         * binary — it falls back to the packaged default config path. */
        if (!strcmp(argv[1], "describe"))
            a->config = DEFAULT_CONFIG;
        else {
            fputs("missing --config\n", stderr);
            return -1;
        }
    }
    return 0;
}

static bool point_matches(const args_t *a, const tdot_point_t *pt) {
    if (a->npoints == 0)
        return true;
    for (int i = 0; i < a->npoints; i++)
        if (fnmatch(a->points[i], pt->id, 0) == 0)
            return true;
    return false;
}

static bool device_matches(const args_t *a, const tdot_device_t *dev) {
    return !a->device || fnmatch(a->device, dev->name, 0) == 0;
}

/* Load config + build connector + configure. */
static int setup(const args_t *a, tdot_config_t **cfg,
                 tdot_connector_t **conn) {
    char err[256];
    *cfg = tdot_config_load(a->config, err, sizeof err);
    if (!*cfg) {
        fprintf(stderr, "error: %s\n", err);
        return -1;
    }
    *conn = tdot_connector_factory((*cfg)->protocol);
    if (!*conn) {
        fprintf(stderr, "error: unknown protocol '%s'\n", (*cfg)->protocol);
        tdot_config_free(*cfg);
        return -1;
    }
    return 0;
}

static void print_sample(const args_t *a, tdot_config_t *cfg,
                         tdot_device_t *dev, tdot_point_t *pt,
                         const tdot_sample_t *s) {
    pt->seq++;
    if (a->json) {
        char *json = tdot_envelope_sample(cfg, dev, pt, s);
        puts(json);
        free(json);
        return;
    }
    char hex[TDOT_RAW_MAX * 3 + 1];
    tdot_hex_format(s->raw, s->raw_len, s->raw_group, hex, sizeof hex);
    if (s->quality == TDOT_Q_BAD) {
        printf("%-8s %-14s quality=bad  error=%s\n", dev->name, pt->id,
               s->error);
        return;
    }
    char val[80] = "-";
    switch (s->value.kind) {
    case TDOT_VAL_BOOL:
        snprintf(val, sizeof val, "%s", s->value.b ? "true" : "false");
        break;
    case TDOT_VAL_NUM:
        snprintf(val, sizeof val, "%g", s->value.num);
        break;
    case TDOT_VAL_STR:
        snprintf(val, sizeof val, "\"%s\"", s->value.str);
        break;
    default:
        break;
    }
    printf("%-8s %-14s %-12s %s%s%s (raw: %s)\n", dev->name, pt->id, val,
           pt->unit ? "" : "", pt->unit ? pt->unit : "",
           pt->unit ? " " : "", hex);
}

static int cmd_read(const args_t *a) {
    tdot_config_t *cfg;
    tdot_connector_t *conn;
    char err[256];
    if (setup(a, &cfg, &conn) != 0)
        return 1;
    if (conn->configure(conn, cfg, err, sizeof err) != 0) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }

    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGINT, &sa, NULL);

    int exit_code = 0;
    bool any_matched = false;
    for (size_t i = 0; i < cfg->ndevices; i++) {
        tdot_device_t *dev = &cfg->devices[i];
        if (!device_matches(a, dev))
            continue;
        bool has_point = false;
        for (size_t j = 0; j < dev->npoints; j++)
            if (point_matches(a, &dev->points[j]) &&
                (dev->points[j].access & TDOT_ACCESS_READ))
                has_point = true;
        if (!has_point)
            continue;
        any_matched = true;
        if (conn->connect_device(conn, dev, err, sizeof err) != 0) {
            fprintf(stderr, "error: device %s: %s\n", dev->name, err);
            exit_code = 1;
            continue;
        }
    }
    if (!any_matched) {
        fprintf(stderr, "error: no matching readable points\n");
        return 1;
    }

    int rounds = 0;
    do {
        for (size_t i = 0; i < cfg->ndevices && !g_stop; i++) {
            tdot_device_t *dev = &cfg->devices[i];
            if (!device_matches(a, dev) || !dev->proto)
                continue;
            for (size_t j = 0; j < dev->npoints; j++) {
                tdot_point_t *pt = &dev->points[j];
                if (!point_matches(a, pt) ||
                    !(pt->access & TDOT_ACCESS_READ))
                    continue;
                tdot_sample_t s;
                tdot_sample_init(&s);
                conn->read_point(conn, dev, pt, &s);
                print_sample(a, cfg, dev, pt, &s);
                if (s.quality == TDOT_Q_BAD)
                    exit_code = 1;
            }
        }
        rounds++;
        if (a->poll && !g_stop && (a->count == 0 || rounds < a->count)) {
            struct timespec ts = {
                .tv_sec = (time_t)a->interval_s,
                .tv_nsec = (long)((a->interval_s -
                                   (double)(time_t)a->interval_s) *
                                  1e9)};
            nanosleep(&ts, NULL);
        }
    } while (a->poll && !g_stop && (a->count == 0 || rounds < a->count));

    for (size_t i = 0; i < cfg->ndevices; i++)
        conn->disconnect_device(conn, &cfg->devices[i]);
    conn->destroy(conn);
    tdot_config_free(cfg);
    return exit_code;
}

static void parse_value(const char *s, tdot_value_t *v) {
    memset(v, 0, sizeof *v);
    if (!strcmp(s, "true") || !strcmp(s, "false")) {
        v->kind = TDOT_VAL_BOOL;
        v->b = !strcmp(s, "true");
        return;
    }
    char *end = NULL;
    double num = strtod(s, &end);
    if (end && *end == '\0' && end != s) {
        v->kind = TDOT_VAL_NUM;
        v->num = num;
        return;
    }
    v->kind = TDOT_VAL_STR;
    snprintf(v->str, sizeof v->str, "%s", s);
}

static int cmd_write(const args_t *a) {
    if (!a->device || a->npoints != 1 || !a->value) {
        fputs("write requires -d <device> -p <point> --value <v>\n", stderr);
        return 1;
    }
    tdot_config_t *cfg;
    tdot_connector_t *conn;
    char err[256];
    if (setup(a, &cfg, &conn) != 0)
        return 1;
    if (conn->configure(conn, cfg, err, sizeof err) != 0) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    tdot_device_t *dev = tdot_config_device(cfg, a->device);
    if (!dev) {
        fprintf(stderr, "error: unknown device: %s\n", a->device);
        return 1;
    }
    tdot_point_t *pt = tdot_device_point(dev, a->points[0]);
    if (!pt) {
        fprintf(stderr, "error: unknown point: %s\n", a->points[0]);
        return 1;
    }
    if (!(pt->access & TDOT_ACCESS_WRITE)) {
        fprintf(stderr, "error: point %s is not writable\n", pt->id);
        return 1;
    }
    if (conn->connect_device(conn, dev, err, sizeof err) != 0) {
        fprintf(stderr, "error: device %s: %s\n", dev->name, err);
        return 1;
    }
    tdot_value_t value;
    parse_value(a->value, &value);
    int rc = conn->write_point(conn, dev, pt, &value, err, sizeof err);
    if (rc != 0)
        fprintf(stderr, "error: write %s/%s: %s\n", dev->name, pt->id, err);
    else
        printf("wrote %s/%s = %s\n", dev->name, pt->id, a->value);
    conn->disconnect_device(conn, dev);
    conn->destroy(conn);
    tdot_config_free(cfg);
    return rc == 0 ? 0 : 1;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Run every *.toml in a directory, one connector per file, in one process
 * (the packaged systemd unit points ExecStart at /etc/tedge/plugins/ot). */
static int run_dir(const char *dir, const tdot_run_opts_t *opts) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "error: cannot open config directory %s\n", dir);
        return 1;
    }
    char **paths = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".toml") != 0)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            paths = realloc(paths, cap * sizeof *paths);
        }
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        paths[n++] = strdup(full);
    }
    closedir(d);
    if (n == 0) {
        fprintf(stderr, "error: no *.toml configs in %s\n", dir);
        free(paths);
        return 1;
    }
    qsort(paths, n, sizeof *paths, cmp_str); /* stable, predictable order */

    int rc = tdot_runtime_run_configs((const char *const *)paths, n, opts);
    for (size_t i = 0; i < n; i++)
        free(paths[i]);
    free(paths);
    return rc == 0 ? 0 : 1;
}

static int cmd_run(const args_t *a) {
    tdot_run_opts_t opts = {
        .output = strcmp(a->output, "stdout") == 0 ? TDOT_OUTPUT_STDOUT
                                                   : TDOT_OUTPUT_MQTT,
        .duration_s = a->duration_s,
    };

    /* A directory argument runs every connector config it contains. */
    struct stat st;
    if (a->config && stat(a->config, &st) == 0 && S_ISDIR(st.st_mode))
        return run_dir(a->config, &opts);

    tdot_config_t *cfg;
    tdot_connector_t *conn;
    if (setup(a, &cfg, &conn) != 0)
        return 1;
    int rc = tdot_runtime_run(conn, cfg, &opts);
    conn->destroy(conn);
    tdot_config_free(cfg);
    return rc == 0 ? 0 : 1;
}

/* Render the Cumulocity DTM property definitions derived from a configuration
 * (mirrors cmd_describe in src/main.rs). Needs no device, broker or protocol
 * module — only the config file. */
static int cmd_describe(const args_t *a) {
    if (strcmp(a->format, "c8y-dtm") != 0) {
        fprintf(stderr, "error: unknown --format '%s' (expected c8y-dtm)\n",
                a->format);
        return 1;
    }
    char err[256];
    tdot_config_t *cfg = tdot_config_load(a->config, err, sizeof err);
    if (!cfg) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }

    /* Restrict to the matching devices by moving them to the front; ndevices is
     * restored before the free so nothing leaks. */
    size_t all = cfg->ndevices, keep = 0;
    for (size_t i = 0; i < all; i++) {
        if (!device_matches(a, &cfg->devices[i]))
            continue;
        tdot_device_t tmp = cfg->devices[keep];
        cfg->devices[keep] = cfg->devices[i];
        cfg->devices[i] = tmp;
        keep++;
    }
    cfg->ndevices = keep;
    int rc = 1;
    char *forced_owned = NULL;
    if (keep == 0 && a->device) {
        fprintf(stderr, "error: no device matches '%s'\n", a->device);
        goto out;
    }

    /* Parameter ids become fragment keys on the device twin, so they must be
     * plain identifiers. */
    /* A blank --set means "none given", as an empty `default_set` does in the
     * flow: an unset variable in a provisioning script (--set "$PARAM_SET")
     * must not force every point into a nameless set. The Rust build applies
     * the same rule. */
    const char *forced = NULL;
    if (a->set) {
        /* Trimmed, not just tested for blankness: `--set "$(cat name.txt)"`
         * carries a trailing newline, and the Rust CLI trims the same way, so
         * the two must not disagree on a padded value either. */
        forced_owned = strdup(a->set);
        const char *start = forced_owned;
        while (*start && isspace((unsigned char)*start))
            start++;
        size_t end = strlen(start);
        while (end && isspace((unsigned char)start[end - 1]))
            end--;
        memmove(forced_owned, start, end);
        forced_owned[end] = '\0';
        forced = *forced_owned ? forced_owned : NULL;
    }

    char *bad = tdot_param_invalid_keys(cfg, forced);
    if (bad) {
        fprintf(stderr, "error: parameter keys must match [A-Za-z0-9_]: %s\n",
                bad);
        free(bad);
        goto out;
    }

    /* A DTM identifier is tenant-wide, so a set named after the protocol is
     * shared with every other device type that speaks it. Declaring the device
     * type is what keeps them apart. */
    if (!forced) {
        char *collisions = tdot_param_type_warnings(cfg);
        if (collisions) {
            fprintf(stderr, "%s\n", collisions);
            free(collisions);
        }
        char *untyped = tdot_param_untyped_devices(cfg);
        if (untyped) {
            fprintf(stderr,
                    "warning: device(s) %s declare no `type`, so their parameter "
                    "sets are named after the protocol ('%s_...') and collide "
                    "with every other %s device type in the tenant; set `type` "
                    "on the device or in its point library\n",
                    untyped, cfg->protocol, cfg->protocol);
            free(untyped);
        }
    }

    cJSON *docs = tdot_c8y_dtm_definitions(cfg, forced);
    if (a->compact) {
        cJSON *doc;
        cJSON_ArrayForEach(doc, docs) {
            char *line = cJSON_PrintUnformatted(doc);
            puts(line);
            free(line);
        }
    } else {
        char *out = cJSON_Print(docs);
        puts(out);
        free(out);
    }
    cJSON_Delete(docs);
    rc = 0;

out:
    free(forced_owned);
    cfg->ndevices = all;
    tdot_config_free(cfg);
    return rc;
}

static bool is_subcommand(const char *s) {
    return !strcmp(s, "read") || !strcmp(s, "write") || !strcmp(s, "run") ||
           !strcmp(s, "describe");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    /* Like the Rust binary: invoked with just config paths/options and no
     * subcommand (`tedge-dot /etc/connector.toml`, the systemd unit and the e2e
     * entrypoints do this), behave as `run`. */
    char **args = argv;
    int nargs = argc;
    char **shifted = NULL;
    if (!is_subcommand(argv[1]) && strcmp(argv[1], "-h") != 0 &&
        strcmp(argv[1], "--help") != 0) {
        shifted = calloc((size_t)argc + 2, sizeof *shifted);
        shifted[0] = argv[0];
        shifted[1] = "run";
        for (int i = 1; i < argc; i++)
            shifted[i + 1] = argv[i];
        args = shifted;
        nargs = argc + 1;
    }
    args_t a;
    int rc = 2;
    if (parse_args(nargs, args, &a) != 0)
        usage();
    else if (!strcmp(args[1], "read"))
        rc = cmd_read(&a);
    else if (!strcmp(args[1], "write"))
        rc = cmd_write(&a);
    else if (!strcmp(args[1], "run"))
        rc = cmd_run(&a);
    else if (!strcmp(args[1], "describe"))
        rc = cmd_describe(&a);
    else
        usage();
    free(shifted);
    return rc;
}
