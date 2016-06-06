/**
 * @file butil.c
 * @brief 基于 B 编码语法树的工具函数实现
 */

#include "butil.h"
#include "metainfo.h"
#include <string.h>
#include <arpa/inet.h>
#include <openssl/sha.h>

#define PIECE_HASH (1 << 0)  ///< 打印 hash 的 flag
#define PEERS (1 << 1)       ///< 打印二进制 peers 列表的 flag

/**
 * @brief 输出缩进
 * @param indent 缩进量
 */
static inline void
put_indent(int indent)
{
    printf("%*s", indent, "");
}

/**
 * @brief 带缩进的格式化打印
 */
#define print_with_indent(indent, fmt, ...) \
    printf("%*s" fmt, indent, "", ## __VA_ARGS__)

/**
 * @brief 打印整型
 * @param bi 整型结点
 * @param indent 缩进级别
 */
static void
print_int(const struct BNode *bi, int indent)
{
    print_with_indent(indent, "%ld\n", bi->i);
}

/**
 * @brief 打印串
 * @param b 串结点
 * @param indent 缩进级别
 * @param flags 打印格式要求
 */
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
        size_t n = b->s_size / 6;
        printf("size %ld, n %lu\n", b->s_size, n);
        for (int i = 0; i < n; i++) {
            put_indent(indent + 4);
            struct {
                unsigned char ip[4];
                unsigned short port;
            } *peer = (void *)(b->s_data + i * 6);
            printf("%u.%u.%u.%u:%u\n", peer->ip[0], peer->ip[1], peer->ip[2], peer->ip[3], ntohs(peer->port));
        }
    }
    else {
        print_with_indent(indent, "\"%s\"\n", b->s_data);
    }
}

/**
 * @brief 打印列表
 * @param list 列表结点
 * @param indent 缩进级别
 * @param flags 打印格式要求
 */
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

/**
 * @brief 打印字典
 * @param dict 字典结点
 * @param indent 缩进级别
 * @param flags 打印格式要求
 */
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
        default:     print_int(dict->d_val, 1); break;
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
    default:     print_int(node, indent); break;
    }
}

/**
 * @brief 深度优先搜索 bencode 树中字典里的某个键 key, 返回对应的值结点
 * @param node 语法树根结点
 * @param key 要搜索的键
 * @return 键对应的值结点，没有则返回 NULL.
 */
static const struct BNode *
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

const struct BNode *
query_bcode_by_key(const struct BNode *node, const char *key)
{
    return dfs_bcode(node, key);
}

void
make_info_hash(const struct BNode *root, unsigned char *md)
{
    const struct BNode *val = query_bcode_by_key(root, "info");
    SHA1((void *)val->start, val->end - val->start, md);  // avoid the last 'e' for the top-level dict
}
