#include <stdlib.h>
#ifdef __NEWLIB__
#include <string.h>
#include "context.h"
#include "json_heap.h"
/* The union forces payload alignment suitable for binary64 on ARM as well as
 * host builds; the ledger link never aliases the Bigint freelist/cache link. */
struct json_num_allocation {
    union { struct json_num_allocation *next; double alignment; } head;
};
void json_num_resource_error(struct json_num_context *c)
{
    if (!c->failed) json_heap_reject(4);
    c->failed = 1;
    c->_errno = ENOMEM;
}
void *json_num_calloc(struct json_num_context *c, size_t count, size_t size)
{
    struct json_num_allocation *a;
    size_t bytes;
    if (c->failed) return NULL;
    if (size && count > SIZE_MAX / size) goto rejected;
    bytes = count * size;
    if (bytes > SIZE_MAX - sizeof(*a)) goto rejected;
    bytes += sizeof(*a);
    if (c->blocks >= JSON_NUMERIC_BLOCK_LIMIT ||
        bytes > JSON_NUMERIC_SCRATCH_LIMIT - c->bytes) goto rejected;
    a = json_heap_calloc(1, bytes);
    if (!a) {
        /* json_heap_calloc already notified the existing allocation scope. */
        c->failed = 1; c->_errno = ENOMEM; return NULL;
    }
    a->head.next = c->allocations;
    c->allocations = a;
    c->bytes += bytes;
    ++c->blocks;
    return a + 1;
rejected:
    json_num_resource_error(c);
    return NULL;
}
int json_num_init_mp(struct json_num_context *c)
{
    if (!c->_mp) c->_mp = json_num_calloc(c, 1, sizeof(*c->_mp));
    return c->_mp != NULL;
}
void json_num_cleanup(struct json_num_context *c)
{
    struct json_num_allocation *a = c->allocations;
    while (a) {
        struct json_num_allocation *next = a->head.next;
        json_heap_free(a);
        a = next;
    }
    /* Idempotent: no native reclaim may see these headered interior pointers. */
    c->allocations = NULL; c->_mp = NULL; c->bytes = 0; c->blocks = 0;
}

#endif /* __NEWLIB__ */
