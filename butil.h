/**
 * @file butil.h
 * @brief 基于 B 编码语法树的工具函数声明
 */

#ifndef BUTIL_H
#define BUTIL_H

#include "bparser.h"

/**
 * @brief 打印以 node 为根的 bencode 树
 * @param node 语法树根结点
 * @param indent 起始缩进
 * @param flag 特殊打印要求
 */
void print_bcode(const struct BNode *node, int indent, int flag);

/**
 * @brief 搜索 bencode 树中字典里的某个键 key, 返回对应的值结点
 * @param tree 语法树根结点
 * @param key 要搜索的键
 * @return 键对应的值结点，没有则返回 NULL.
 */
const struct BNode *query_bcode_by_key(const struct BNode *tree, const char *key);

/**
 * @brief 计算 torrent 文件的 info hash
 * @param root 语法树根
 * @param md sha1 输出缓冲区（至少 HASH_SIZE 字节）
 */
void make_info_hash(const struct BNode *root, unsigned char *md);

#endif  // BUTIL_H
