#ifndef BUTIL_H
#define BUTIL_H

#include "bparser.h"

// 打印以 node 为根的 bencode 树
void print_bcode(const struct BNode *node, int indent, int print_hash);

// 深度优先搜索 bencode 树中字典里的某个键 key, 返回对应的值结点
const struct BNode *dfs_bcode(const struct BNode *tree, const char *key);

/**
 * @brief 计算 torrent 文件的 info hash
 * @param data 完整的种子文件数据
 * @param size 种子文件大小
 * @param md sha1 输出缓冲区（至少 HASH_SIZE 字节）
 */
void make_info_hash(const struct BNode *root, unsigned char *md);

#endif  // BUTIL_H
