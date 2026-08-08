#include "avl.h"

void avl_init(AVLNode *node) {
    node->depth = 1;
    node->cnt = 1;
    node->left = node->right = node->parent = nullptr;
}

uint32_t avl_depth(AVLNode *node) {
    return node ? node->depth : 0;
}

uint32_t avl_cnt(AVLNode *node) {
    return node ? node->cnt : 0;
}

static void avl_update(AVLNode *node) {
    node->depth = 1 + std::max(avl_depth(node->left), avl_depth(node->right));
    node->cnt = 1 + avl_cnt(node->left) + avl_cnt(node->right);
}

static AVLNode *rot_left(AVLNode *node) {
    AVLNode *new_node = node->right;
    if (new_node->left) new_node->left->parent = node;
    node->right = new_node->left;
    new_node->left = node;
    new_node->parent = node->parent;
    node->parent = new_node;
    avl_update(node);
    avl_update(new_node);
    return new_node;
}

static AVLNode *rot_right(AVLNode *node) {
    AVLNode *new_node = node->left;
    if (new_node->right) new_node->right->parent = node;
    node->left = new_node->right;
    new_node->right = node;
    new_node->parent = node->parent;
    node->parent = new_node;
    avl_update(node);
    avl_update(new_node);
    return new_node;
}

AVLNode *avl_fix(AVLNode *node) {
    while (true) {
        avl_update(node);
        uint32_t l = avl_depth(node->left);
        uint32_t r = avl_depth(node->right);
        AVLNode **from = node->parent ? (node->parent->left == node ? &node->parent->left : &node->parent->right) : nullptr;

        if (l == r + 2) {
            if (avl_depth(node->left->left) < avl_depth(node->left->right)) {
                node->left = rot_left(node->left);
            }
            node = rot_right(node);
        } else if (r == l + 2) {
            if (avl_depth(node->right->right) < avl_depth(node->right->left)) {
                node->right = rot_right(node->right);
            }
            node = rot_left(node);
        }

        if (from) *from = node;
        if (!node->parent) break;
        node = node->parent;
    }
    return node;
}

AVLNode *avl_del(AVLNode *node) {
    if (!node->right) {
        AVLNode *parent = node->parent;
        if (node->left) node->left->parent = parent;
        if (parent) {
            (parent->left == node ? parent->left : parent->right) = node->left;
            return avl_fix(parent);
        }
        return node->left;
    } else {
        AVLNode *victim = node->right;
        while (victim->left) victim = victim->left;
        AVLNode *root = avl_del(victim);
        *victim = *node;
        if (victim->left) victim->left->parent = victim;
        if (victim->right) victim->right->parent = victim;
        AVLNode *parent = node->parent;
        if (parent) {
            (parent->left == node ? parent->left : parent->right) = victim;
        }
        return root;
    }
}

AVLNode *avl_offset(AVLNode *node, int64_t offset) {
    int64_t pos = 0;
    while (offset != pos) {
        if (pos < offset && pos + avl_cnt(node->right) >= offset) {
            node = node->right;
            pos += 1 + avl_cnt(node->left);
        } else if (pos > offset && pos - avl_cnt(node->left) <= offset) {
            node = node->left;
            pos -= 1 + avl_cnt(node->right);
        } else {
            int64_t left_cnt = avl_cnt(node->left);
            if (offset < pos) {
                node = node->left;
                pos -= (1 + avl_cnt(node->right));
            } else {
                node = node->right;
                pos += (1 + left_cnt);
            }
        }
    }
    return node;
}