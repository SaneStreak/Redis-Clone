# MiniRedis

A single-threaded, high-performance, in-memory key-value database built in C++17 modeled after Redis. MiniRedis is engineered to guarantee **predictable, bounded $p99$ tail latency** under heavy write workloads through intrusive memory structures, asynchronous non-blocking I/O multiplexing, and twin-table progressive rehashing.

---

## Key Features

* **Asynchronous Network Reactor:** Multi-client event loop powered by non-blocking sockets (`O_NONBLOCK`) and POSIX `poll()` to eliminate multi-threading context-switching overhead and mutex lock contention.
* **Intrusive Data Structures:** Embedded `HNode` and `AVLNode` hooks inside data payloads eliminate dynamic memory wrapper allocations on insertions and maximize CPU L1/L2 cache locality.
* **Bounded $p99$ Tail Latency:** Incremental twin-table rehashing (`newer` and `older`) migrates up to 128 buckets per request, eliminating $O(N)$ "stop-the-world" resize freezes.
* **Dual-Indexed Sorted Sets (ZSET):** Single `ZNode` allocations house both `HNode` (hashtable) and `AVLNode` (AVL tree) hooks, providing $O(1)$ member lookups alongside $O(\log N)$ score-based range operations.
* **Binary-Safe Framing & RESP Protocol:** 4-byte length-prefixed framing protects against TCP packet fragmentation, paired with full Redis Serialization Protocol (RESP) output support (`$`, `:`, `-`).
* **Zero-Allocation Lookups & Branchless Detachments:** Employs stack-allocated dummy objects for searches and double-pointer (`HNode**`) branchless list modifications to remove CPU branch mispredictions on hot paths.

---

## Architecture Overview

```text
┌────────────────────────────────────────────────────────────────────────┐
│ 1. Async Network Reactor (server.cpp)                                  │
│    - Non-blocking Sockets (O_NONBLOCK) multiplexed via poll()           │
│    - Per-connection state buffers (incoming / outgoing)                │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ Raw Socket Bytes
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│ 2. Framing & Protocol Parser (server.cpp)                              │
│    - 4-Byte Outer Length Framing Header (TCP Fragment Protection)      │
│    - RESP Serialization (Bulk Strings, Integers, Errors, Arrays)       │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ Deserialized Command Vector
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│ 3. In-Memory Storage Engine                                            │
│    - Master Hash Map (HMap: hashtable.h / hashtable.cpp)               │
│      └─ Progressive Rehashing (128 buckets/op micro-migrations)        │
│    - Dual-Indexed Sorted Set (ZSet: zset.h / zset.cpp / avl.cpp)       │
│      └─ Intrusive HNode (O(1) Lookup) + Intrusive AVLNode (O(log N))   │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ Direct Memory Pointer
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│ 4. Zero-Copy Outbound Response Pipeline (server.cpp)                  │
│    - Direct buffer appending without intermediate heap string copies   │
└────────────────────────────────────────────────────────────────────────┘

```

---

## Project Structure

| File | Description |
| --- | --- |
| `common.h` | Shared preprocessor macros (`container_of`), FNV-1a hash functions, and error utilities. |
| `hashtable.h` / `.cpp` | Intrusive hashtable (`HMap`), double-pointer detachments (`HNode**`), and progressive rehashing. |
| `avl.h` / `.cpp` | Intrusive balanced binary search tree (`AVLNode`), rotations (`rot_left`, `rot_right`), and subtree rank counts (`cnt`). |
| `zset.h` / `.cpp` | Sorted Set dual-indexing engine, Flexible Array Member allocation (`ZNode`), and score ordering predicates (`zless`). |
| `server.cpp` | Non-blocking socket configuration, `poll()` event loop, RESP framing parser, and command dispatchers. |
| `client.cpp` | Test client sending pipelined requests over TCP and displaying formatted RESP server outputs. |
| `Makefile` | Build configuration enforcing strict C++17 compiler flags (`-Wall -Wextra -O2`) and incremental compilation. |

---

## Supported Commands

| Command | Arguments | Description | RESP Output Format |
| --- | --- | --- | --- |
| `SET` | `key value` | Sets key to string value. | Bulk String (`$2\r\nOK\r\n`) |
| `GET` | `key` | Retrieves string value for key. | Bulk String (`$len\r\nvalue\r\n`) or Null (`$-1\r\n`) |
| `DEL` | `key` | Removes key from storage engine. | Integer (`:1\r\n` if deleted, `:0\r\n` if not found) |
| `ZADD` | `key score member` | Inserts or updates member score in Sorted Set. | Integer (`:1\r\n` if new member added, `:0\r\n` if updated) |

---

## Building and Running

### Prerequisites

* GCC/G++ supporting C++17 (`g++ >= 7.0`)
* POSIX-compliant operating system (Linux / WSL / macOS)
* GNU `make`

### 1. Compilation

Compile the modular source files into the binary target via the provided Makefile:

```bash
make clean && make

```

### 2. Launching the Server

Start the database server listening on TCP port `1234`:

```bash
./server

```

*Console Output:*

```text
[Server] Modular Redis Clone active on port 1234...

```

### 3. Running Integration Tests

In a separate terminal, compile and execute the test client to issue pipelined commands (`SET`, `GET`, `DEL`, `ZADD`):

```bash
g++ -O2 -Wall -std=c++17 client.cpp -o client
./client

```

*Client Terminal Output:*

```text
[Client] Transmitting 5 pipelined commands to server...

[Server RESP Responses]:
$2
OK
$9
val_alpha
:1
$-1
:1

```

---

## Core Systems Engineering Highlights

### 1. `container_of` Macro Arithmetic

Parent structure recovery from embedded structural hooks (`HNode` or `AVLNode`) without standard library wrapper allocation:

$$\text{Parent Pointer} = (\text{type}*)\left((\text{char}*)ptr - \text{offsetof}(\text{type}, \text{member})\right)$$

```cpp
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

```

### 2. Branchless Deletion via Double Pointers

`h_lookup` returns `HNode**` (address of incoming pointer), allowing list head and chain middle node detachments without conditional `if-else` branching:

```cpp
static HNode *h_detach(HTab *htab, HNode **from) {
    HNode *node = *from;
    *from = node->next; // Single branchless assignment instruction
    htab->size--;
    return node;
}

```

### 3. Progressive Twin-Table Rehashing

When table load factor breaches 8, a new double-capacity table is allocated via `calloc()` (lazy Copy-On-Write kernel allocation). Every incoming database operation migrates a fixed micro-unit of work (`k_rehashing_work = 128` nodes) from `older` to `newer`, converting an $O(N)$ freeze into smooth $O(1)$ turns.