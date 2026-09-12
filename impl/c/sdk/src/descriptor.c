#include "tedge_dot/descriptor.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fold `src` into `dst` in place: every RUN of characters outside [A-Za-z0-9]
 * becomes a single '_', so a device type or group can be written the way it
 * reads ("acme-meter-v2") and still be a valid fragment key. A run rather than
 * a character because this folds bytes while the Rust implementation folds
 * chars: collapsing runs is what makes them agree on a name with a non-ASCII
 * character in it (one multi-byte character = one run either way). */
static void sanitize_in_place(char *s) {
    size_t o = 0;
    bool last_was_sep = false;
    for (size_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c)) {
            s[o++] = (char)c;
            last_was_sep = false;
        } else if (!last_was_sep) {
            s[o++] = '_';
            last_was_sep = true;
        }
    }
    s[o] = '\0';
}

char *tdot_param_set_name(const char *qualifier, const char *group) {
    if (!group || !*group)
        group = TDOT_PARAM_DEFAULT_GROUP;
    if (!qualifier)
        qualifier = "";
    /* Assembled first and sanitized as a whole, so a qualifier that already
     * ends in a separator does not produce a doubled '_'. */
    size_t n = strlen(qualifier) + 1 + strlen(group) + sizeof("_parameters");
    char *out = malloc(n);
    snprintf(out, n, "%s_%s_parameters", qualifier, group);
    sanitize_in_place(out);
    return out;
}

tdot_set_naming_t tdot_param_naming(const tdot_device_t *dev,
                                    const char *protocol, const char *forced) {
    tdot_set_naming_t naming = {forced, protocol};
    if (dev && dev->type && *dev->type)
        naming.qualifier = dev->type;
    return naming;
}

