/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "http.h"
#include <stdio.h>
#include <string.h>

static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Length: 15\r\nConnection: close\r\n\r\n{\"code\":\"busy\"}";
static const char invalid[] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
static bool expired(uint32_t now, uint32_t deadline)
{ return (int32_t)(now - deadline) >= 0; }
static void publish(ph_http_server *s)
{ if (s->io->publish) s->io->publish(&s->diagnostics); }
static void count(uint32_t *n) { if (*n != UINT32_MAX) ++*n; }
static bool close_fd(ph_http_server *s, int fd)
{
    int rc = s->io->close(fd);
    int error = rc < 0 ? s->io->error() : 0;
    s->diagnostics.last_close_rc = rc;
    s->diagnostics.last_close_errno = error;
    if (rc >= 0) return true;
    count(&s->diagnostics.close_failures);
    /* Ownership on SDK close failure is unspecified. Do not retry a potentially
     * reused global descriptor, or reopen a listener over an uncertain resource. */
    s->running = false;
    return false;
}
static void close_slot(ph_http_server *s, unsigned i, ph_http_close_reason why, bool sent)
{
    ph_http_slot *c = &s->slots[i];
    if (c->state == PH_SLOT_FREE) return;
    if (!close_fd(s, c->fd)) { why = PH_HTTP_CLOSED_IO; sent = false; }
    if (c->lease) { s->app->release(&s->exchange, sent); s->active = -1; }
    if (s->queued == (int)i) s->queued = -1;
    if(c->simple && c->reason==PH_HTTP_CLOSED_BUSY)
        count(sent?&s->diagnostics.busy_sent:&s->diagnostics.busy_failed);
    count(&s->diagnostics.closed[why]);
    c->state = PH_SLOT_FREE; c->fd = -1; c->lease = false;
}
static void simple_reply(ph_http_server *s, unsigned i, bool overload)
{
    ph_http_slot *c = &s->slots[i];
    c->simple = overload ? busy : invalid;
    c->header_sent = c->body_sent = 0; c->state = PH_SLOT_SEND;
    c->reason = overload ? PH_HTTP_CLOSED_BUSY : PH_HTTP_CLOSED_HEADER;
    c->deadline = s->io->now() + 100u;
    if (s->queued == (int)i) s->queued = -1;
    if (overload) count(&s->diagnostics.rejected);
}
static bool response_header(ph_http_server *s)
{
    const ph_http_response *r = &s->exchange.response;
    const char *status = r->status == 200 ? "OK" : r->status == 202 ? "Accepted" :
        r->status == 304 ? "Not Modified" : r->status == 401 ? "Unauthorized" :
        r->status == 403 ? "Forbidden" : r->status == 404 ? "Not Found" :
        r->status == 409 ? "Conflict" : r->status == 503 ? "Service Unavailable" : "Error";
    int n = snprintf(s->response_header, sizeof(s->response_header),
        "HTTP/1.1 %u %s\r\nConnection: close\r\n", r->status, status);
    if (n < 0 || (size_t)n >= sizeof(s->response_header)) return false;
    size_t used = (size_t)n;
#define ADD(...) do { n=snprintf(s->response_header+used,sizeof(s->response_header)-used,__VA_ARGS__); \
    if(n<0 || (size_t)n>=sizeof(s->response_header)-used)return false; \
    used+=(size_t)n; } while(0)
    if (r->status != 304) {
        ADD("Content-Type: %s\r\nContent-Length: %u\r\n", r->type ? r->type : "application/json", (unsigned)r->length);
    }
    ADD("Cache-Control: %s\r\n", r->private_cache ? "private, no-cache" : "no-store");
    if (r->private_cache) ADD("Vary: Authorization\r\n");
    if (r->etag) ADD("ETag: %s\r\n", r->etag);
    if (r->gzip && r->status != 304) ADD("Content-Encoding: gzip\r\n");
    if (r->realm) ADD("WWW-Authenticate: Basic realm=\"%s\"\r\n", r->realm);
    if (r->cors) ADD("%s", r->cors);
    ADD("\r\n");
#undef ADD
    s->response_header_length = used; return true;
}
static void respond(ph_http_server *s, unsigned i)
{
    ph_http_slot *c=&s->slots[i];
    if (!response_header(s)) { close_slot(s,i,PH_HTTP_CLOSED_IO,false); return; }
    c->simple=NULL; c->header_sent=c->body_sent=0;
    const ph_http_response *r=&s->exchange.response;
    /* Gather a small response into existing header tail. No optional TC1
     * socket option and no extra allocation; partial sends keep one offset. */
    if(!r->suppress_body && r->status!=304 && r->length) {
        size_t n=sizeof(s->response_header)-s->response_header_length;
        if(n>r->length)n=r->length;
        memcpy(s->response_header+s->response_header_length,r->body,n);
        s->response_header_length+=n;c->body_sent=n;
    }
    c->state=PH_SLOT_SEND;
    c->reason=PH_HTTP_CLOSED_OK; c->deadline=s->io->now()+PH_HTTP_RESPONSE_MS;
}
static void action(ph_http_server *s, unsigned i, ph_http_action next)
{
    ph_http_slot *c=&s->slots[i]; uint32_t now=s->io->now();
    if (next == PH_HTTP_WAIT) {
        c->state=PH_SLOT_WAIT; c->deadline=now+2000u; c->idle_deadline=now+25u;
    } else if (next == PH_HTTP_READ_BODY) {
        c->state=PH_SLOT_BODY;
        c->remaining=c->parser.request.length;
        c->deadline=now+s->exchange.body_total_ms;
        c->idle_deadline=now+s->exchange.body_idle_ms;
        if (!c->remaining) { s->app->complete(&s->exchange); respond(s,i); }
    } else respond(s,i);
}
static void acquire(ph_http_server *s, unsigned i)
{
    ph_http_slot *c=&s->slots[i];
    s->active=(int)i; if(s->queued==(int)i)s->queued=-1;
    c->lease=true;
    memset(&s->exchange,0,sizeof(s->exchange));
    s->exchange.request=&c->parser.request;
    s->exchange.response.status=503;
    s->exchange.body_idle_ms=5000u; s->exchange.body_total_ms=15000u;
    action(s,i,s->app->begin(&s->exchange));
}
static void ready(ph_http_server *s, unsigned i)
{
    ph_http_slot *c=&s->slots[i];
    c->remaining=c->parser.request.length;
    if(s->active<0) { acquire(s,i); return; }
    if(s->queued<0) {
        s->queued=(int)i; count(&s->diagnostics.queued);c->state=PH_SLOT_READY;
        c->deadline=s->io->now()+PH_HTTP_READY_MS;
    } else simple_reply(s,i,true);
}
bool ph_http_server_open(ph_http_server *s, const ph_http_io *io, const ph_http_app *app, unsigned cap)
{
    memset(s,0,sizeof(*s)); s->io=io; s->app=app;
    s->listener=s->active=s->queued=-1;
    for(unsigned i=0;i<PH_HTTP_CLIENT_SLOTS;++i)s->slots[i].fd=-1;
    s->diagnostics.last_fd=-1;
    s->diagnostics.slot_bytes=sizeof(ph_http_slot); s->diagnostics.server_bytes=sizeof(*s);
    if(!cap || cap>PH_HTTP_CLIENT_SLOTS)return false;
    s->listener=io->open(cap);
    if(s->listener<0)return false;
    if(s->listener>=PH_HTTP_FD_LIMIT || io->limit(s->listener,cap,&s->diagnostics)<0 ||
       io->nonblock(s->listener)<0) {
        close_fd(s,s->listener); s->listener=-1; publish(s); return false;
    }
    s->cap=cap; s->running=true; publish(s); return true;
}
void ph_http_server_close(ph_http_server *s)
{
    for(unsigned i=0;i<PH_HTTP_CLIENT_SLOTS;++i)close_slot(s,i,PH_HTTP_CLOSED_STOP,false);
    if(s->listener>=0)close_fd(s,s->listener);
    s->listener=-1; s->running=false; publish(s);
}
static void read_header(ph_http_server *s, unsigned i)
{
    ph_http_slot *c=&s->slots[i];
    int n=s->io->recv(c->fd,c->input,sizeof(c->input));
    s->diagnostics.last_read_rc=n;
    if(n<=0) {
        if(n<0 && s->io->retry()) { s->io->yield(); return; }
        close_slot(s,i,n==0?PH_HTTP_CLOSED_PEER:PH_HTTP_CLOSED_IO,false);return;
    }
    c->in_used=(uint16_t)n; c->in_pos=0;
    while(c->in_pos<c->in_used) {
        int r=ph_http_parser_feed(&c->parser,c->input[c->in_pos++]);
        if(r<0) { simple_reply(s,i,false);return; }
        if(r>0) { ready(s,i);return; }
    }
    c->in_used=c->in_pos=0;
}
static void read_body(ph_http_server *s, unsigned i)
{
    ph_http_slot *c=&s->slots[i]; const void *data; int n;
    if(c->in_pos<c->in_used) { data=c->input+c->in_pos; n=c->in_used-c->in_pos; c->in_pos=c->in_used; }
    else {
        size_t want=c->remaining<sizeof(s->scratch)?c->remaining:sizeof(s->scratch);
        n=s->io->recv(c->fd,s->scratch,want); data=s->scratch;
        s->diagnostics.last_read_rc=n;
        if(n<=0) {
            if(n<0 && s->io->retry()) { s->io->yield();return; }
            close_slot(s,i,n==0?PH_HTTP_CLOSED_PEER:PH_HTTP_CLOSED_IO,false);return;
        }
    }
    if((uint32_t)n>c->remaining)n=(int)c->remaining;
    c->remaining-=(uint32_t)n;
    if(!s->app->body(&s->exchange,data,(size_t)n)) {
        respond(s,i);return;
    }
    c->idle_deadline=s->io->now()+s->exchange.body_idle_ms;
    if(!c->remaining) { s->app->complete(&s->exchange);respond(s,i); }
}
static void send_part(ph_http_server *s, unsigned i)
{
    ph_http_slot *c=&s->slots[i]; const char *data; size_t length;
    if(c->simple) { data=c->simple+c->header_sent; length=strlen(c->simple)-c->header_sent; }
    else if(c->header_sent<s->response_header_length) {
        data=s->response_header+c->header_sent; length=s->response_header_length-c->header_sent;
    } else {
        const ph_http_response *r=&s->exchange.response;
        length=(r->suppress_body || r->status==304) ? 0 : r->length-c->body_sent;
        data=length?(const char *)r->body+c->body_sent:NULL;
    }
    if(length) {
        if(length>1024)length=1024;
        int n=s->io->send(c->fd,data,length);
        if(n<0 && s->io->retry()) { s->io->yield();return; }
        if(n<=0) { close_slot(s,i,PH_HTTP_CLOSED_IO,false);return; }
        if(c->simple && c->reason==PH_HTTP_CLOSED_BUSY)s->diagnostics.busy_bytes+=(unsigned)n;
        if(c->simple || c->header_sent<s->response_header_length)c->header_sent+=(size_t)n;
        else c->body_sent+=(size_t)n;
        return;
    }
    /* Bound unread uploads at close; never reinterpret them as requests. */
    if(c->remaining) {
        if(c->remaining>8192)c->remaining=8192;
        unsigned carried=c->in_used-c->in_pos;
        c->remaining=carried<c->remaining?c->remaining-carried:0;
        c->in_used=c->in_pos=0;
    }
    if(c->remaining) { c->state=PH_SLOT_DRAIN;c->deadline=s->io->now()+100u; }
    else close_slot(s,i,c->reason,true);
}
void ph_http_server_step(ph_http_server *s)
{
    if(!s->running)return;
    uint32_t now=s->io->now();
    unsigned wanted=s->app->admission?s->app->admission():s->cap;
    if(!wanted || wanted>PH_HTTP_CLIENT_SLOTS || (wanted!=s->cap && s->io->limit(s->listener,wanted,&s->diagnostics)<0)) {
        ph_http_server_close(s);return;
    }
    s->cap=wanted;
    for(unsigned i=0;i<PH_HTTP_CLIENT_SLOTS;++i) {
        ph_http_slot *c=&s->slots[i];
        if(c->state==PH_SLOT_FREE)continue;
        if(expired(now,c->deadline)) {
            if(c->state==PH_SLOT_READY)simple_reply(s,i,true);
            else if(c->state==PH_SLOT_DRAIN)close_slot(s,i,c->reason,true);
            else if(c->state==PH_SLOT_WAIT) {
                s->exchange.response.status=503; respond(s,i);
            } else close_slot(s,i,PH_HTTP_CLOSED_TIMEOUT,false);
        } else if(c->state==PH_SLOT_BODY && expired(now,c->idle_deadline))close_slot(s,i,PH_HTTP_CLOSED_TIMEOUT,false);
        else if(c->state==PH_SLOT_WAIT && expired(now,c->idle_deadline)) {
            ph_http_action next=s->app->retry(&s->exchange);
            if(next!=PH_HTTP_WAIT)action(s,i,next);
            else c->idle_deadline=s->io->now()+25u;
        }
    }
    if(!s->running)return;
    if(s->active<0 && s->queued>=0)acquire(s,(unsigned)s->queued);
    for(unsigned i=0;i<PH_HTTP_CLIENT_SLOTS;++i) {
        ph_http_slot *c=&s->slots[i];
        if(c->state==PH_SLOT_BODY && c->in_pos<c->in_used)read_body(s,i);
    }
    if(!s->running)return;
    uint64_t reads=0,writes=0,rr=0,ww=0; unsigned occupied=0; int free_slot=-1;
    for(unsigned i=0;i<PH_HTTP_CLIENT_SLOTS;++i) {
        ph_http_slot *c=&s->slots[i];
        if(c->state==PH_SLOT_FREE) { if(free_slot<0)free_slot=(int)i;continue; }
        ++occupied;
        if(c->state==PH_SLOT_HEADER || c->state==PH_SLOT_DRAIN || c->state==PH_SLOT_BODY)reads|=UINT64_C(1)<<c->fd;
        if(c->state==PH_SLOT_SEND)writes|=UINT64_C(1)<<c->fd;

    }
    if(free_slot>=0 && occupied<s->cap)reads|=UINT64_C(1)<<s->listener;
    int result=s->io->wait(reads,writes,&rr,&ww,25u);
    s->diagnostics.last_select_rc=result;
    if(result<0) {
        s->diagnostics.last_errno=s->io->error();
        if(s->io->retry())s->io->yield();else ph_http_server_close(s);
        publish(s);return;
    }
    if((rr & (UINT64_C(1)<<s->listener)) && free_slot>=0) {
        int fd=s->io->accept(s->listener);
        if(fd>=0) {
            s->diagnostics.last_fd=fd;count(&s->diagnostics.accepted);
            if(fd>=PH_HTTP_FD_LIMIT || s->io->nonblock(fd)<0) { count(&s->diagnostics.nonblock_failures);close_fd(s,fd); }
            else {
                ph_http_slot *c=&s->slots[free_slot]; memset(c,0,sizeof(*c));
                c->fd=fd;c->state=PH_SLOT_HEADER;c->deadline=s->io->now()+PH_HTTP_HEADER_MS;
                ph_http_parser_init(&c->parser);
            }
        } else if(s->io->retry())s->io->yield();
        else { s->diagnostics.last_errno=s->io->error();count(&s->diagnostics.accept_failures); }
    }
    for(unsigned n=0;n<PH_HTTP_CLIENT_SLOTS;++n) {
        unsigned i=(s->turn+n)%PH_HTTP_CLIENT_SLOTS; ph_http_slot *c=&s->slots[i];
        if(!s->running)break;
        if(c->state==PH_SLOT_FREE)continue;
        uint64_t bit=UINT64_C(1)<<c->fd;
        if((rr&bit) && c->state==PH_SLOT_HEADER)read_header(s,i);
        else if((rr&bit) && c->state==PH_SLOT_BODY)read_body(s,i);
        else if((ww&bit) && c->state==PH_SLOT_SEND)send_part(s,i);
        else if((rr&bit) && c->state==PH_SLOT_DRAIN) {
            size_t want=c->remaining<sizeof(s->scratch)?c->remaining:sizeof(s->scratch);
            int got=s->io->recv(c->fd,s->scratch,want);
            if(got>0) { s->diagnostics.drained+=(unsigned)got;c->remaining-=(unsigned)got;if(!c->remaining)close_slot(s,i,c->reason,true); }
            else if(got==0 || !s->io->retry())close_slot(s,i,c->reason,true);
        }
    }
    s->turn=(s->turn+1u)%PH_HTTP_CLIENT_SLOTS; publish(s);
}
