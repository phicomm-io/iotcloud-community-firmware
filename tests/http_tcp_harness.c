/* Linux substrate for extracted production HTTP C. Not a MiCO TCP emulator.
 * Socket I/O is real. Only RTOS queues/threads, listener address/backlog and
 * MiCO-only TCP options are adapted. See test_http_tcp_integration.py. */
#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <time.h>
#include <unistd.h>

typedef int OSStatus;
typedef uintptr_t mico_thread_arg_t;
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    int fds[8];
    unsigned head, count;
} mico_queue_t;
#define kNoErr 0
#define MICO_WAIT_FOREVER UINT32_MAX
#define MICO_APPLICATION_PRIORITY 7
#define TCP_MAX_CONN_NUM 10001
#define TCP_CONN_NUM 10002
#define UNUSED_PARAMETER(x) (void)(x)
#define custom_log(...) ((void)0)
#define APP_APP_OFFSET 0u
#define APP_APP_LIMIT (1024u * 1024u)
#define OTA_ENVELOPE_SIZE 512u
static pthread_mutex_t app_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ledger_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t hold_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_ready = PTHREAD_COND_INITIALIZER;
static bool released;
static __thread int role; /* listener/accept=1; request=2 */
static pthread_t accept_thread, request_thread;
static _Atomic bool request_created, stopping;
static _Atomic bool ota_active;
static bool ota_job_active(void) { return ota_active; }
static int listener_fd, close_policy, host_send_buffer;
static _Atomic int tcp_limit;
static unsigned open_clients, accepted, closed, unread_closes, close_errors;
static unsigned nonblocking_closes, send_failures, handled, active, queue_peak;
static unsigned accepted_requests, rejected_requests, body_allocations;
static unsigned queued_output_closes, maximum_queued_output;
static int owners[FD_SETSIZE];
static uint32_t mico_rtos_get_time(void) {
    struct timespec t; assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint32_t)((uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
static void mico_thread_msleep(unsigned ms) { usleep(ms * 1000); }
static void mico_rtos_lock_mutex(pthread_mutex_t *p) { assert(!pthread_mutex_lock(p)); }
static void mico_rtos_unlock_mutex(pthread_mutex_t *p) { assert(!pthread_mutex_unlock(p)); }
static void mico_rtos_delete_thread(void *p) { assert(!p); pthread_exit(NULL); }
struct entry { void (*fn)(mico_thread_arg_t); mico_thread_arg_t arg; int role; };
static void *launch(void *p) {
    struct entry e = *(struct entry *)p; free(p); role = e.role; e.fn(e.arg); return NULL;
}
static int mico_rtos_create_thread(void *p, int priority, const char *name,
        void (*fn)(mico_thread_arg_t), unsigned stack, mico_thread_arg_t arg) {
    (void)name; (void)stack; assert(!p && priority == 7);
    struct entry *e = malloc(sizeof(*e)); assert(e);
    *e = (struct entry){fn, arg, role == 0 ? 1 : 2};
    pthread_t *thread = role == 0 ? &accept_thread : &request_thread;
    int rc = pthread_create(thread, NULL, launch, e);
    if (rc) free(e); else if (role == 1) request_created = true;
    return rc;
}
static int mico_rtos_init_queue(mico_queue_t *q, const char *name, unsigned size, unsigned count) {
    (void)name; assert(role == 1 && size == sizeof(int) && count == 8);
    memset(q, 0, sizeof(*q)); assert(!pthread_mutex_init(&q->lock, NULL));
    assert(!pthread_cond_init(&q->ready, NULL)); return 0;
}
static void mico_rtos_deinit_queue(mico_queue_t *q) {
    pthread_cond_destroy(&q->ready); pthread_mutex_destroy(&q->lock);
}
static int mico_rtos_push_to_queue(mico_queue_t *q, void *value, uint32_t timeout) {
    assert(role == 1 && timeout == 0); pthread_mutex_lock(&q->lock);
    if (q->count == 8) { pthread_mutex_unlock(&q->lock); return -1; }
    int fd = *(int *)value;
    pthread_mutex_lock(&ledger_lock); assert(owners[fd] == 1); owners[fd] = 2;
    accepted_requests++; if (q->count + 1 > queue_peak) queue_peak = q->count + 1;
    pthread_mutex_unlock(&ledger_lock);
    q->fds[(q->head + q->count++) % 8] = fd;
    pthread_cond_signal(&q->ready); pthread_mutex_unlock(&q->lock); return 0;
}
static void unlock_cleanup(void *p) { pthread_mutex_unlock(p); }
static int mico_rtos_pop_from_queue(mico_queue_t *q, void *value, uint32_t timeout) {
    assert(role == 2 && timeout == MICO_WAIT_FOREVER);
    pthread_mutex_lock(&q->lock);
    pthread_cleanup_push(unlock_cleanup, &q->lock);
    while (!q->count && !stopping) pthread_cond_wait(&q->ready, &q->lock);
    if (stopping) pthread_exit(NULL);
    *(int *)value = q->fds[q->head]; q->head = (q->head + 1) % 8; q->count--;
    pthread_cleanup_pop(1);
    return 0;
}
static void check_client(int fd) {
    pthread_mutex_lock(&ledger_lock); assert(fd >= 0 && fd < FD_SETSIZE);
    assert(owners[fd] == role); pthread_mutex_unlock(&ledger_lock);
}
static int host_socket(int domain, int type, int protocol) {
    assert(role == 1); listener_fd = socket(domain, type, protocol); assert(listener_fd >= 0); return listener_fd;
}
static int host_bind(int fd, const struct sockaddr *address, socklen_t length) {
    assert(role == 1 && fd == listener_fd);
    struct sockaddr_in local = *(const struct sockaddr_in *)address;
    local.sin_port = 0; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return bind(fd, (struct sockaddr *)&local, length);
}
static int host_listen(int fd, int backlog) {
    assert(role == 1 && fd == listener_fd && backlog == 0);
    /* MiCO ignores backlog; Linux does not. Use a nonrestrictive host backlog. */
    return listen(fd, 64);
}
static int host_setsockopt(int fd, int level, int option, const void *value, socklen_t length) {
    if (option == TCP_NODELAY) {
        check_client(fd); assert(role == 2 && level == IPPROTO_TCP);
        return setsockopt(fd, level, option, value, length);
    }
    assert(role == 1 && fd == listener_fd && level == IPPROTO_TCP);
    assert(option == TCP_MAX_CONN_NUM && length == sizeof(int)); tcp_limit = *(const int *)value; return 0;
}
static int host_getsockopt(int fd, int level, int option, void *value, socklen_t *length) {
    assert(role == 1 && fd == listener_fd && level == IPPROTO_TCP && *length == sizeof(int));
    assert(option == TCP_MAX_CONN_NUM || option == TCP_CONN_NUM);
    pthread_mutex_lock(&ledger_lock); *(int *)value = option == TCP_MAX_CONN_NUM ? tcp_limit : (int)open_clients;
    pthread_mutex_unlock(&ledger_lock); return 0;
}
static int host_accept(int fd, struct sockaddr *address, socklen_t *length) {
    assert(role == 1 && fd == listener_fd);
    int client = accept(fd, address, length);
    if (stopping) {
        if (client >= 0) close(client);
        close(fd); pthread_exit(NULL);
    }
    if (client >= 0) {
        assert(client < FD_SETSIZE); int small = host_send_buffer;
        assert(!setsockopt(client, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)));
        pthread_mutex_lock(&ledger_lock); assert(!owners[client]); owners[client] = 1;
        open_clients++; accepted++; pthread_mutex_unlock(&ledger_lock);
    }
    return client;
}
static int host_close(int fd) {
    if (fd == listener_fd) { assert(role == 1); return close(fd); }
    check_client(fd);
    if (close_policy == 1) assert(!fcntl(fd, F_SETFL, 0)); /* experimental control only */
    if (close_policy == 2) { /* experimental unread-byte control only */
        char discard[4096]; while (recv(fd, discard, sizeof(discard), MSG_DONTWAIT) > 0) {}
    }
    int unread = 0; assert(!ioctl(fd, FIONREAD, &unread));
    int queued = 0; assert(!ioctl(fd, TIOCOUTQ, &queued));
    int flags = fcntl(fd, F_GETFL); assert(flags >= 0);
    pthread_mutex_lock(&ledger_lock);
    unread_closes += unread > 0; nonblocking_closes += !!(flags & O_NONBLOCK);
    queued_output_closes += queued > 0;
    if ((unsigned)queued > maximum_queued_output) maximum_queued_output = queued;
    rejected_requests += role == 1;
    int rc = close(fd); close_errors += rc != 0; closed++; open_clients--; owners[fd] = 0;
    pthread_mutex_unlock(&ledger_lock); return rc;
}
static int host_recv(int fd, void *data, size_t length, int flags) {
    check_client(fd); return recv(fd, data, length, flags);
}
static int host_send(int fd, const void *data, size_t length, int flags) {
    check_client(fd); return send(fd, data, length, flags);
}
static int host_fcntl(int fd, int command, int flags) {
    check_client(fd); return fcntl(fd, command, flags);
}
#define socket host_socket
#define bind host_bind
#define listen host_listen
#define setsockopt host_setsockopt
#define getsockopt host_getsockopt
#define accept host_accept
#define close host_close
#define recv host_recv
#define send host_send
#define fcntl host_fcntl
/*PRODUCTION_TRANSPORT*/

/* Test route adapter: parsing, response serialization and workers are production
 * C; product service/JSON, Wi-Fi and SDK allocator are not compiled here. */
static void handle(int fd) {
    request_t req; bool ok = read_request(fd, &req);
    pthread_mutex_lock(&ledger_lock); handled++; active++; pthread_mutex_unlock(&ledger_lock);
    if (ok && !strcmp(req.path, "/hold")) {
        pthread_mutex_lock(&hold_lock);
        while (!released) pthread_cond_wait(&hold_ready, &hold_lock);
        pthread_mutex_unlock(&hold_lock);
    }
    if (ok && !strcmp(req.path, "/read-body")) {
        char buffer[1024]; unsigned left = req.length;
        while (left) {
            int n = app_socket_read(fd, buffer, left < sizeof(buffer) ? left : sizeof(buffer), 1000);
            if (n <= 0) { ok = false; break; } left -= n;
        }
    }
    size_t size = ok && !strncmp(req.path, "/bytes/", 7) ? strtoul(req.path + 7, NULL, 10) : 32;
    assert(size <= 16u * 1024 * 1024); char *body = malloc(size); assert(body); memset(body, 'x', size);
    pthread_mutex_lock(&ledger_lock); body_allocations++; pthread_mutex_unlock(&ledger_lock);
    bool sent = response(fd, ok ? 200 : 400, "text/plain", body, size, false); free(body);
    pthread_mutex_lock(&ledger_lock); body_allocations--; send_failures += !sent; active--;
    pthread_mutex_unlock(&ledger_lock);
}
/*PRODUCTION_WORKERS*/
#undef socket
#undef bind
#undef listen
#undef setsockopt
#undef getsockopt
#undef accept
#undef close
#undef recv
#undef send
#undef fcntl

static void report(void) {
    pthread_mutex_lock(&ledger_lock);
    printf("{\"accepted\":%u,\"closed\":%u,\"open_clients\":%u,\"unread_closes\":%u,"
           "\"nonblocking_closes\":%u,\"close_errors\":%u,\"send_failures\":%u,"
           "\"handled\":%u,\"active\":%u,\"queue_peak\":%u,\"enqueued\":%u,"
           "\"rejected\":%u,\"body_allocations\":%u,\"queued_output_closes\":%u,"
           "\"maximum_queued_output\":%u,\"tcp_limit\":%d}\n", accepted, closed, open_clients,
           unread_closes, nonblocking_closes, close_errors, send_failures, handled, active,
           queue_peak, accepted_requests, rejected_requests, body_allocations,
           queued_output_closes, maximum_queued_output, (int)tcp_limit);
    pthread_mutex_unlock(&ledger_lock); fflush(stdout);
}
int main(int argc, char **argv) {
    assert(argc == 3); close_policy = atoi(argv[1]); host_send_buffer = atoi(argv[2]);
    assert(host_send_buffer > 0); signal(SIGPIPE, SIG_IGN);
    assert(app_http_start() == 0);
    while (!request_created) usleep(1000);
    struct sockaddr_in address; socklen_t length = sizeof(address);
    assert(!getsockname(listener_fd, (struct sockaddr *)&address, &length));
    printf("{\"port\":%u}\n", ntohs(address.sin_port)); fflush(stdout);
    char command[32];
    while (fgets(command, sizeof(command), stdin)) {
        if (!strcmp(command, "RELEASE\n")) {
            pthread_mutex_lock(&hold_lock); released = true; pthread_cond_broadcast(&hold_ready);
            pthread_mutex_unlock(&hold_lock); puts("{\"released\":true}"); fflush(stdout);
        } else if (!strcmp(command, "REPORT\n")) report();
        else if (!strcmp(command, "OTA ON\n")) { ota_active = true; puts("{\"ota_active\":true}"); fflush(stdout); }
        else if (!strcmp(command, "OTA OFF\n")) { ota_active = false; puts("{\"ota_active\":false}"); fflush(stdout); }
        else if (!strcmp(command, "STOP\n")) break;
        else assert(0);
    }
    report(); assert(!open_clients && !body_allocations);
    stopping = true;
    pthread_mutex_lock(&http_pending.lock); pthread_cond_signal(&http_pending.ready);
    pthread_mutex_unlock(&http_pending.lock);
    int wake = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); assert(wake >= 0);
    assert(!connect(wake, (struct sockaddr *)&address, length)); close(wake);
    pthread_join(accept_thread, NULL); pthread_join(request_thread, NULL);
    mico_rtos_deinit_queue(&http_pending);
    return 0;
}
