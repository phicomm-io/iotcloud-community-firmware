/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "http.h"
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
/* Recognize binary discovery's ordered Accept alternatives as well as one
 * explicit business representation. Content-Type remains a single type. */
static int media_format(const char *s)
{
    size_t n=strcspn(s,";, \t");
    if(n==19 && !strncasecmp(s,"application/msgpack",n))return PH_WIRE_MSGPACK;
    if(n==16 && !strncasecmp(s,"application/cbor",n))return PH_WIRE_CBOR;
    if(n==16 && !strncasecmp(s,"application/json",n))return PH_WIRE_JSON;
    return -1;
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
    } else if (!strcasecmp(p->line, "Content-Type")) {
        if(r->have_type)return false;
        int f=strchr(value,',')?-1:media_format(value);r->have_type=f<0?2:1;
        r->content_format=f<0?PH_WIRE_JSON:(ph_wire_format)f;
    } else if (!strcasecmp(p->line, "Accept")) {
        if(r->have_accept)return false;
        int f=media_format(value);r->have_accept=f<0?2:1;
        r->accept_format=f<0?PH_WIRE_JSON:(ph_wire_format)f;
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
    /* Unknown fields remain subject to the strict aggregate budget. */
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
    if (!password[0]) return true;
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char raw[72], expected[104] = "Basic ";
    size_t u = strlen(user), p = strlen(password);
    if (u + p + 1 >= sizeof(raw)) return false;
    memcpy(raw, user, u); raw[u] = ':'; memcpy(raw + u + 1, password, p);
    size_t n = u + p + 1, pos = 6;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)(unsigned char)raw[i] << 16;
        if (i+1 < n) v |= (uint32_t)(unsigned char)raw[i+1] << 8;
        if (i+2 < n) v |= (unsigned char)raw[i+2];
        expected[pos++] = alphabet[v>>18]; expected[pos++] = alphabet[(v>>12)&63];
        expected[pos++] = i+1<n ? alphabet[(v>>6)&63] : '=';
        expected[pos++] = i+2<n ? alphabet[v&63] : '=';
    }
    expected[pos] = 0;
    if (strlen(authorization) != pos) return false;
    unsigned diff = 0;
    for (size_t i=0; i<pos; ++i) diff |= (unsigned char)authorization[i] ^ (unsigned char)expected[i];
    return diff == 0;
}
