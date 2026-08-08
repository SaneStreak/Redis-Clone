#include "common.h"
#include "hashtable.h"
#include "avl.h"
#include "zset.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>

const size_t k_max_msg = 32 << 20;
const size_t k_max_args = 200 * 1000;

enum { TAG_NIL = 0, TAG_STR = 1, TAG_ZSET = 2 };

struct Entry {
    HNode node;
    std::string key;
    uint32_t type = 0;
    std::string str;
    ZSet zset;
};

static bool entry_eq(HNode *lhs, HNode *rhs) {
    Entry *le = container_of(lhs, Entry, node);
    Entry *re = container_of(rhs, Entry, node);
    return le->key == re->key;
}

static struct { HMap db; } g_data;

struct Conn {
    int fd = -1;
    bool want_read = false;
    bool want_write = false;
    bool want_close = false;
    std::vector<uint8_t> incoming;
    std::vector<uint8_t> outgoing;
};

static void fd_set_nb(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) die("fcntl(F_GETFL) failed");
    flags |= O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) < 0) die("fcntl(F_SETFL) failed");
}

static void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

static void buf_consume(std::vector<uint8_t> &buf, size_t n) {
    assert(n <= buf.size());
    buf.erase(buf.begin(), buf.begin() + n);
}

static void out_nil(std::vector<uint8_t> &out) {
    buf_append(out, (const uint8_t *)"$-1\r\n", 5);
}

static void out_str(std::vector<uint8_t> &out, const std::string &val) {
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "$%zu\r\n", val.size());
    buf_append(out, (const uint8_t *)buf, len);
    buf_append(out, (const uint8_t *)val.data(), val.size());
    buf_append(out, (const uint8_t *)"\r\n", 2);
}

static void out_int(std::vector<uint8_t> &out, int64_t val) {
    char buf[64];
    int len = snprintf(buf, sizeof(buf), ":%lld\r\n", (long long)val);
    buf_append(out, (const uint8_t *)buf, len);
}

static void out_err(std::vector<uint8_t> &out, int code, const std::string &msg) {
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "-ERR %d %s\r\n", code, msg.c_str());
    buf_append(out, (const uint8_t *)buf, len);
}

static void do_get(std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((const uint8_t *)key.key.data(), key.key.size());

    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (!node) return out_nil(out);

    Entry *entry = container_of(node, Entry, node);
    if (entry->type != TAG_STR) return out_err(out, 1, "expect string type");
    out_str(out, entry->str);
}

static void do_set(std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((const uint8_t *)key.key.data(), key.key.size());

    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (node) {
        Entry *entry = container_of(node, Entry, node);
        entry->type = TAG_STR;
        entry->str.swap(cmd[2]);
    } else {
        Entry *entry = new Entry();
        entry->key.swap(key.key);
        entry->node.hcode = key.node.hcode;
        entry->type = TAG_STR;
        entry->str.swap(cmd[2]);
        hm_insert(&g_data.db, &entry->node);
    }
    out_str(out, "OK");
}

static void do_del(std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((const uint8_t *)key.key.data(), key.key.size());

    HNode *node = hm_delete(&g_data.db, &key.node, &entry_eq);
    if (!node) return out_int(out, 0);

    Entry *entry = container_of(node, Entry, node);
    delete entry;
    out_int(out, 1);
}

static void do_zadd(std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    double score = 0;
    try {
        score = std::stod(cmd[2]);
    } catch (...) {
        return out_err(out, 1, "invalid score parameter");
    }

    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((const uint8_t *)key.key.data(), key.key.size());

    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    Entry *entry = nullptr;
    if (!node) {
        entry = new Entry();
        entry->key.swap(key.key);
        entry->node.hcode = key.node.hcode;
        entry->type = TAG_ZSET;
        hm_insert(&g_data.db, &entry->node);
    } else {
        entry = container_of(node, Entry, node);
        if (entry->type != TAG_ZSET) return out_err(out, 1, "expect zset type");
    }

    bool added = zset_add(&entry->zset, cmd[3].data(), cmd[3].size(), score);
    out_int(out, added ? 1 : 0);
}

