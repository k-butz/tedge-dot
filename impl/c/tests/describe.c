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
    "type = \"acme-boiler-v2\"\n"
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
    "  meta = { parameter = false }\n"
    "\n"
    "  [[device.point]]\n"
    "  id = \"commission_code\"\n"
    "  datatype = \"uint16\"\n"
    "  access = \"read_write\"\n"
    "  address = { table = \"holding\", address = 22, count = 1 }\n"
    "  meta = { parameter = { group = \"commissioning\" } }\n"
    "\n"
    "# A device of an undeclared type: its sets fall back to the protocol.\n"
    "[[device]]\n"
    "name = \"plc2\"\n"
    "protocol_address = { transport = \"tcp\", host = \"127.0.0.1\", port = 503, unit_id = 1 }\n"
    "\n"
    "  [[device.point]]\n"
    "  id = \"spare_rw\"\n"
    "  datatype = \"uint16\"\n"
    "  access = \"read_write\"\n"
    "  address = { table = \"holding\", address = 30, count = 1 }\n";

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

    /* ---- set naming + key validation ------------------------------------- */
    /* A set name is qualified by the device *type* -- what decides which points
     * exist -- and only falls back to the protocol when none is declared. */
    char *opcua_set =
        tdot_param_set_name("opc-ua", TDOT_PARAM_DEFAULT_GROUP);
    CHECK(strcmp(opcua_set, "opc_ua_control_parameters") == 0, "opc-ua set = %s",
          opcua_set);
    free(opcua_set);
    char *pump_set = tdot_param_set_name("ACME meter v2", "pump");
    CHECK(strcmp(pump_set, "ACME_meter_v2_pump_parameters") == 0,
          "grouped set = %s", pump_set);
    free(pump_set);
    /* A run of separators (or one multi-byte character) folds to a single '_',
     * which is what keeps this identical to the Rust implementation's char-wise
     * fold. Mirrors descriptor.rs::key_validation_and_set_names. */
    char *run_set = tdot_param_set_name("acme -- v2", "control");
    CHECK(strcmp(run_set, "acme_v2_control_parameters") == 0, "run set = %s",
          run_set);
    free(run_set);
    char *utf8_set = tdot_param_set_name("wärmezähler", "control");
    CHECK(strcmp(utf8_set, "w_rmez_hler_control_parameters") == 0,
          "utf-8 set = %s", utf8_set);
    free(utf8_set);
    CHECK(cfg->devices[0].type && strcmp(cfg->devices[0].type, "acme-boiler-v2") == 0,
          "plc1 type = %s", cfg->devices[0].type ? cfg->devices[0].type : "<none>");
    CHECK(cfg->devices[1].type == NULL, "plc2 must have no type");
    CHECK(tdot_param_key_valid("ok_id_1"), "ok_id_1 should be a valid key");
    CHECK(!tdot_param_key_valid("Environment.Temperature"),
          "a dotted id must be rejected");
    CHECK(!tdot_param_key_valid(""), "an empty id must be rejected");

    /* ---- which points are parameters, and in which set ------------------- */
    struct {
        const char *point;
        const char *set;
    } want[] = {
        {"temp_u16", "acme_boiler_v2_control_parameters"},
        {"coil_rw", "acme_boiler_v2_control_parameters"},
        {"pump_speed", "pump"}, /* meta.parameter = "<name>" is absolute */
        {"status_word", "acme_boiler_v2_control_parameters"},
        {"commission_code", "acme_boiler_v2_commissioning_parameters"},
        /* plc2 declares no type: back to the protocol, which is what collides
         * across device types and is the reason `describe` warns about it. */
        {"spare_rw", "modbus_control_parameters"},
    };
    size_t nwant = sizeof want / sizeof *want, found = 0;
    for (size_t i = 0; i < cfg->ndevices; i++) {
        tdot_set_naming_t naming =
            tdot_param_naming(&cfg->devices[i], cfg->protocol, NULL);
        for (size_t j = 0; j < cfg->devices[i].npoints; j++) {
            const tdot_point_t *pt = &cfg->devices[i].points[j];
            char *set = NULL;
            if (!tdot_param_of(pt, &naming, &set))
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
    }
    CHECK(found == nwant, "%zu parameters derived, wanted %zu", found, nwant);

    /* The untyped device is the one `describe` warns about. */
    char *untyped = tdot_param_untyped_devices(cfg);
    CHECK(untyped && strcmp(untyped, "plc2") == 0, "untyped devices = %s",
          untyped ? untyped : "<none>");
    free(untyped);

    /* Invalid keys are reported, valid ones are not. */
    char *bad = tdot_param_invalid_keys(cfg, NULL);
    CHECK(bad == NULL, "fixture reported invalid keys: %s", bad ? bad : "");
    free(bad);
    bad = tdot_param_invalid_keys(cfg, "plant.floor");
    CHECK(bad && strstr(bad, "parameter set 'plant.floor'"),
          "a dotted default set must be reported, got '%s'", bad ? bad : "");
    free(bad);
    free(cfg->devices[0].points[0].id);
    cfg->devices[0].points[0].id = strdup("Boiler.Temp");
    bad = tdot_param_invalid_keys(cfg, NULL);
    CHECK(bad && strcmp(bad, "point id 'Boiler.Temp'") == 0,
          "a dotted point id must be reported, got '%s'", bad ? bad : "");
    free(bad);
    free(cfg->devices[0].points[0].id);
    cfg->devices[0].points[0].id = strdup("temp_u16");

    /* ---- DTM rendering --------------------------------------------------- */
    cJSON *docs = tdot_c8y_dtm_definitions(cfg, NULL);
    CHECK(cJSON_GetArraySize(docs) == 4, "%d definitions, wanted 4",
          cJSON_GetArraySize(docs));
    const cJSON *main_def = cJSON_GetArrayItem(docs, 0);
    CHECK(strcmp(str_of(main_def, "identifier"),
                 "acme_boiler_v2_control_parameters") == 0,
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

    /* One device type, two sets (the group), and one untyped device on the
     * protocol name. */
    CHECK(strcmp(str_of(cJSON_GetArrayItem(docs, 2), "identifier"),
                 "acme_boiler_v2_commissioning_parameters") == 0,
          "grouped identifier = %s",
          str_of(cJSON_GetArrayItem(docs, 2), "identifier"));
    CHECK(strcmp(str_of(cJSON_GetArrayItem(docs, 3), "identifier"),
                 "modbus_control_parameters") == 0,
          "untyped identifier = %s",
          str_of(cJSON_GetArrayItem(docs, 3), "identifier"));
    CHECK(prop(cJSON_GetArrayItem(docs, 3), "spare_rw") != NULL,
          "the untyped device's parameter must be rendered");
    cJSON_Delete(docs);

    /* The title of a multi-word set capitalizes only the first word, and an
     * explicit --set overrides every derived name. */
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

    tdot_config_free(cfg);
    unlink(path);
    if (failures) {
        printf("describe: %d check(s) failed\n", failures);
        return 1;
    }
    puts("describe: all checks passed");
    return 0;
}
