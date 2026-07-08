#include <iostream>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>

enum {
    RES_OK = 0,
    RES_ERR = 1,
    RES_NX = 2
};

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

// Serialization helper to pack commands into the multi-string protocol matrix
static void pack_command(std::vector<uint8_t> &payload, const std::vector<std::string> &cmd) {
    uint32_t body_len = 4; // Start with 4 bytes for the nstr header
    for (const auto &s : cmd) {
        body_len += 4 + s.size(); // 4 bytes for string len header + string body
    }

    size_t old_size = payload.size();
    payload.resize(old_size + 4 + body_len);

    uint8_t *cur = payload.data() + old_size;

    // 1. Pack the outer transmission length header
    memcpy(cur, &body_len, 4);
    cur += 4;

    // 2. Pack the array argument count header (nstr)
    uint32_t nstr = cmd.size();
    memcpy(cur, &nstr, 4);
    cur += 4;

    // 3. Pack each individual length-prefixed string argument
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

    // Pipeline a series of valid structured queries directly into one buffer block
    std::vector<uint8_t> pipeline_payload;
    pack_command(pipeline_payload, {"set", "mykey", "hello_systems_world"});
    pack_command(pipeline_payload, {"get", "mykey"});
    pack_command(pipeline_payload, {"del", "mykey"});
    pack_command(pipeline_payload, {"get", "mykey"}); // Should return RES_NX

    std::cout << "[Client] Firing 4 structured pipelined database commands..." << std::endl;
    if (write_all(fd, (const char *)pipeline_payload.data(), pipeline_payload.size()) < 0) {
        die("write pipeline failed");
    }

    // Read back the 4 individual responses
    for (int i = 0; i < 4; ++i) {
        uint32_t reply_len = 0;
        ssize_t rv = read(fd, &reply_len, 4);
        if (rv != 4) die("read header failed");

        uint32_t status = 0;
        rv = read(fd, &status, 4);
        if (rv != 4) die("read status failed");

        uint32_t data_size = reply_len - 4; // Subtract status header bytes
        std::vector<char> reply_body(data_size + 1, 0);
        
        if (data_size > 0) {
            rv = read(fd, reply_body.data(), data_size);
            if (rv != (ssize_t)data_size) die("read payload body failed");
        }
        
        std::cout << "[Client] Response " << i + 1 << " -> Status: " << status 
                  << " | Data: " << (data_size > 0 ? reply_body.data() : "(empty)") << std::endl;
    }

    close(fd);
    return 0;
}