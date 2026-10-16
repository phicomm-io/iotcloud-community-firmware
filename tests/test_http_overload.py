import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
START = "        http_accept_count(&http_accept_queue_reject);"
END = "        close(fd);"


def overload_branch(product):
    source = (ROOT / product / "http.c").read_text()
    start = source.index(START)
    end = source.index(END, start) + len(END)
    return source[start:end]


HARNESS = r'''#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef unsigned int socklen_t;
typedef struct { unsigned long bits; } fd_set;
struct timeval { long tv_sec; long tv_usec; };
#define FD_ZERO(p) ((p)->bits = 0)
#define FD_SET(fd, p) ((p)->bits |= (1ul << (fd)))
#define F_SETFL 4
#define O_NONBLOCK 1

static const char busy[] =
    "HTTP/1.1 503 Service Unavailable\r\n"
    "Content-Type: application/json\r\nContent-Length: 15\r\n"
    "Connection: close\r\n\r\n{\"code\":\"busy\"}";
static uint32_t fake_now;
static uint32_t select_advance;
static int timeout_on_select_call;
static int send_plan[8];
static unsigned send_plan_count, send_plan_at, send_advance;
static int recv_mode, recv_calls, select_calls, close_calls;
static unsigned timeouts_ms[128], timeout_count;
static char sent_data[256];
static size_t sent_length;
static unsigned http_accept_queue_reject, http_accept_busy_send_ok;
static unsigned http_accept_busy_send_fail, http_accept_busy_drained;
static unsigned http_accept_busy_fcntl_fail, http_accept_overflow_close;
static int http_accept_busy_send_errno, http_accept_busy_send_bytes;

static void http_accept_count(unsigned *counter) { ++*counter; }
static uint32_t mico_rtos_get_time(void) { return fake_now; }
static int fcntl(int fd, int cmd, int flags)
{
    assert(fd == 7 && cmd == F_SETFL && flags == O_NONBLOCK);
    return 0;
}
static int select(int nfds, fd_set *readfds, fd_set *writefds,
                  fd_set *exceptfds, struct timeval *timeout)
{
    (void)readfds; (void)writefds; (void)exceptfds;
    assert(nfds == 8 && timeout_count < 128);
    if (writefds) assert(writefds->bits == (1ul << 7));
    else assert(readfds && readfds->bits == (1ul << 7));
    unsigned ms = (unsigned)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
    timeouts_ms[timeout_count++] = ms;
    ++select_calls;
    fake_now += select_advance;
    if (select_calls == timeout_on_select_call) {
        fake_now += ms;
        return 0;
    }
    return 1;
}
static int send(int fd, const void *data, size_t length, int flags)
{
    (void)flags;
    assert(fd == 7 && sent_length + length <= sizeof(sent_data));
    int result = send_plan_at < send_plan_count ? send_plan[send_plan_at++] : (int)length;
    if (result < 0) { errno = EPIPE; return result; }
    if (result == 0) return result;
    if ((size_t)result > length) result = (int)length;
    memcpy(sent_data + sent_length, data, (size_t)result);
    sent_length += (size_t)result;
    fake_now += send_advance;
    return result;
}
static int recv(int fd, void *data, size_t length, int flags)
{
    (void)flags;
    assert(fd == 7 && length == 128);
    ++recv_calls;
    if (recv_mode == 0) return 0;
    if (recv_mode == 1) { errno = EIO; return -1; }
    memset(data, 'x', length);
    return (int)length;
}
static int close(int fd)
{
    assert(fd == 7);
    ++close_calls;
    return 0;
}

static void run_reject(int fd)
{
BRANCH
}

static void reset_case(uint32_t start)
{
    fake_now = start; select_advance = 0; timeout_on_select_call = 0;
    memset(send_plan, 0, sizeof(send_plan));
    send_plan_count = send_plan_at = send_advance = 0;
    recv_mode = recv_calls = select_calls = close_calls = 0;
    timeout_count = 0; sent_length = 0; memset(sent_data, 0, sizeof(sent_data));
    http_accept_queue_reject = http_accept_busy_send_ok = 0;
    http_accept_busy_send_fail = http_accept_busy_drained = 0;
    http_accept_busy_fcntl_fail = http_accept_overflow_close = 0;
    http_accept_busy_send_errno = http_accept_busy_send_bytes = 0;
}

static void test_partial_send_then_eof(void)
{
    reset_case(1000);
    send_plan[0] = 17; send_plan[1] = 31; send_plan_count = 2;
    run_reject(7);
    assert(http_accept_busy_send_ok == 1 && http_accept_busy_send_fail == 0);
    assert(sizeof(busy) - 1 == 122);
    assert(http_accept_busy_send_bytes == (int)(sizeof(busy) - 1));
    assert(sent_length == sizeof(busy) - 1 && memcmp(sent_data, busy, sent_length) == 0);
    assert(recv_calls == 1 && http_accept_busy_drained == 0);
    assert(close_calls == 1 && http_accept_overflow_close == 1);
}

static void test_send_error(void)
{
    reset_case(1000); send_plan[0] = -1; send_plan_count = 1; recv_mode = 0;
    run_reject(7);
    assert(http_accept_busy_send_ok == 0 && http_accept_busy_send_fail == 1);
    assert(http_accept_busy_send_bytes == 0 && http_accept_busy_send_errno == EPIPE);
    assert(recv_calls == 1 && close_calls == 1);
}

static void test_send_timeout(void)
{
    reset_case(1000); timeout_on_select_call = 1;
    run_reject(7);
    assert(http_accept_busy_send_fail == 1 && http_accept_busy_send_ok == 0);
    assert(http_accept_busy_send_bytes == 0 && sent_length == 0);
    assert(select_calls == 1 && timeouts_ms[0] == 100);
    assert(recv_calls == 0 && close_calls == 1);
}

static void test_drain_limit(void)
{
    reset_case(1000); recv_mode = 2;
    run_reject(7);
    assert(http_accept_busy_send_ok == 1);
    assert(recv_calls == 64 && http_accept_busy_drained == 64);
    assert(select_calls == 65 && close_calls == 1);
}

static void test_shared_deadline(void)
{
    reset_case(1000); send_advance = 70; timeout_on_select_call = 2;
    run_reject(7);
    assert(http_accept_busy_send_ok == 1 && sent_length == sizeof(busy) - 1);
    assert(select_calls == 2 && timeouts_ms[0] == 100 && timeouts_ms[1] == 30);
    assert(fake_now == 1100 && recv_calls == 0 && close_calls == 1);
}

static void test_tick_wraparound(void)
{
    reset_case(UINT32_MAX - 50u); send_advance = 20; timeout_on_select_call = 2;
    run_reject(7);
    assert(http_accept_busy_send_ok == 1);
    assert(select_calls == 2 && timeouts_ms[0] == 100 && timeouts_ms[1] == 80);
    assert(fake_now == 49u && recv_calls == 0 && close_calls == 1);
}

int main(void)
{
    test_partial_send_then_eof();
    test_send_error();
    test_send_timeout();
    test_drain_limit();
    test_shared_deadline();
    test_tick_wraparound();
    puts("http overload branch: OK");
    return 0;
}
'''


class HttpOverloadTest(unittest.TestCase):
    def test_a1_and_m1_branch_match_and_boundaries(self):
        a1_branch = overload_branch("A1")
        m1_branch = overload_branch("M1")
        self.assertEqual(a1_branch, m1_branch)
        harness = HARNESS.replace("BRANCH", a1_branch)
        with tempfile.TemporaryDirectory() as tmp:
            c_file = pathlib.Path(tmp) / "http_overload.c"
            binary = pathlib.Path(tmp) / "http_overload"
            c_file.write_text(harness)
            subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", str(c_file), "-o", str(binary)],
                check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