bool tdot_param_key_valid(const char *key) {
    if (!key || !*key)
        return false;
    for (const char *p = key; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_')
            return false;
    return true;
}

/* The point's `meta.parameter` normalized to an object, or NULL when the point
 * is not a parameter. Caller cJSON_Delete()s the result.
 * Mirrors descriptor.rs::parameter_of. */
static cJSON *parameter_options(const tdot_point_t *point) {
    cJSON *meta = point->meta_json ? cJSON_Parse(point->meta_json) : NULL;
    cJSON *param =
        meta ? cJSON_GetObjectItemCaseSensitive(meta, "parameter") : NULL;
    cJSON *options = NULL;

    if (param && cJSON_IsFalse(param)) { /* explicit opt-out */
        cJSON_Delete(meta);
        return NULL;
    }
    if (param) {
        if (cJSON_IsObject(param))
            options = cJSON_Duplicate(param, 1);
        else if (cJSON_IsString(param)) {
            options = cJSON_CreateObject(); /* a bare string names the set */
            cJSON_AddStringToObject(options, "set", param->valuestring);
        } else {
            options = cJSON_CreateObject(); /* `true`, or any other scalar */
        }
    } else if (point->access & TDOT_ACCESS_WRITE) {
        options = cJSON_CreateObject(); /* writable, no meta.parameter */
    }
    cJSON_Delete(meta);
    return options;
}

/* Append `name` to a growing string list unless it is empty or already there.
 * Takes ownership of `name` (frees it when it is a duplicate). */
static void push_name(char ***list, size_t *n, char *name) {
    if (!name || !*name) {
        free(name);
        return;
    }
    for (size_t i = 0; i < *n; i++)
        if (strcmp((*list)[i], name) == 0) {
            free(name);
            return;
        }
    *list = realloc(*list, (*n + 1) * sizeof **list);
    (*list)[(*n)++] = name;
}

/* The names a `set`/`group` option holds: one string, or an array of them.
 * Empty and non-string entries are ignored, so a mistyped entry degrades to the
 * default group rather than inventing a set name. Mirrors
 * descriptor.rs::names_of. */
static char **names_of(const cJSON *options, const char *key, size_t *n) {
    char **names = NULL;
    *n = 0;
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(options, key);
    if (cJSON_IsString(value)) {
        push_name(&names, n, strdup(value->valuestring));
    } else if (cJSON_IsArray(value)) {
        const cJSON *item;
        cJSON_ArrayForEach(item, value) {
            if (cJSON_IsString(item))
                push_name(&names, n, strdup(item->valuestring));
        }
    }
    return names;
}

/* Every set the options put the point in. `set` is absolute (used verbatim) and
 * wins over `group`; each accepts a string or a list.
 * Mirrors descriptor.rs::SetNaming::sets_of. */
static char **sets_of(const cJSON *options, const tdot_set_naming_t *naming,
                      size_t *n) {
    char **sets = names_of(options, "set", n);
    if (*n)
        return sets; /* absolute */
    if (naming->forced) {
        push_name(&sets, n, strdup(naming->forced));
        return sets;
    }
    size_t ngroups = 0;
    char **groups = names_of(options, "group", &ngroups);
    if (!ngroups) {
        push_name(&sets, n,
                  tdot_param_set_name(naming->qualifier,
                                      TDOT_PARAM_DEFAULT_GROUP));
    }
    /* Deduped on the resulting names, not the group names: two groups can fold
     * to one set ("a b" and "a-b") and a point must not appear twice in one
     * definition. */
    for (size_t i = 0; i < ngroups; i++)
        push_name(&sets, n, tdot_param_set_name(naming->qualifier, groups[i]));
    tdot_param_sets_free(groups, ngroups);
    return sets;
}

char **tdot_param_sets(const tdot_point_t *point,
                       const tdot_set_naming_t *naming, size_t *n) {
    *n = 0;
    cJSON *options = parameter_options(point);
    if (!options)
        return NULL;
    char **sets = sets_of(options, naming, n);
    cJSON_Delete(options);
    return sets;
}

void tdot_param_sets_free(char **sets, size_t n) {
    for (size_t i = 0; i < n; i++)
        free(sets[i]);
    free(sets);
}

bool tdot_param_is(const tdot_point_t *point, const tdot_set_naming_t *naming) {
    size_t n = 0;
    char **sets = tdot_param_sets(point, naming, &n);
    bool is = sets != NULL;
    tdot_param_sets_free(sets, n);
    return is;
}

/* Append "sep"-joined text to a growing heap string. */
static void append(char **buf, size_t *len, const char *sep, const char *text) {
    size_t add = strlen(text) + (*len ? strlen(sep) : 0);
    *buf = realloc(*buf, *len + add + 1);
    if (*len)
        memcpy(*buf + *len, sep, strlen(sep));
    strcpy(*buf + *len + (*len ? strlen(sep) : 0), text);
    *len += add;
}

char *tdot_param_invalid_keys(const tdot_config_t *cfg, const char *forced) {
    char *buf = NULL;
    size_t len = 0;
    for (size_t i = 0; i < cfg->ndevices; i++) {
        const tdot_device_t *dev = &cfg->devices[i];
        tdot_set_naming_t naming =
            tdot_param_naming(dev, cfg->protocol, forced);
        for (size_t j = 0; j < dev->npoints; j++) {
            const tdot_point_t *pt = &dev->points[j];
            size_t nsets = 0;
            char **sets = tdot_param_sets(pt, &naming, &nsets);
            if (!sets)
                continue;
            char item[256];
            if (!tdot_param_key_valid(pt->id)) {
                snprintf(item, sizeof item, "point id '%s'", pt->id);
                append(&buf, &len, ", ", item);
            }
            for (size_t k = 0; k < nsets; k++)
                if (!tdot_param_key_valid(sets[k])) {
                    snprintf(item, sizeof item, "parameter set '%s'", sets[k]);
                    append(&buf, &len, ", ", item);
                }
            tdot_param_sets_free(sets, nsets);
        }
    }
    return buf;
}

char *tdot_param_untyped_devices(const tdot_config_t *cfg) {
    char *buf = NULL;
    size_t len = 0;
    for (size_t i = 0; i < cfg->ndevices; i++) {
        const tdot_device_t *dev = &cfg->devices[i];
        if (dev->type && *dev->type)
            continue;
        tdot_set_naming_t naming = tdot_param_naming(dev, cfg->protocol, NULL);
        for (size_t j = 0; j < dev->npoints; j++) {
            if (tdot_param_is(&dev->points[j], &naming)) {
                append(&buf, &len, ", ", dev->name);
                break;
            }
        }
    }
    return buf;
}

/* JSON-schema type and datatype range for a point's datatype. 64-bit limits
 * exceed the JS safe range, so they are left unbounded (as in Rust). */
static const char *schema_type(tdot_datatype_t dt, bool *has_range, double *min,
                               double *max) {
    *has_range = true;
    switch (dt) {
    case TDOT_DT_BOOL:
        *has_range = false;
        return "boolean";
    case TDOT_DT_INT8:
        *min = -128.0, *max = 127.0;
        return "integer";
    case TDOT_DT_UINT8:
        *min = 0.0, *max = 255.0;
        return "integer";
    case TDOT_DT_INT16:
        *min = -32768.0, *max = 32767.0;
        return "integer";
    case TDOT_DT_UINT16:
        *min = 0.0, *max = 65535.0;
        return "integer";
    case TDOT_DT_INT32:
        *min = -2147483648.0, *max = 2147483647.0;
        return "integer";
    case TDOT_DT_UINT32:
        *min = 0.0, *max = 4294967295.0;
        return "integer";
    case TDOT_DT_INT64:
    case TDOT_DT_UINT64:
        *has_range = false;
        return "integer";
    case TDOT_DT_FLOAT32:
    case TDOT_DT_FLOAT64:
        *has_range = false;
        return "number";
    default: /* string / raw / unset */
        *has_range = false;
        return "string";
    }
}

static const char *opt_string(const cJSON *options, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(options, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* JSON-schema property for one parameter: type and limits from the datatype,
 * everything else from `meta.parameter`. */
static cJSON *property_schema(const tdot_point_t *point, const cJSON *options) {
    cJSON *schema = cJSON_CreateObject();
    bool has_range = false;
    double min = 0, max = 0;
    cJSON_AddStringToObject(schema, "type",
                            schema_type(point->datatype, &has_range, &min, &max));

    /* meta.parameter.title wins, then the point's own `name`, then the id: a
     * point can carry a general-purpose label and still say something
     * different in the parameter UI (mirrors descriptor.rs property_schema). */
    const char *title = opt_string(options, "title");
    if (!title)
        title = point->name;
    cJSON_AddStringToObject(schema, "title", title ? title : point->id);

    char description[320] = "";
    const char *d = opt_string(options, "description");
    if (!d)
        d = point->description;
    if (d)
        snprintf(description, sizeof description, "%s", d);
    if (point->unit) {
        size_t n = strlen(description);
        snprintf(description + n, sizeof description - n, "%s[%s]",
                 n ? " " : "", point->unit);
    }
    if (point->access == TDOT_ACCESS_WRITE) {
        size_t n = strlen(description);
        snprintf(description + n, sizeof description - n,
                 "%s(write-only: shows the last value written)", n ? " " : "");
    }
    if (*description)
        cJSON_AddStringToObject(schema, "description", description);

    const cJSON *opt_min = cJSON_GetObjectItemCaseSensitive(options, "min");
    const cJSON *opt_max = cJSON_GetObjectItemCaseSensitive(options, "max");
    if (cJSON_IsNumber(opt_min))
        cJSON_AddNumberToObject(schema, "minimum", opt_min->valuedouble);
    else if (has_range)
        cJSON_AddNumberToObject(schema, "minimum", min);
    if (cJSON_IsNumber(opt_max))
        cJSON_AddNumberToObject(schema, "maximum", opt_max->valuedouble);
    else if (has_range)
        cJSON_AddNumberToObject(schema, "maximum", max);

    static const char *passthrough[] = {"enum", "default", "order"};
    for (size_t i = 0; i < sizeof passthrough / sizeof *passthrough; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(options, passthrough[i]);
        if (v)
            cJSON_AddItemToObject(schema, passthrough[i], cJSON_Duplicate(v, 1));
    }
    if (!(point->access & TDOT_ACCESS_WRITE))
        cJSON_AddTrueToObject(schema, "readOnly");
    return schema;
}

/* "modbus_control_parameters" -> "Modbus control parameters" (only the first word is
 * capitalized, as in Rust's title_from_key). Writes into `out`. */
static void title_from_key(const char *key, char *out, size_t outlen) {
    size_t o = 0;
    bool first_word = true, word_start = true;
    for (const char *p = key; *p && o + 1 < outlen; p++) {
        if (*p == '_') {
            if (!word_start) {
                word_start = true;
                first_word = false;
            }
            continue;
        }
        if (word_start) {
            if (!first_word && o + 1 < outlen)
                out[o++] = ' ';
            out[o++] = first_word ? (char)toupper((unsigned char)*p) : *p;
            word_start = false;
            continue;
        }
        out[o++] = *p;
    }
    out[o] = '\0';
}

cJSON *tdot_c8y_dtm_definitions(const tdot_config_t *cfg, const char *forced) {
    /* One entry per set, in configuration order; `properties` keeps the order
     * the points are configured in so the default `order` matches Rust. */
    cJSON *sets = cJSON_CreateObject(); /* set name -> properties object */
    for (size_t i = 0; i < cfg->ndevices; i++) {
        const tdot_device_t *dev = &cfg->devices[i];
        tdot_set_naming_t naming =
            tdot_param_naming(dev, cfg->protocol, forced);
        for (size_t j = 0; j < dev->npoints; j++) {
            const tdot_point_t *pt = &dev->points[j];
            cJSON *options = parameter_options(pt);
            if (!options)
                continue;
            size_t nsets = 0;
            char **point_sets = sets_of(options, &naming, &nsets);
            for (size_t k = 0; k < nsets; k++) {
                cJSON *props =
                    cJSON_GetObjectItemCaseSensitive(sets, point_sets[k]);
                if (!props)
                    props = cJSON_AddObjectToObject(sets, point_sets[k]);
                if (!cJSON_GetObjectItemCaseSensitive(props, pt->id)) /* first definition wins */
                    cJSON_AddItemToObject(props, pt->id,
                                          property_schema(pt, options));
            }
            tdot_param_sets_free(point_sets, nsets);
            cJSON_Delete(options);
        }
    }

    cJSON *docs = cJSON_CreateArray();
    cJSON *props;
    cJSON_ArrayForEach(props, sets) {
        cJSON *doc = cJSON_CreateObject();
        cJSON_AddStringToObject(doc, "identifier", props->string);

        cJSON *schema = cJSON_AddObjectToObject(doc, "jsonSchema");
        cJSON_AddStringToObject(schema, "$schema",
                                "http://json-schema.org/draft-07/schema#");
        char title[256];
        title_from_key(props->string, title, sizeof title);
        cJSON_AddStringToObject(schema, "title", title);
        char description[320];
        snprintf(description, sizeof description,
                 "Writable %s points exposed by tedge-dot (generated from the "
                 "connector configuration)",
                 cfg->protocol);
        cJSON_AddStringToObject(schema, "description", description);
        cJSON_AddStringToObject(schema, "type", "object");

        /* Properties without an explicit `order` get their 1-based position. */
        int position = 0;
        cJSON *prop;
        cJSON_ArrayForEach(prop, props) {
            position++;
            if (!cJSON_GetObjectItemCaseSensitive(prop, "order"))
                cJSON_AddNumberToObject(prop, "order", position);
        }
        cJSON_AddItemToObject(schema, "properties", cJSON_Duplicate(props, 1));

        cJSON *contexts = cJSON_AddArrayToObject(doc, "contexts");
        cJSON_AddItemToArray(contexts, cJSON_CreateString("asset"));
        cJSON_AddItemToArray(contexts, cJSON_CreateString("event"));
        cJSON_AddItemToArray(contexts, cJSON_CreateString("operation"));

        cJSON *tags = cJSON_AddArrayToObject(doc, "tags");
        cJSON_AddItemToArray(tags, cJSON_CreateString("tedge-dot"));
        cJSON_AddItemToArray(tags, cJSON_CreateString(cfg->protocol));

        cJSON_AddItemToArray(docs, doc);
    }
    cJSON_Delete(sets);
    return docs;
}
