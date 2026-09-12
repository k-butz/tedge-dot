/* Device-parameter derivation and Cumulocity DTM rendering.
 *
 * Holds the C implementation to the same assertions (and the same fixture
 * config) as the Rust SDK's descriptor tests in impl/rust/crates/sdk/src/descriptor.rs,
 * so `tedge-dot describe` renders the same definitions in both builds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cjson/cJSON.h"
#include "tedge_dot/descriptor.h"

static const char *CONFIG =
    "[connector]\n"
    "protocol = \"modbus\"\n"
    "\n"
    "[[device]]\n"
    "name = \"plc1\"\n"
    "protocol_address = { transport = \"tcp\", host = \"127.0.0.1\", port = 502, unit_id = 1 }\n"
    "\n"
    "  [[device.point]]\n"
    "  id = \"temp_u16\"\n"
    "  datatype = \"uint16\"\n"
    "  access = \"read_write\"\n"
    "  unit = \"°C\"\n"
    "  name = \"Boiler temp\"\n"
    "  description = \"Outlet temperature after the heat exchanger\"\n"
    "  address = { table = \"holding\", address = 3, count = 1 }\n"
    "  meta = { parameter = { title = \"Temperature setpoint\", min = 0, max = 100, order = 7 } }\n"
    "\n"
    "  [[device.point]]\n"
    "  id = \"coil_rw\"\n"
    "  datatype = \"bool\"\n"
    "  access = \"read_write\"\n"
    "  name = \"Pump enable\"\n"
    "  address = { table = \"coil\", address = 48, count = 1 }\n"
    "\n"
    "  [[device.point]]\n"
    "  id = \"pump_speed\"\n"
    "  datatype = \"float32\"\n"
    "  access = \"write\"\n"
    "  address = { table = \"holding\", address = 10, count = 2 }\n"
    "  meta = { parameter = \"pump\" }\n"
    "\n"
    "  [[device.point]]\n"
    "  id = \"level_f32\"\n"
    "  datatype = \"float32\"\n"
    "  description = \"Level in the buffer tank\"\n"
    "  address = { table = \"holding\", address = 6, count = 2 }\n"
    "\n"
    "  [[device.point]]\n"
    "  id = \"status_word\"\n"
    "  datatype = \"uint16\"\n"
    "  address = { table = \"holding\", address = 20, count = 1 }\n"
    "  meta = { parameter = true }\n"
    "\n"
    "  [[device.point]]\n"
    "  id = \"hidden_rw\"\n"
    "  datatype = \"uint16\"\n"
    "  access = \"read_write\"\n"
    "  address = { table = \"holding\", address = 21, count = 1 }\n"
    "  meta = { parameter = false }\n";

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

static char *write_temp_config(const char *body) {
    static char path[] = "/tmp/tdot-describe-XXXXXX.toml";
    char template[] = "/tmp/tdot-describe-XXXXXX";
    char *dir = mkdtemp(template);
    if (!dir) {
        perror("mkdtemp");
        exit(2);
    }
    snprintf(path, sizeof path, "%s/modbus.toml", dir);
    FILE *fp = fopen(path, "w");
    fputs(body, fp);
    fclose(fp);
    return path;
}

static const cJSON *prop(const cJSON *def, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(def, "jsonSchema"), "properties"),
        key);
}

static const char *str_of(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : "<missing>";
}

static double num_of(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : -1e300;
}

int main(void) {
    char *path = write_temp_config(CONFIG);
    char err[256];
    tdot_config_t *cfg = tdot_config_load(path, err, sizeof err);
    if (!cfg) {
        printf("FAIL: cannot load fixture config: %s\n", err);
        return 1;
    }

    /* ---- default set name + key validation ------------------------------- */
    char *dflt = tdot_param_default_set(cfg->protocol);
    CHECK(strcmp(dflt, "modbus_parameters") == 0, "default set = %s", dflt);
    char *opcua_set = tdot_param_default_set("opc-ua");
    CHECK(strcmp(opcua_set, "opc_ua_parameters") == 0, "opc-ua set = %s",
          opcua_set);
    free(opcua_set);
    CHECK(tdot_param_key_valid("ok_id_1"), "ok_id_1 should be a valid key");
    CHECK(!tdot_param_key_valid("Environment.Temperature"),
          "a dotted id must be rejected");
    CHECK(!tdot_param_key_valid(""), "an empty id must be rejected");

    /* ---- which points are parameters, and in which set ------------------- */
    struct {
        const char *point;
        const char *set;
    } want[] = {
        {"temp_u16", "modbus_parameters"},
        {"coil_rw", "modbus_parameters"},
        {"pump_speed", "pump"},
        {"status_word", "modbus_parameters"},
    };
    size_t nwant = sizeof want / sizeof *want, found = 0;
    for (size_t j = 0; j < cfg->devices[0].npoints; j++) {
        const tdot_point_t *pt = &cfg->devices[0].points[j];
        char *set = NULL;
        if (!tdot_param_of(pt, dflt, &set))
            continue;
        if (found < nwant) {
            CHECK(strcmp(pt->id, want[found].point) == 0,
                  "parameter #%zu is %s, wanted %s", found, pt->id,
                  want[found].point);
            CHECK(strcmp(set, want[found].set) == 0, "%s set is %s, wanted %s",
                  pt->id, set, want[found].set);
        }
        found++;
        free(set);
    }
    CHECK(found == nwant, "%zu parameters derived, wanted %zu", found, nwant);

    /* Invalid keys are reported, valid ones are not. */
    char *bad = tdot_param_invalid_keys(cfg, dflt);
    CHECK(bad == NULL, "fixture reported invalid keys: %s", bad ? bad : "");
    free(bad);
    bad = tdot_param_invalid_keys(cfg, "plant.floor");
    CHECK(bad && strstr(bad, "parameter set 'plant.floor'"),
          "a dotted default set must be reported, got '%s'", bad ? bad : "");
    free(bad);
    free(cfg->devices[0].points[0].id);
    cfg->devices[0].points[0].id = strdup("Boiler.Temp");
    bad = tdot_param_invalid_keys(cfg, dflt);
    CHECK(bad && strcmp(bad, "point id 'Boiler.Temp'") == 0,
          "a dotted point id must be reported, got '%s'", bad ? bad : "");
    free(bad);
    free(cfg->devices[0].points[0].id);
    cfg->devices[0].points[0].id = strdup("temp_u16");

    /* ---- DTM rendering --------------------------------------------------- */
    cJSON *docs = tdot_c8y_dtm_definitions(cfg, NULL);
    CHECK(cJSON_GetArraySize(docs) == 2, "%d definitions, wanted 2",
          cJSON_GetArraySize(docs));
    const cJSON *main_def = cJSON_GetArrayItem(docs, 0);
    CHECK(strcmp(str_of(main_def, "identifier"), "modbus_parameters") == 0,
          "identifier = %s", str_of(main_def, "identifier"));
    const cJSON *contexts = cJSON_GetObjectItemCaseSensitive(main_def, "contexts");
    CHECK(cJSON_GetArraySize(contexts) == 3 &&
              strcmp(cJSON_GetArrayItem(contexts, 0)->valuestring, "asset") == 0 &&
              strcmp(cJSON_GetArrayItem(contexts, 1)->valuestring, "event") == 0 &&
              strcmp(cJSON_GetArrayItem(contexts, 2)->valuestring,
                     "operation") == 0,
          "contexts must be [asset, event, operation]");

    const cJSON *temp = prop(main_def, "temp_u16");
    CHECK(strcmp(str_of(temp, "type"), "integer") == 0, "temp_u16 type = %s",
          str_of(temp, "type"));
    CHECK(strcmp(str_of(temp, "title"), "Temperature setpoint") == 0,
          "temp_u16 title = %s", str_of(temp, "title"));
    CHECK(num_of(temp, "minimum") == 0.0, "temp_u16 minimum = %g",
          num_of(temp, "minimum"));
    CHECK(num_of(temp, "maximum") == 100.0, "temp_u16 maximum = %g",
          num_of(temp, "maximum"));
    CHECK(num_of(temp, "order") == 7, "temp_u16 order = %g",
          num_of(temp, "order"));
    /* meta.parameter.title wins over the point's `name` (asserted above), while
     * the point's own `description` is used -- there is no
     * meta.parameter.description here -- and still composes with the unit. */
    CHECK(strcmp(str_of(temp, "description"),
                 "Outlet temperature after the heat exchanger [°C]") == 0,
          "temp_u16 description = %s", str_of(temp, "description"));

    const cJSON *coil = prop(main_def, "coil_rw");
    /* With no meta at all, the point's `name` becomes the title. */
    CHECK(strcmp(str_of(coil, "title"), "Pump enable") == 0, "coil_rw title = %s",
          str_of(coil, "title"));
    CHECK(strcmp(str_of(coil, "type"), "boolean") == 0, "coil_rw type = %s",
          str_of(coil, "type"));
    CHECK(num_of(coil, "order") == 2, "coil_rw order = %g",
          num_of(coil, "order"));
    CHECK(!cJSON_GetObjectItemCaseSensitive(coil, "minimum"),
          "a boolean must carry no range");

    const cJSON *status = prop(main_def, "status_word");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(status, "readOnly")),
          "an opted-in read-only point must be readOnly");
    CHECK(num_of(status, "maximum") == 65535.0, "status_word maximum = %g",
          num_of(status, "maximum"));

    CHECK(!prop(main_def, "hidden_rw"), "meta.parameter = false must opt out");
    CHECK(!prop(main_def, "level_f32"), "a read-only point must not appear");

    const cJSON *pump = cJSON_GetArrayItem(docs, 1);
    CHECK(strcmp(str_of(pump, "identifier"), "pump") == 0, "identifier = %s",
          str_of(pump, "identifier"));
    CHECK(strcmp(str_of(cJSON_GetObjectItemCaseSensitive(pump, "jsonSchema"),
                        "title"),
                 "Pump") == 0,
          "pump title = %s",
          str_of(cJSON_GetObjectItemCaseSensitive(pump, "jsonSchema"), "title"));
    const cJSON *speed = prop(pump, "pump_speed");
    CHECK(strcmp(str_of(speed, "type"), "number") == 0, "pump_speed type = %s",
          str_of(speed, "type"));
    CHECK(strstr(str_of(speed, "description"), "write-only") != NULL,
          "a write-only point must say so: %s", str_of(speed, "description"));
    cJSON_Delete(docs);

    /* The title of a multi-word set capitalizes only the first word, and an
     * explicit default set overrides the protocol-derived one. */
    docs = tdot_c8y_dtm_definitions(cfg, "plant_settings");
    CHECK(strcmp(str_of(cJSON_GetArrayItem(docs, 0), "identifier"),
                 "plant_settings") == 0,
          "overridden identifier = %s",
          str_of(cJSON_GetArrayItem(docs, 0), "identifier"));
    CHECK(strcmp(str_of(cJSON_GetObjectItemCaseSensitive(
                            cJSON_GetArrayItem(docs, 0), "jsonSchema"),
                        "title"),
                 "Plant settings") == 0,
          "overridden title = %s",
          str_of(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(docs, 0),
                                                  "jsonSchema"),
                 "title"));
    cJSON_Delete(docs);

    free(dflt);
    tdot_config_free(cfg);
    unlink(path);
    if (failures) {
        printf("describe: %d check(s) failed\n", failures);
        return 1;
    }
    puts("describe: all checks passed");
    return 0;
}
