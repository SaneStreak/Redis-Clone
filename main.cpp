#include <iostream>
#include <cstring> //for strlen
#include <sys/socket.h> //for socket(), bind(), listen(), accpet(), setsockopt()
#include <netinet/in.h> //for struct sockaddr_in, htons(), htonl()
#include <unistd.h> //for read(), write(), close()

//a simple error helper function
void die(const char *msg){ 
    perror(msg); //prints the error message along with the system reason why it failed
    exit(1); //terminate the program immediately
}

//step 6: read and write logic provided by the book
static void do_something(int connfd) {
    char rbuf[64] = {};
    // Read up to 63 bytes to leave room for the null terminator '\0'
    ssize_t n = read(connfd, rbuf, sizeof(rbuf) - 1);
    if (n < 0) {
        std::cerr << "read() error\n";
        return;
    }
    
    std::cout << "client says: " << rbuf << "\n";

    char wbuf[] = "world";
    ssize_t bytes_written = write(connfd, wbuf, strlen(wbuf));
    (void)bytes_written;
}

int main(){
    //step 1: obtain a socket handle
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0){
        die("socket");
    }

    //step 2: set socket options (SO_REUSEADDR) so we can restart without port blocks
    int val = 1;
    if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0)
        die("setsocketopt()");

    //step 3: bind to an address (0.0.0.0:1234)
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080); //port number converted to big-endian
    addr.sin_addr.s_addr = htonl(0); //wildcard ip 0.0.0.0 converted to big endian

    int rv = bind(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if(rv < 0)
        die("listen()");

    //step 4: Listen (forgot step 4 initially hence it won't take any input)
    rv = listen(fd, SOMAXCONN);
    if(rv < 0)
        die("listen()");

    std::cout << "Server is up and listening on port 1234... \n";

    //step 5: accept connections loop
    while(true){
        struct sockaddr_in client_addr = {};
        socklen_t addrlen = sizeof(client_addr);

        //wait here until a client connects
        int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
        if(connfd < 0)
            continue;

        //process the individual client connection
        do_something(connfd);

        //hang up the connection so the client isn't left hanging forever
        close(connfd);
    }

    close(fd);
    return 0;
}