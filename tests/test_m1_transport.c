#include "MQTTClient.h"
#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>

int m1_mqtt_read(Network *, unsigned char *, int, int);
extern uint64_t host_now(void);
int main(void)
{
    int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    Network network = {0}; network.my_socket = pair[0];
    unsigned char bytes[4]; uint64_t start = host_now();
    assert(m1_mqtt_read(&network, bytes, 1, 100) == 0);
    assert(host_now() - start >= 80 && host_now() - start < 1000);
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        close(pair[0]);
        assert(write(pair[1], "ab", 2) == 2); usleep(50000);
        assert(write(pair[1], "cd", 2) == 2); close(pair[1]); _exit(0);
    }
    close(pair[1]);
    assert(m1_mqtt_read(&network, bytes, 4, 500) == 4);
    assert(!memcmp(bytes, "abcd", 4));
    assert(m1_mqtt_read(&network, bytes, 1, 100) == -1);
    int status; waitpid(child, &status, 0); assert(status == 0); close(pair[0]);
    puts("M1 MQTT idle timeout, fragmented receive and peer close passed");
}
