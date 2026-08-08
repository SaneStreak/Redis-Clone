#include "zset.h"
#include "common.h"
#include <cstdlib>

bool znode_eq(HNode *lhs, HNode *rhs) {
    ZNode *le = container_of(lhs, ZNode, hmap);
    ZNode *re = container_of(rhs, ZNode, hmap);
    if (le->len != re->len) return false;
    return memcmp(le->name, re->name, le->len) == 0;
}

bool zless(AVLNode *a, AVLNode *b) {
    ZNode *za = container_of(a, ZNode, tree);
    ZNode *zb = container_of(b, ZNode, tree);
    if (za->score != zb->score) return za->score < zb->score;
    int rv = memcmp(za->name, zb->name, std::min(za->len, zb->len));
    if (rv != 0) return rv < 0;
    return za->len < zb->len;
}

ZNode *znode_new(const char *name, size_t len, double score) {
    ZNode *node = (ZNode *)malloc(sizeof(ZNode) + len + 1);
    avl_init(&node->tree);
    node->hmap.next = nullptr;
    node->hmap.hcode = str_hash((const uint8_t *)name, len);
    node->score = score;
    node->len = len;
    memcpy(node->name, name, len);
    node->name[len] = '\0';
    return node;
}

static void tree_add(ZSet *zset, ZNode *node) {
    AVLNode *cur = zset->tree;
    AVLNode *parent = nullptr;
    while (cur) {
        parent = cur;
        if (zless(&node->tree, cur)) {
            cur = cur->left;
        } else {
            cur = cur->right;
        }
    }
    node->tree.parent = parent;
    if (parent) {
        if (zless(&node->tree, parent)) parent->left = &node->tree;
        else parent->right = &node->tree;
        zset->tree = avl_fix(parent);
    } else {
        zset->tree = &node->tree;
    }
}

ZNode *zset_lookup(ZSet *zset, const char *name, size_t len) {
    if (!zset->hmap.newer.tab) return nullptr;
    ZNode key;
    key.hmap.hcode = str_hash((const uint8_t *)name, len);
    key.len = len;
    memcpy(key.name, name, len);
    HNode *found = hm_lookup(&zset->hmap, &key.hmap, &znode_eq);
    return found ? container_of(found, ZNode, hmap) : nullptr;
}

bool zset_add(ZSet *zset, const char *name, size_t len, double score) {
    ZNode *node = zset_lookup(zset, name, len);
    if (node) {
        if (node->score == score) return false;
        zset->tree = avl_del(&node->tree);
        node->score = score;
        avl_init(&node->tree);
        tree_add(zset, node);
        return false;
    } else {
        node = znode_new(name, len, score);
        hm_insert(&zset->hmap, &node->hmap);
        tree_add(zset, node);
        return true;
    }
}