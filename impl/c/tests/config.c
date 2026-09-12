/* Config-loader semantics that the runtime depends on but no higher layer
 * pins down precisely.
 *
 * The liveness bounds (contract §8.1) and the sampling-interval hint both have
 * rules that are easy to get subtly wrong and expensive to notice: a
 * stall_timeout below the per-call bound restarts a healthy connector in a
 * loop, and confusing a point's OWN poll_interval with its resolved one turns
 * push delivery back into polling at the device's rate. Both mirror the Rust
 * implementation (impl/rust/src/main.rs `stall_timeout`, and
 * impl/rust/crates/sdk/src/runtime.rs `point_ref`/`setup_subscriptions`).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tedge_dot/config.h"

static int failures = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            /* pass */                                                         \
        } else {                                                               \
            failures++;                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

/* A minimal modbus config; `extra` is spliced into [connector] and
 * `point_extra` into the single point. */
static tdot_config_t *load(const char *extra, const char *point_extra) {
    char template[] = "/tmp/tdot-config-XXXXXX";
    char *dir = mkdtemp(template);
    if (!dir) {
        perror("mkdtemp");
        exit(2);
    }
    char path[256];
    snprintf(path, sizeof path, "%s/modbus.toml", dir);
    FILE *fp = fopen(path, "w");
    fprintf(fp,
            "[connector]\n"
            "protocol = \"modbus\"\n"
            "poll_interval = \"2s\"\n"
            "%s"
            "\n"
            "[[device]]\n"
            "name = \"plc1\"\n"
            "poll_interval = \"30s\"\n"
            "protocol_address = { transport = \"tcp\", host = \"127.0.0.1\", "
            "port = 502, unit_id = 1 }\n"
            "\n"
            "  [[device.point]]\n"
            "  id = \"temp_u16\"\n"
            "  datatype = \"uint16\"\n"
            "  address = { table = \"holding\", address = 3, count = 1 }\n"
            "%s",
            extra, point_extra);
    fclose(fp);

    char err[256];
    tdot_config_t *cfg = tdot_config_load(path, err, sizeof err);
    if (!cfg) {
        printf("FAIL config did not load: %s\n", err);
        failures++;
    }
    unlink(path);
    rmdir(dir);
    return cfg;
}

static void check_timeout_defaults(void) {
    tdot_config_t *cfg = load("", "");
    if (!cfg)
        return;
    CHECK(cfg->operation_timeout_s == 30.0,
          "default operation_timeout should be 30s, got %.1f",
          cfg->operation_timeout_s);
    CHECK(cfg->stall_timeout_s == 120.0,
          "default stall_timeout should be 120s, got %.1f",
          cfg->stall_timeout_s);
    tdot_config_free(cfg);
}

static void check_timeouts_are_parsed(void) {
    tdot_config_t *cfg =
        load("operation_timeout = \"5s\"\nstall_timeout = \"90s\"\n", "");
    if (!cfg)
        return;
    CHECK(cfg->operation_timeout_s == 5.0, "operation_timeout 5s, got %.1f",
          cfg->operation_timeout_s);
    CHECK(cfg->stall_timeout_s == 90.0, "stall_timeout 90s, got %.1f",
          cfg->stall_timeout_s);
    tdot_config_free(cfg);
}

static void check_stall_timeout_is_floored(void) {
    /* Below 2x operation_timeout a single slow-but-legitimate call would look
     * like a hang, so the value is raised rather than honoured. */
    tdot_config_t *cfg =
        load("operation_timeout = \"20s\"\nstall_timeout = \"25s\"\n", "");
    if (!cfg)
        return;
    CHECK(cfg->stall_timeout_s == 40.0,
          "stall_timeout below the 2x floor should be raised to 40s, got %.1f",
          cfg->stall_timeout_s);
    tdot_config_free(cfg);
}

static void check_stall_timeout_zero_disables(void) {
    /* 0 means "no watchdog" and must survive the floor, which would otherwise
     * silently re-arm it. */
    tdot_config_t *cfg =
        load("operation_timeout = \"20s\"\nstall_timeout = \"0\"\n", "");
    if (!cfg)
        return;
    CHECK(cfg->stall_timeout_s == 0.0,
          "stall_timeout = 0 must stay disabled, got %.1f",
          cfg->stall_timeout_s);
    tdot_config_free(cfg);
}

static void check_invalid_timeouts_fall_back(void) {
    /* An unparseable bound warns and uses the default rather than refusing to
     * start: a typo must not take a gateway's connectors offline. */
    tdot_config_t *cfg = load(
        "operation_timeout = \"soon\"\nstall_timeout = \"later\"\n", "");
    if (!cfg)
        return;
    CHECK(cfg->operation_timeout_s == 30.0,
          "invalid operation_timeout should fall back to 30s, got %.1f",
          cfg->operation_timeout_s);
    CHECK(cfg->stall_timeout_s == 120.0,
          "invalid stall_timeout should fall back to 120s, got %.1f",
          cfg->stall_timeout_s);
    tdot_config_free(cfg);
}

static void check_point_interval_resolution(void) {
    /* No poll_interval on the point: the polling schedule inherits the device's
     * 30s, but own_poll_interval_s stays unset so a subscribe-capable module
     * uses its OWN sampling default instead of sampling once every 30s. */
    tdot_config_t *cfg = load("", "");
    if (!cfg)
        return;
    tdot_point_t *pt = &cfg->devices[0].points[0];
    CHECK(pt->poll_interval_s == 30.0,
          "point should inherit the device poll_interval (30s), got %.1f",
          pt->poll_interval_s);
    CHECK(pt->own_poll_interval_s < 0,
          "a point without its own poll_interval must report none, got %.1f",
          pt->own_poll_interval_s);
    tdot_config_free(cfg);

    /* With its own poll_interval, both are that value. */
    cfg = load("", "  poll_interval = \"250ms\"\n");
    if (!cfg)
        return;
    pt = &cfg->devices[0].points[0];
    CHECK(pt->poll_interval_s == 0.25, "point poll_interval 250ms, got %.3f",
          pt->poll_interval_s);
    CHECK(pt->own_poll_interval_s == 0.25,
          "own poll_interval should be 250ms, got %.3f",
          pt->own_poll_interval_s);
    tdot_config_free(cfg);
}

static void check_subscribe_defaults_on(void) {
    /* Push delivery is opt-out, not opt-in (contract §4.2). */
    tdot_config_t *cfg = load("", "");
    if (!cfg)
        return;
    CHECK(cfg->devices[0].points[0].subscribe,
          "subscribe should default to true");
    CHECK(!cfg->devices[0].points[0].subscribed,
          "subscribed is runtime state and must start false");
    tdot_config_free(cfg);

    cfg = load("", "  subscribe = false\n");
    if (!cfg)
        return;
    CHECK(!cfg->devices[0].points[0].subscribe,
          "subscribe = false must be honoured");
    tdot_config_free(cfg);
}

int main(void) {
    check_timeout_defaults();
    check_timeouts_are_parsed();
    check_stall_timeout_is_floored();
    check_stall_timeout_zero_disables();
    check_invalid_timeouts_fall_back();
    check_point_interval_resolution();
    check_subscribe_defaults_on();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("config: all checks passed\n");
    return 0;
}
