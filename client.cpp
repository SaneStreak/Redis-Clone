#include <iostream>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static int write_all(int fd, const char *buf, size_t n) {
    size_t offset = 0;
    while (offset < n) {
        ssize_t rv = write(fd, buf + offset, n - offset);
        if (rv <= 0) return -1;
        offset += (size_t)rv;
    }
    return 0;
}

static void pack_command(std::vector<uint8_t> &payload, const std::vector<std::string> &cmd) {
    uint32_t body_len = 4;
    for (const auto &s : cmd) {
        body_len += 4 + s.size();
    }

    size_t old_size = payload.size();
    payload.resize(old_size + 4 + body_len);

    uint8_t *cur = payload.data() + old_size;

    memcpy(cur, &body_len, 4);
    cur += 4;

    uint32_t nstr = cmd.size();
    memcpy(cur, &nstr, 4);
    cur += 4;

    for (const auto &s : cmd) {
        uint32_t len = s.size();
        memcpy(cur, &len, 4);
        cur += 4;
        memcpy(cur, s.data(), len);
        cur += len;
    }
}

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket()");

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        die("connect()");
    }

    std::vector<uint8_t> pipeline_payload;
    pack_command(pipeline_payload, {"set", "k1", "val_alpha"});
    pack_command(pipeline_payload, {"get", "k1"});
    pack_command(pipeline_payload, {"del", "k1"});
    pack_command(pipeline_payload, {"get", "k1"});
    pack_command(pipeline_payload, {"zadd", "myzset", "100.5", "user1"});

    std::cout << "[Client] Transmitting 5 pipelined commands to server..." << std::endl;
    if (write_all(fd, (const char *)pipeline_payload.data(), pipeline_payload.size()) < 0) {
        die("write pipeline failed");
    }

    char buf[1024];
    ssize_t rv = read(fd, buf, sizeof(buf) - 1);
    if (rv < 0) die("read failed");
    buf[rv] = '\0';

    std::cout << "\n[Server RESP Responses]:\n" << buf << std::endl;

    close(fd);
    return 0;
}