#include <iostream>
#include <vector> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <cstring>
#include <cassert>
#include <map>

const size_t k_max_msg = 32 << 20;
const size_t k_max_args = 200 * 1000; //safety limit matching the target architecture

enum {
    RES_OK = 0, //success
    RES_ERR = 1, //unrecognizeed or malformed content
    RES_NX = 2, //key not found 
};

//per connection state
struct Conn {
    int fd = -1;
    bool want_read = false;
    bool want_write = false;
    bool want_close = false;
    std::vector<uint8_t> incoming;
    std::vector<uint8_t> outgoing;
};

static void handle_write(Conn *conn);

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

//to read the 4 byte integer
static bool read_u32(const uint8_t *&cur, const uint8_t *end, uint32_t &out){
    if(cur + 4 > end){
        return false;
    }

    memcpy(&out, cur, 4);
    cur += 4;
    return true;
}

//to read the actual string
static bool read_str(const uint8_t *&cur, const uint8_t *end, size_t n, std::string &out){
    if(cur + n > end){
        return false;
    }
    out.assign((const char *)cur, n);
    cur += n;
    return true;
}

struct Response {
    uint32_t status = 0;
    //old
    //std::vector<uint8_t> data;

    //new
    const std::string *data_ptr = nullptr; //pointer to the raw string inside g_data
};

//
static int32_t parse_req(const uint8_t *data, size_t size, std::vector<std::string> &out){
    const uint8_t *end = data + size;
    uint32_t nstr = 0;

    //extract the multi string argument count header (nstr)
    if(!read_u32(data, end, nstr)){
        return -1;
    }

    //safety cap validation
    if(nstr > k_max_args){
        return -1;
    }

    //loop until all designated length prefixed strings are parsed
    while(out.size() < nstr){
        uint32_t len = 0;
        if(!read_u32(data, end, len)){
            return -1;
        }
        out.push_back(std::string());
        if(!read_str(data, end, len, out.back())){
            return -1;
        }
    }

    //assert no trailing garbage data exists outside the packet declaration frame
    if(data != end){
        return -1;
    }

    return 0;
}

//global placeholder storage mag; later drop it for my own hashtable in later chapter
static std::map<std::string, std::string> g_data;

static void do_request(std::vector<std::string> &cmd, Response &out){
    if(cmd.size() == 2 && cmd[0] == "get"){
        auto it = g_data.find(cmd[1]);
        if(it == g_data.end()){
            out.status = RES_NX; //key doesn't exist
            return;
        }   
        //const std::string &val = it->second;
        // out.data.assign(val.begin(), val.end()); //copy raw string value bytes into byte array

        //updated
        out.data_ptr = &it->second;
        out.status = RES_OK; //zero copy: assign the address of the string inside the map directly
    }else if(cmd.size() == 3 && cmd[0] == "set"){
        g_data[cmd[1]].swap(cmd[2]); //zero copy swap efficiency to transfer string data ownership
        out.status = RES_OK;
    }else if(cmd.size() == 2 && cmd[0] == "del"){
        g_data.erase(cmd[1]);
        out.status = RES_OK;
    }else{
        out.status = RES_ERR; //unrecognized command structure
    }

}

static void make_response(const Response &resp, std::vector<uint8_t> &out){
    uint32_t data_size = resp.data_ptr ? (uint32_t)resp.data_ptr.size() : 0;
    uint32_t resp_len = 4 + data_size; //4 bytes for status code + data payload bytes

    //1. serialize the outer packet messagelength header
    buf_append(out, (const uint8_t *)&resp_len, 4);

    //2. serialize the application response status code
    buf_append(out, (const uint8_t *)&resp.status, 4);

    //3. zero copy leap: push the data directly from the storage map memore pool
    if(data_size > 0){
        buf_append(out, (const uint8_t *)resp.data_ptr->data(), data_size);
    }
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
    // buf_append(conn->outgoing, (const uint8_t *)&len, 4);
    // buf_append(conn->outgoing, request, len);

    //updated:
    //1. parse the raw request payload into a clean string vector
    std::vector<std::string> cmd;
    if(parse_req(request, len, cmd) < 0){
        std::cerr << "[Server] Protocol error: bad request layout" << std::endl;
        conn->want_close = true;
        return false;
    }

    //2. Process the command using the global map reouting matrix
    Response resp;
    do_request(cmd, resp);

    //3. serialize the response directly onto the outbound network stream
    make_response(resp, conn->outgoing);

    //end of application logic
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