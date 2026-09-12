/* tedge-dot C SDK — device *parameters* and their Cumulocity DTM declaration.
 *
 * Mirrors crates/sdk/src/descriptor.rs: a parameter is a point whose `access`
 * permits writes, plus any point that opts in through `meta.parameter`
 * (`meta.parameter = false` opts a writable point out). Parameters are grouped
 * into sets (`meta.parameter.set`, default `<protocol>_parameters`): one set is
 * one twin fragment on the device (published by the ot-parameter-state flow)
 * and one Digital Twin Manager property definition in the tenant, rendered
 * here for `tedge-dot describe`. The device itself never talks to the DTM
 * service — see doc/rfc/0003-parameter-writes.md.
 */
#ifndef TDOT_DESCRIPTOR_H
#define TDOT_DESCRIPTOR_H

#include <stdbool.h>

#include "cjson/cJSON.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default parameter set name for a protocol: `<protocol>_parameters`, with any
 * character outside [A-Za-z0-9] replaced by '_'. Caller frees. */
char *tdot_param_default_set(const char *protocol);

/* True when `key` can be used verbatim as a twin fragment key (Cumulocity
 * rejects '.' and '$'). */
bool tdot_param_key_valid(const char *key);

/* True when the point is a parameter. When it is and `set_out` is non-NULL,
 * `*set_out` receives the newly allocated set name (caller frees). */
bool tdot_param_of(const tdot_point_t *point, const char *default_set,
                   char **set_out);

/* Parameter ids and set names that cannot be used as fragment keys, joined with
 * ", " (e.g. "point id 'Boiler.Temp'"). NULL when every key is usable.
 * Caller frees. */
char *tdot_param_invalid_keys(const tdot_config_t *cfg,
                              const char *default_set);

/* Cumulocity Digital Twin Manager property definitions — one per parameter set
 * — as a cJSON array. Each element is the request body of
 * POST /service/dtm/definitions/properties, which a tenant admin registers
 * once. `default_set` may be NULL for `<protocol>_parameters`. Sets appearing
 * on several devices are merged (the identifier is tenant-wide); for a key
 * defined twice the first definition wins. Caller cJSON_Delete()s the result. */
cJSON *tdot_c8y_dtm_definitions(const tdot_config_t *cfg,
                                const char *default_set);

#ifdef __cplusplus
}
#endif

#endif /* TDOT_DESCRIPTOR_H */
