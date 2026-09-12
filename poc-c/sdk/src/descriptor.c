#include "tedge_dot/descriptor.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *tdot_param_default_set(const char *protocol) {
    size_t n = protocol ? strlen(protocol) : 0;
    char *out = malloc(n + sizeof("_parameters"));
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)protocol[i];
        out[i] = isalnum(c) ? (char)c : '_';
    }
    strcpy(out + n, "_parameters");
    return out;
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

static char *set_name_of(const cJSON *options, const char *default_set) {
    const cJSON *set = cJSON_GetObjectItemCaseSensitive(options, "set");
    return strdup(cJSON_IsString(set) ? set->valuestring : default_set);
}

bool tdot_param_of(const tdot_point_t *point, const char *default_set,
                   char **set_out) {
    cJSON *options = parameter_options(point);
    if (!options)
        return false;
    if (set_out)
        *set_out = set_name_of(options, default_set);
    cJSON_Delete(options);
    return true;
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

char *tdot_param_invalid_keys(const tdot_config_t *cfg,
                              const char *default_set) {
    char *buf = NULL;
    size_t len = 0;
    for (size_t i = 0; i < cfg->ndevices; i++) {
        const tdot_device_t *dev = &cfg->devices[i];
        for (size_t j = 0; j < dev->npoints; j++) {
            const tdot_point_t *pt = &dev->points[j];
            char *set = NULL;
            if (!tdot_param_of(pt, default_set, &set))
                continue;
            char item[256];
            if (!tdot_param_key_valid(pt->id)) {
                snprintf(item, sizeof item, "point id '%s'", pt->id);
                append(&buf, &len, ", ", item);
            }
            if (!tdot_param_key_valid(set)) {
                snprintf(item, sizeof item, "parameter set '%s'", set);
                append(&buf, &len, ", ", item);
            }
            free(set);
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

    const char *title = opt_string(options, "title");
    cJSON_AddStringToObject(schema, "title", title ? title : point->id);

    char description[320] = "";
    const char *d = opt_string(options, "description");
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

/* "modbus_parameters" -> "Modbus parameters" (only the first word is
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

cJSON *tdot_c8y_dtm_definitions(const tdot_config_t *cfg,
                                const char *default_set) {
    char *owned_default = default_set ? NULL
                                      : tdot_param_default_set(cfg->protocol);
    const char *dflt = default_set ? default_set : owned_default;

    /* One entry per set, in configuration order; `properties` keeps the order
     * the points are configured in so the default `order` matches Rust. */
    cJSON *sets = cJSON_CreateObject(); /* set name -> properties object */
    for (size_t i = 0; i < cfg->ndevices; i++) {
        const tdot_device_t *dev = &cfg->devices[i];
        for (size_t j = 0; j < dev->npoints; j++) {
            const tdot_point_t *pt = &dev->points[j];
            cJSON *options = parameter_options(pt);
            if (!options)
                continue;
            char *set = set_name_of(options, dflt);
            cJSON *props = cJSON_GetObjectItemCaseSensitive(sets, set);
            if (!props)
                props = cJSON_AddObjectToObject(sets, set);
            if (!cJSON_GetObjectItemCaseSensitive(props, pt->id)) /* first definition wins */
                cJSON_AddItemToObject(props, pt->id,
                                      property_schema(pt, options));
            free(set);
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
    free(owned_default);
    return docs;
}
