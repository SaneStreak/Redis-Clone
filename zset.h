#pragma once
#include "avl.h"
#include "hashtable.h"

struct ZSet {
    AVLNode *tree = nullptr;
    HMap hmap;
};

struct ZNode {
    AVLNode tree;
    HNode hmap;
    double score = 0;
    size_t len = 0;
    char name[0];
};

bool znode_eq(HNode *lhs, HNode *rhs);
bool zless(AVLNode *a, AVLNode *b);
ZNode *znode_new(const char *name, size_t len, double score);
ZNode *zset_lookup(ZSet *zset, const char *name, size_t len);
bool zset_add(ZSet *zset, const char *name, size_t len, double score);