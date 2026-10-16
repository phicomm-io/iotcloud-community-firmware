#pragma once
#include "json.h"

/* Producer fields are required unless a call explicitly uses the nullable SDK
 * API. Keep old SDK consumers (including TC1) source-compatible. */
static inline void json_build_object_add(json_object *object, const char *key, json_object *value)
{
#ifdef JSON_ALLOCATION_SAFETY
    json_object_object_add_checked(object, key, value);
#else
    json_object_object_add(object, key, value);
#endif
}
static inline int json_build_array_add(json_object *array, json_object *value)
{
#ifdef JSON_ALLOCATION_SAFETY
    return json_object_array_add_checked(array, value);
#else
    return json_object_array_add(array, value);
#endif
}
static inline int json_build_array_put_idx(json_object *array, int index, json_object *value)
{
#ifdef JSON_ALLOCATION_SAFETY
    return json_object_array_put_idx_checked(array, index, value);
#else
    return json_object_array_put_idx(array, index, value);
#endif
}
