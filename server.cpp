#include <iostream>
#include <vector> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <cstring>
#include <cassert>

const size_t k_max_msg = 32 << 20;

//per connection state
struct Conn {
    int fd = -1;
    bool want_read = false;
    bool want_write = false;
    bool want_close = false;
    std::vector<uint8_t> incoming;
    std::vector<uint8_t> outgoing;
};

static void die(const char *msg){
    perror(msg);
    exit(EXIT_FAILURE);
}

//utility to safely enable non blocking mode on a socket descriptor
static void fd_set_nb(int fd){
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags < 0) die("fcntl(F_GETFL) failed");

    flags |= O_NONBLOCK;

    if(fcntl(fd, F_SETFL, flags) < 0){
        die("fcntl(F_SETFL) failed");
    }
}

static void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len){
    buf.insert(buf.end(), data, data + len);
}

static void buf_consume(std::vector<uint8_t> &buf, size_t n){
    assert(n <= buf.size());
    buf.erase(buf.begin(), buf.begin() + n);
}

//3. accept a new incoming client connection non-blockingly
static Conn *handle_accept(int fd){
    struct sockaddr_in client_addr = {};
    socklen_t addrlen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
    if(connfd < 0){
        if(errno == EAGAIN || errno == EWOULDBLOCK){
            return nullptr;
        }
        return nullptr; //why tf do you need if statement above then?
    }

    fd_set_nb(connfd);

    Conn *conn = new Conn();
    conn->fd = connfd;
    conn->want_read = true;
    return conn;
}

//helper to inspect the buffer and processe exactly one complete packet if ready

static bool try_one_request(Conn *conn){
    //check if we can atleast read the 4 byte lengrh header
    if(conn->incoming.size() < 4){
        return false;
    }

    uint32_t len = 0;
    std::memcpy(&len, conn->incoming.data(), 4);
    if(len > k_max_msg){
        std::cerr << "[Server] Protocol error: message too long (" << len << " bytes)" << std::endl;
        conn->want_close = true;
        return false;
    }

    //check if the entire payload?? body has arrived in our stream buffer
     if(4 + len > conn->incoming.size()){
        return false;
     }

     //extract message body reference
     const uint8_t *request = &conn->incoming[4];

     //application logic workroom
     buf_append(conn->outgoing, (const uint8_t *)&len, 4);
     buf_append(conn->outgoing, request, len);

     //slice the processed packet cleanly off our dynamic vector stream
     buf_consume(conn->incoming, 4 + len);
     return true;
}

//4 gulp data into incoming stream buffer
static void handle_read(Conn *conn){
    uint8_t buf[64 * 1024];
    ssize_t rv = read(conn->fd, buf, sizeof(buf));

    if(rv < 0){
        if(errno == EAGAIN || errno == EWOULDBLOCK) return;
        conn->want_close = true;
        return;
    }

    if(rv == 0){
        conn->want_close = true; //clean end of file (client disconnected)
        return;
    }

    buf_append(conn->incoming, buf, (size_t)rv);

    //keep parsing packets sequentially as long as full messages are available
    while(try_one_request(conn)){}

    //request response toggle state logic
    if(conn->outgoing.size() > 0){
        conn->want_read = false;
        conn->want_write = true;

        return handle_write(conn);
    }
}

//4b flush data out of outgoing vector outbox buffer
static void handle_write(Conn *conn){
    assert(conn->outgoing.size() > 0);
    ssize_t rv = write(conn->fd, conn->outgoing.data(), conn->outgoing.size());

    if(rv < 0){
        if(errno == EAGAIN || errno == EWOULDBLOCK) return;
        conn->want_close = true;
        return;    
    }
    //slice off whatever portion of bytes the kernel accepted
    buf_consume(conn->outgoing, (size_t)rv);

    //response-request toggle state logic
    if(conn->outgoing.size() == 0){
        conn->want_read = true;
        conn->want_write = false;
    }
}

int main(){
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(server_fd < 0) die("socket()");

    int val = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if(bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) die("bind()");

    if(listen(server_fd, SOMAXCONN) < 0) die("listen()");

    //set the master listening door socket to non blocking
    fd_set_nb(server_fd);

    std::vector<Conn *> fd2conn;
    std::vector<struct pollfd> poll_args;

    std::cout << "[Server] Asnc Event-Leep running scurely on port 1234..." << std::endl;

    while(true){
        //1. pack fresh array of polling instructions
        poll_args.clear();

        struct pollfd pfd_listen = {server_fd, POLLIN, 0};
        poll_args.push_back(pfd_listen);

        for(Conn *conn : fd2conn){
            if(!conn) continue;
            struct pollfd pfd = {conn->fd, POLLERR, 0};
            if(conn->want_read) pfd.events |= POLLIN;
            if(conn->want_write) pfd.events |= POLLOUT;
            poll_args.push_back(pfd);
        }

        //2. yield execution until hardware event arrives
        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);

        if(rv < 0 && errno == EINTR) continue;
        if(rv < 0) die("poll()");

        //3 handle the master front door socket
        if(poll_args[0].revents & POLLIN){
            if(Conn *conn = handle_accept(server_fd)){
                if(fd2conn.size() <= (size_t)conn->fd){
                    fd2conn.resize(conn->fd + 1, nullptr);
                }
                fd2conn[conn->fd] = conn;
            }
        }

        //4 and 5: invoke callbacks and manage cleanup pipelines
        for(size_t i = 1; i < poll_args.size(); ++i){
            uint32_t ready = poll_args[i].revents;
            Conn *conn = fd2conn[poll_args[i].fd];
            if(!conn) continue;

            if(ready & POLLIN) handle_read(conn);
            if(ready & POLLOUT) handle_write(conn);

            //garbage collection step
            if((ready & POLLERR) || conn->want_close){
                (void)close(conn->fd);
                fd2conn[conn->fd] = nullptr;
                delete conn;
            }
        }
    }

    return 0;
}