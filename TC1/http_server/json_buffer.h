#pragma once
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
/* One bounded response allocation; no per-field JSON tree on telemetry paths. */
typedef struct {
    char *data;
    size_t used, capacity;
    bool valid;
} JsonBuffer;
static inline void jb_printf(JsonBuffer *b, const char *format, ...) {
    if (!b->valid)
        return;
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(b->data + b->used, b->capacity - b->used, format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->capacity - b->used)
        b->valid = false;
    else
        b->used += n;
}
/* Bytes occupied by the string contents after JSON escaping (excluding quotes). */
static inline size_t jb_string_size(const char *text) {
    size_t size = 0;
    for (const unsigned char *c = (const unsigned char *)text; *c; ++c)
        size += *c < 32 ? 6 : (*c == '"' || *c == '\\' ? 2 : 1);
    return size;
}
static inline void jb_string(JsonBuffer *b, const char *text) {
    jb_printf(b, "\"");
    for (const unsigned char *c = (const unsigned char *)text; *c; ++c) {
        if (*c == '"' || *c == '\\')
            jb_printf(b, "\\%c", *c);
        else if (*c < 32)
            jb_printf(b, "\\u%04x", *c);
        else
            jb_printf(b, "%c", *c);
    }
    jb_printf(b, "\"");
}
static inline void jb_field(JsonBuffer *b, const char *key, const char *text) {
    jb_printf(b, "\"%s\":", key);
    jb_string(b, text);
    jb_printf(b, ",");
}
