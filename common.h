#pragma once
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <cassert>

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

inline uint64_t str_hash(const uint8_t *data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; ++i) {
        h = (h ^ data[i]) * 0x100000001b3ULL;
    }
    return h;
}

inline void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}