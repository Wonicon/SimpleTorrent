#include "metainfo.h"
#include "bparser.h"
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <openssl/sha.h>

#define PIECE_HASH (1 << 0)
#define PEERS (1 << 1)

static inline void
put_indent(int indent)
{
    printf("%*s", indent, "");
}

#define print_with_indent(indent, fmt, ...) \
    printf("%*s" fmt, indent, "", ## __VA_ARGS__)

static void
print_int(const struct BNode *bi, int indent, int flags)
{
    print_with_indent(indent, "%ld\n", bi->i);
}

static void
print_str(const struct BNode *b, int indent, int flags)
{
    if (flags & PIECE_HASH) {
        put_indent(indent);
        for (int i = 0; i < HASH_SIZE; i++) {
            printf("%02x", (unsigned char)b->s_data[i]);
        }
        puts("...");
    }
    else if (flags & PEERS) {
        put_indent(indent);
        int n = b->s_size / 6;
        printf("size %ld, n %d\n", b->s_size, n);
        for (int i = 0; i < n; i++) {
            put_indent(indent + 4);
            struct {
                unsigned char ip[4];
                short port;
            } *peer = (void *)(b->s_data + i * 6);
            printf("%u.%u.%u.%u:%u\n", peer->ip[0], peer->ip[1], peer->ip[2], peer->ip[3], ntohs(peer->port));
        }
    }
    else {
        print_with_indent(indent, "\"%s\"\n", b->s_data);
    }
}

void print_bcode(const struct BNode *bnode, int indent, int flags);

static void
print_list(const struct BNode *list, int indent, int flags)
{
    print_with_indent(indent, "[\n");

    while (list) {
        print_bcode(list->l_item, indent + 2, flags);
        list = list->l_next;
    }

    print_with_indent(indent, "]\n");
}

static void
print_dict(const struct BNode *dict, int indent, int flags)
{
    print_with_indent(indent, "{\n");

    while (dict) {
        print_with_indent(indent + 2, "\"%s\":", dict->d_key);
        int flags_new = flags;
        if (!strcmp(dict->d_key, "pieces")) {
            flags_new |= PIECE_HASH;
        }
        else if (!strcmp(dict->d_key, "peers")) {
            flags_new |= PEERS;
        }
        switch (dict->d_val->type) {
        case B_LIST: puts(""); print_list(dict->d_val, indent + 2, flags_new); break;
        case B_DICT: puts(""); print_dict(dict->d_val, indent + 2, flags_new); break;
        case B_STR:  print_str(dict->d_val, 1, flags_new); break;
        default:     print_int(dict->d_val, 1, flags_new); break;
        }
        dict = dict->d_next;
    }

    print_with_indent(indent, "}\n");
}

void
print_bcode(const struct BNode *node, int indent, int flags)
{
    switch (node->type) {
    case B_LIST: print_list(node, indent, flags);   break;
    case B_DICT: print_dict(node, indent, flags);   break;
    case B_STR:  print_str(node, indent, flags);    break;
    default:     print_int(node, indent, flags); break;
    }
}

const struct BNode *
dfs_bcode(const struct BNode *node, const char *key)
{
    switch (node->type) {
    case B_LIST:
        for (const struct BNode *iter = node; iter; iter = iter->l_next) {
            const struct BNode *ret = dfs_bcode(iter->l_item, key);
            if (ret) {
                return ret;
            }
        }
        return NULL;
    case B_DICT:
        for (const struct BNode *iter = node; iter; iter = iter->d_next) {
            if (!strcmp(key, iter->d_key)) {
                return iter->d_val;
            }
            const struct BNode *ret = dfs_bcode(iter->d_val, key);
            if (ret != NULL) {
                return ret;
            }
        }
        return NULL;
    case B_STR:
        if (!strcmp(key, node->s_data)) {
            return node;
        }
        else {
            return NULL;
        }
    default:
        return NULL;
    }
}

void
make_info_hash(const struct BNode *root, void *md)
{
    const struct BNode *val = dfs_bcode(root, "info");
    SHA1((void *)val->start, val->end - val->start, md);  // avoid the last 'e' for the top-level dict
}