static int32_t parse_req(const uint8_t *data, size_t size, std::vector<std::string> &out) {
    const uint8_t *end = data + size;
    uint32_t nstr = 0;

    if (size < 4) return -1;
    memcpy(&nstr, data, 4);
    data += 4;

    if (nstr > k_max_args) return -1;

    while (out.size() < nstr) {
        if (data + 4 > end) return -1;
        uint32_t len = 0;
        memcpy(&len, data, 4);
        data += 4;
        if (data + len > end) return -1;
        out.push_back(std::string((const char *)data, len));
        data += len;
    }

    return (data == end) ? 0 : -1;
}

static void do_request(std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    if (cmd.size() == 2 && cmd[0] == "get") do_get(cmd, out);
    else if (cmd.size() == 3 && cmd[0] == "set") do_set(cmd, out);
    else if (cmd.size() == 2 && cmd[0] == "del") do_del(cmd, out);
    else if (cmd.size() == 4 && cmd[0] == "zadd") do_zadd(cmd, out);
    else out_err(out, 1, "unknown command");
}

static bool try_one_request(Conn *conn) {
    if (conn->incoming.size() < 4) return false;
    uint32_t len = 0;
    memcpy(&len, conn->incoming.data(), 4);
    if (len > k_max_msg) {
        conn->want_close = true;
        return false;
    }

    if (4 + len > conn->incoming.size()) return false;

    std::vector<std::string> cmd;
    if (parse_req(&conn->incoming[4], len, cmd) < 0) {
        conn->want_close = true;
        return false;
    }

    do_request(cmd, conn->outgoing);
    buf_consume(conn->incoming, 4 + len);
    return true;
}

static void handle_write(Conn *conn) {
    assert(conn->outgoing.size() > 0);
    ssize_t rv = write(conn->fd, conn->outgoing.data(), conn->outgoing.size());
    if (rv < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        conn->want_close = true;
        return;
    }
    buf_consume(conn->outgoing, (size_t)rv);
    if (conn->outgoing.size() == 0) {
        conn->want_read = true;
        conn->want_write = false;
    }
}

static void handle_read(Conn *conn) {
    uint8_t buf[64 * 1024];
    ssize_t rv = read(conn->fd, buf, sizeof(buf));
    if (rv < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        conn->want_close = true;
        return;
    }
    if (rv == 0) {
        conn->want_close = true;
        return;
    }
    buf_append(conn->incoming, buf, (size_t)rv);
    while (try_one_request(conn)) {}
    if (conn->outgoing.size() > 0) {
        conn->want_read = false;
        conn->want_write = true;
        return handle_write(conn);
    }
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) die("socket()");

    int val = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) die("bind()");
    if (listen(server_fd, SOMAXCONN) < 0) die("listen()");

    fd_set_nb(server_fd);
    std::vector<Conn *> fd2conn;
    std::vector<struct pollfd> poll_args;

    std::cout << "[Server] Modular Redis Clone active on port 1234..." << std::endl;

    while (true) {
        poll_args.clear();
        struct pollfd pfd_listen = {server_fd, POLLIN, 0};
        poll_args.push_back(pfd_listen);

        for (Conn *conn : fd2conn) {
            if (!conn) continue;
            struct pollfd pfd = {conn->fd, POLLERR, 0};
            if (conn->want_read) pfd.events |= POLLIN;
            if (conn->want_write) pfd.events |= POLLOUT;
            poll_args.push_back(pfd);
        }

        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);
        if (rv < 0 && errno == EINTR) continue;
        if (rv < 0) die("poll()");

        if (poll_args[0].revents & POLLIN) {
            struct sockaddr_in client_addr = {};
            socklen_t addrlen = sizeof(client_addr);
            int connfd = accept(server_fd, (struct sockaddr *)&client_addr, &addrlen);
            if (connfd >= 0) {
                fd_set_nb(connfd);
                Conn *conn = new Conn();
                conn->fd = connfd;
                conn->want_read = true;
                if (fd2conn.size() <= (size_t)conn->fd) {
                    fd2conn.resize(conn->fd + 1, nullptr);
                }
                fd2conn[conn->fd] = conn;
            }
        }

        for (size_t i = 1; i < poll_args.size(); ++i) {
            uint32_t ready = poll_args[i].revents;
            Conn *conn = fd2conn[poll_args[i].fd];
            if (!conn) continue;

            if (ready & POLLIN) handle_read(conn);
            if (ready & POLLOUT) handle_write(conn);

            if ((ready & POLLERR) || conn->want_close) {
                (void)close(conn->fd);
                fd2conn[conn->fd] = nullptr;
                delete conn;
            }
        }
    }
    return 0;
}