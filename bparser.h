/**
 * @file bparser.h
 * @brief B 编码 parser 的 API 声明
 */

#ifndef BPARSER_H
#define BPARSER_H

#include <stddef.h>
#include <inttypes.h>

/**
 * @brief B 编码语法结点类型标签
 */
enum BNodeType
{
    B_NA,
    B_STR,
    B_INT,
    B_LIST,
    B_DICT,
    NR_BTYPE
};

/**
 * @brief B 编码结点
 *
 * 使用标签 type 辨别结点的具体类型。不同的类型使用不同前缀的域：
 *
 * 1. l_ 表示列表
 * 2. d_ 表示字典
 * 3. s_ 表示串（虽然串有时候是字符串可以不需要 size, 但是也有二进制串）
 *
 * start 和 end 指向源缓冲区，以记录一个结点的字节范围，
 * 主要为准确计算 info_hash 所准备。但是结点中的实际数据
 * 是深拷贝的，与源缓冲区脱离，只是要使用 start 和 end 时
 * 要保证源缓冲区的有效性。
 */
struct BNode
{
    enum BNodeType type;              ///< 类型标签
    union
    {
        struct {
            struct BNode *l_item;     ///< 列表项
            struct BNode *l_next;     ///< 余下列表项
        };
        struct {
            char *d_key;              ///< 字典键（在 B 编码中是串，但是可以保证是字符串）
            struct BNode *d_val;      ///< 字典值
            struct BNode *d_next;     ///< 余下字典项
        };
        struct {
            size_t s_size;            ///< 串长度
            char *s_data;             ///< 串内容（深拷贝）
        };
        long i;                       ///< 整型
    };
    char *start;                      ///< 结点在源缓冲区的开始处
    char *end;                        ///< 结点在源缓冲区的结束处
};

// 解析 B 编码数据获取抽象语法树
struct BNode *bparser(char *bcode);

// 释放 B 编码的抽象语法树
void free_bnode(struct BNode **pbnode);

#endif //  BPARSER_H
