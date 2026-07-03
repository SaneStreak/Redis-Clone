#include <iostream>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>

static void die(const char *msg){
    perror(msg);
    exit(EXIT_FAILURE);
}

//to send a length prefixed message over the socket
static int write_all(int fd, const char *buf, size_t n){
    size_t offset = 0;
    while(offset < n){
        ssize_t rv = write(fd, buf + offset, n - offset);
        if(rv <= 0) return -1;
        offset += (size_t)rv;
    }

    return 0;
}

int main(){
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) die("socket()");

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_ANY); //connects to localhost

    if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0){
        die("connect()");
    }

    const char *msg = "hello";
    uint32_t len = strlen(msg);

    //1. pack and send header (4 bytes) + body

    std::vector<uint8_t> packet;
    packet.resize(4 + len);
    memcpy(packet.data(), &len, 4);
    memcpy(packet.data() + 4, msg, len);

    if(write_all(fd, (const char *)packet.data(), packet.size()) < 0){
        die("write()");
    }

    //2. read response head (4 bytes)

    uint32_t reply_len = 0;
    ssize_t rv = read(fd, &reply_len, 4);
    if(rv != 4) die("read header failed");

    //3. read response body

    std::vector<char> reply_body(reply_len + 1);
    rv = read(fd, reply_body.data(), reply_len);
    if(rv != (ssize_t)reply_len) die("read body failed");

    reply_body[reply_len] = '\0';
    std::cout << "[Client] Server echoed: " << reply_body.data() << std::endl;

    close(fd);
    return 0;
}