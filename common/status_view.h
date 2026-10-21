#pragma once
#include "json_build.h"
#include <string.h>
#include <stdbool.h>

/* Unknown views must not silently fall back to an expensive full snapshot. */
static inline bool status_view_params(json_object *params, bool *live)
{
    *live = false;
    if (!params || !json_object_is_type(params, json_type_object)) return false;
    json_object_object_foreach(params, key, value) {
        if (strcmp(key, "view") || !value ||
            !json_object_is_type(value, json_type_string)) return false;
        const char *view = json_object_get_string(value);
        if (json_object_get_string_len(value) != (int)strlen(view)) return false;
        if (!strcmp(view, "live")) *live = true;
        else if (strcmp(view, "full")) return false;
    }
    return true;
}

static inline void status_view_capabilities(json_object *out)
{
    json_object *views = json_object_new_array();
    json_build_array_add(views, json_object_new_string("full"));
    json_build_array_add(views, json_object_new_string("live"));
    json_build_object_add(out, "status_views", views);
}
