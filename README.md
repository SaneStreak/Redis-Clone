# Custom In-Memory Key-Value Storage Engine (Redis Clone)

A high-performance, single-threaded in-memory key-value database built from scratch in C++ to demonstrate low-level systems programming, non-blocking network I/O multiplexing, and custom protocol serialization.

## Core Architectural Features

* **Event-Driven Asynchronous Core:** Leverages the Linux `poll()` system call to handle thousands of concurrent client connections over a single execution thread, completely bypassing multi-threading CPU context-switching overhead.
* **Stream-Driven Network Parsing:** Eliminates TCP stream boundaries and pipeline stalls by processing network buffers sequentially using an asynchronous stream-parsing state machine.
* **Optimistic Write Optimization:** Minimizes kernel space transitions by bypassing the event loop cycle to write pending response buffers directly in user space whenever sockets are cleared.
* **Nested Length-Prefixed Protocol:** Implements a custom binary serialization schema supporting dynamic array commands (`nstr` + multi-string arguments) without risks of delimiter collision.

## Repository Structure

```
redis-clone/
├── server.cpp       # Non-blocking event loop and packet deserializer
└── client.cpp       # Pipelined test verification harness

```

## Compilation & Quick Start

### 1. Build Binaries

Compile the codebase using high-optimization flags:

```bash
g++ -Wall -Wextra -O2 server.cpp -o server
g++ -Wall -Wextra -O2 client.cpp -o client

```

### 2. Launch Server with Kernel Diagnostics

Run the server wrapped inside the Linux `strace` utility to intercept and verify underlying system call signatures (`poll`, `read`, `write`) in real-time:

```bash
strace ./server > /dev/null

```

### 3. Run Pipeline Verification Client

Execute the client binary in a separate terminal window to fire a multi-message burst to verify stream-drain mechanics:

```bash
./client

```