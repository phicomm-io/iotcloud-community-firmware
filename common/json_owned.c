/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "json_owned.h"
#include "json_object_private.h"
#include "json_heap.h"
#include "linkhash.h"
#include <limits.h>
/* This adapter is pinned to the patched MiCO json-c layout. The caller passes one
 * owned reference. Retire encoded members before growing the output buffer,
 * instead of retaining the entire status graph alongside the complete wire.
 * A shared ancestor makes its whole subtree read-only, including unique leaves.
 * SDK primitive callbacks preserve its exact escaping and number formatting. */
#define OWNED_JSON_DEPTH 32
/* Bound spare capacity of the final wire. SDK doubling remains local to one
 * reusable primitive buffer, not the multi-kilobyte response allocation. */
static bool json_append(struct printbuf *pb,const char *s,int n,enum json_owned_error *error)
{
    if(n<0 || pb->bpos>INT_MAX-n-1) { *error=JSON_OWNED_INVALID;return false; }
    int needed=pb->bpos+n+1;
    if(needed>pb->size) {
        int size=pb->size<=INT_MAX-256?pb->size+256:INT_MAX;
        if(size<needed)size=needed;
        char *next=json_heap_resize(pb->buf,(size_t)pb->size,(size_t)size);
        if(!next){pb->failed=1;return false;}
        pb->buf=next;pb->size=size;
    }
    return printbuf_memappend(pb,s,n)>=0 && !pb->failed;
}
static bool json_primitive(struct printbuf *pb,struct printbuf *piece,json_object *o,enum json_owned_error *error)
{
    printbuf_reset(piece);
    int rc=o->_to_json_string(o,piece);
    if(rc<0 && !piece->failed) { *error=JSON_OWNED_INVALID;return false; }
    return rc>=0 && !piece->failed && json_append(pb,piece->buf,piece->bpos,error);
}
/* One bounded automatic stack; no recursion or persistent server RAM. An
 * entry remains live until its child has finished and its parent resumes. */
struct json_frame {
    json_object *object;
    union { struct lh_entry *entry; int index; } next;
    bool consume, pending, first;
};
static bool json_write(struct printbuf *pb,struct printbuf *piece,json_object *o,enum json_owned_error *error)
{
    struct json_frame frames[OWNED_JSON_DEPTH];
    unsigned count=0;
    bool consume=true;
    for(;;) {
        if(!o) { if(!json_append(pb,"null",4,error))return false; }
        else {
            if(count>=OWNED_JSON_DEPTH || !o->_to_json_string) { *error=JSON_OWNED_INVALID;return false; }
            if(o->_failed) { *error=JSON_OWNED_GRAPH;return false; }
            consume=consume && o->_ref_count==1;
            if(o->o_type==json_type_object || o->o_type==json_type_array) {
                struct json_frame *f=&frames[count++];
                f->object=o;f->consume=consume;f->pending=false;f->first=true;
                if(o->o_type==json_type_object) {
                    f->next.entry=json_object_get_object(o)->head;
                    if(!json_append(pb,"{",1,error))return false;
                } else {
                    f->next.index=0;
                    if(!json_append(pb,"[",1,error))return false;
                }
                goto resume;
            }
            if(!json_primitive(pb,piece,o,error))return false;
        }
resume:
        if(!count)return true;
        struct json_frame *f=&frames[count-1];
        bool object=f->object->o_type==json_type_object;
        if(f->pending) {
            if(object) {
                struct lh_entry *entry=f->next.entry;
                if(f->consume) {
                    json_object_object_del(f->object,entry->k);
                    f->next.entry=json_object_get_object(f->object)->head;
                } else f->next.entry=entry->next;
            } else {
                if(f->consume)json_object_array_put_idx(f->object,f->next.index,NULL);
                f->next.index++;
            }
            f->pending=false;f->first=false;
        }
        bool more=object?f->next.entry!=NULL:
            f->next.index<json_object_array_length(f->object);
        if(!more) {
            if(!json_append(pb,object?" }":" ]",2,error))return false;
            count--;
            goto resume;
        }
        if(!json_append(pb,f->first?" ":", ",f->first?1:2,error))return false;
        if(object) {
            json_object *quoted=json_object_new_string(f->next.entry->k);
            bool ok=quoted && json_primitive(pb,piece,quoted,error) && json_append(pb,": ",2,error);
            json_object_put(quoted);
            if(!ok)return false;
            o=(json_object *)f->next.entry->v;
        } else o=json_object_array_get_idx(f->object,f->next.index);
        consume=f->consume;f->pending=true;
    }
}

struct printbuf *json_owned_to_wire_result(json_object *o,enum json_owned_error *error)
{
    *error=o?JSON_OWNED_RESOURCE:JSON_OWNED_GRAPH;
    struct printbuf *pb=o?printbuf_new():NULL;
    struct printbuf *piece=pb?printbuf_new():NULL;
    if(pb && (!piece || !json_write(pb,piece,o,error))) { printbuf_free(pb);pb=NULL; }
    printbuf_free(piece);
    json_object_put(o);
    if(pb)*error=JSON_OWNED_OK;
    return pb;
}

struct printbuf *json_owned_to_wire(json_object *o)
{
    enum json_owned_error error;
    return json_owned_to_wire_result(o,&error);
}

int json_owned_publish(int (*publish)(const char *,const char *,bool),
                       const char *topic,json_object *o,bool retain)
{
    struct printbuf *wire=json_owned_to_wire(o);
    if(!wire)return -1;
    int rc=publish(topic,wire->buf,retain);
    printbuf_free(wire);
    return rc;
}
