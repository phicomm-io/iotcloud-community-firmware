/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "http.h"
#include "basic.h"
#include <string.h>
#include <strings.h>

void ph_http_parser_init(ph_http_parser *p)
{ memset(p, 0, sizeof(*p)); p->first = true; }
static bool copy(char *out, size_t capacity, const char *value)
{
    size_t n = strlen(value);
    if (n >= capacity) return false;
    memcpy(out, value, n + 1); return true;
}
static bool request_line(ph_http_parser *p)
{
    char *method = p->line, *path = strchr(method, ' ');
    if (!path || path == method) return false;
    *path++ = 0;
    char *version = strchr(path, ' ');
    if (!version || version == path) return false;
    *version++ = 0;
    if (strcmp(version, "HTTP/1.0") && strcmp(version, "HTTP/1.1")) return false;
    if (*path != '/' || strpbrk(path, "\t ")) return false;
    for (const char *s = method; *s; ++s) if (*s < 'A' || *s > 'Z') return false;
    return copy(p->request.method, sizeof(p->request.method), method) &&
           copy(p->request.path, sizeof(p->request.path), path);
}
static bool header_line(ph_http_parser *p)
{
    char *value = strchr(p->line, ':');
    if (!value || value == p->line) return false;
    for (char *s = p->line; s != value; ++s) {
        unsigned char c = *s;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || strchr("!#$%&'*+-.^_`|~", c))) return false;
    }
    *value++ = 0;
    while (*value == ' ' || *value == '\t') ++value;
    ph_http_request *r = &p->request;
    if (!strcasecmp(p->line, "Content-Length")) {
        if (r->have_length || !*value) return false;
        uint32_t n = 0;
        for (const char *s = value; *s; ++s) {
            if (*s < '0' || *s > '9' || n > (UINT32_MAX - (unsigned)(*s-'0'))/10u) return false;
            n = n*10u + (unsigned)(*s-'0');
        }
        r->length = n; r->have_length = true;
    } else if (!strcasecmp(p->line, "Transfer-Encoding") || !strcasecmp(p->line, "Expect")) {
        return false;
    } else if (!strcasecmp(p->line, "Authorization")) {
        if (!copy(r->authorization, sizeof(r->authorization), value)) return false;
    } else if (!strcasecmp(p->line, "Origin")) {
        if (!copy(r->origin, sizeof(r->origin), value)) return false;
    } else if (!strcasecmp(p->line, "If-None-Match")) {
        /* Oversized validators cannot match our bounded generated tag. */
        if (!copy(r->if_none_match, sizeof(r->if_none_match), value)) r->if_none_match[0] = 0;
    }
    /* Content-Type/User-Agent/unknown fields are not interpreted by any active
     * product handler. The strict line/aggregate budgets apply before ignoring. */
    return true;
}
int ph_http_parser_feed(ph_http_parser *p, unsigned char c)
{
    if (p->complete || ++p->bytes > PH_HTTP_HEADER_BYTES) return -1;
    if (p->cr) {
        if (c != '\n') return -1;
        p->cr = false; p->line[p->used] = 0;
        if (!p->first && !p->used) { p->complete = true; return 1; }
        if (++p->lines > 65) return -1; /* request line + 64 nonempty headers */
        bool valid = p->first ? request_line(p) : header_line(p);
        p->first = false; p->used = 0;
        return valid ? 0 : -1;
    }
    if (c == '\r') { p->cr = true; return 0; }
    if (c == '\n' || c == 127 || c < 32u) {
        if (c != '\t' || p->first) return -1;
    }
    if (p->used + 1u >= sizeof(p->line)) return -1;
    p->line[p->used++] = (char)c; return 0;
}
bool ph_http_basic(const char *authorization, const char *user, const char *password)
{
    return ph_http_basic_match(authorization, user, password);
}
