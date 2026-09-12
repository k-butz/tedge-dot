/* tedge-dot C SDK — device *parameters* and their Cumulocity DTM declaration.
 *
 * Mirrors impl/rust/crates/sdk/src/descriptor.rs: a parameter is a point whose `access`
 * permits writes, plus any point that opts in through `meta.parameter`
 * (`meta.parameter = false` opts a writable point out). Parameters are grouped
 * into sets: one set is one twin fragment on the device (published by the
 * ot-parameter-state flow) and one Digital Twin Manager property definition in
 * the tenant, rendered here for `tedge-dot describe`. The device itself never
 * talks to the DTM service — see doc/rfc/0003-parameter-writes.md.
 *
 * A DTM identifier is tenant-wide, so a set name is qualified by the *device
 * type* — what decides which points exist — rather than by the protocol, which
 * says nothing about them (§5.2):
 *
 *   <device type, else the protocol>_<group, default "control">_parameters
 *
 * `meta.parameter.group` names a second set of the same device type;
 * `meta.parameter.set` bypasses the rule and is used verbatim.
 */
#ifndef TDOT_DESCRIPTOR_H
#define TDOT_DESCRIPTOR_H

#include <stdbool.h>

#include "cjson/cJSON.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The group a parameter belongs to when it names none. */
#define TDOT_PARAM_DEFAULT_GROUP "control"

/* A parameter set name: `<qualifier>_<group>_parameters`, with any character
 * outside [A-Za-z0-9] replaced by '_'. Caller frees. */
char *tdot_param_set_name(const char *qualifier, const char *group);

/* How one device's parameter sets are named. Built per device, because the
 * qualifier is the device's own type. `forced` is `describe --set <name>` (and
 * the ot-parameter-state flow's `default_set`): one set name for everything
 * that does not give an absolute one. Both pointers are borrowed. */
typedef struct {
    const char *forced;    /* NULL unless --set was given */
    const char *qualifier; /* device type, else the protocol */
} tdot_set_naming_t;

/* The naming of `dev`'s sets. `dev` may be NULL (protocol-qualified only). */
tdot_set_naming_t tdot_param_naming(const tdot_device_t *dev,
                                    const char *protocol, const char *forced);

/* True when `key` can be used verbatim as a twin fragment key (Cumulocity
 * rejects '.' and '$'). */
bool tdot_param_key_valid(const char *key);

/* True when the point is a parameter. When it is and `set_out` is non-NULL,
 * `*set_out` receives the newly allocated set name (caller frees). */
bool tdot_param_of(const tdot_point_t *point, const tdot_set_naming_t *naming,
                   char **set_out);

/* Parameter ids and set names that cannot be used as fragment keys, joined with
 * ", " (e.g. "point id 'Boiler.Temp'"). NULL when every key is usable.
 * `forced` may be NULL. Caller frees. */
char *tdot_param_invalid_keys(const tdot_config_t *cfg, const char *forced);

/* Names of the devices that expose parameters without declaring a `type`, so
 * their sets fall back to the protocol — which every other device type on that
 * protocol also falls back to. Joined with ", ", NULL when there are none;
 * `describe` warns about them. Caller frees. */
char *tdot_param_untyped_devices(const tdot_config_t *cfg);

/* Cumulocity Digital Twin Manager property definitions — one per parameter set
 * — as a cJSON array. Each element is the request body of
 * POST /service/dtm/definitions/properties, which a tenant admin registers
 * once. `forced` may be NULL for the derived set names. Sets appearing
 * on several devices are merged (the identifier is tenant-wide); for a key
 * defined twice the first definition wins. Caller cJSON_Delete()s the result. */
cJSON *tdot_c8y_dtm_definitions(const tdot_config_t *cfg, const char *forced);

#ifdef __cplusplus
}
#endif

#endif /* TDOT_DESCRIPTOR_H */
