/**
 * @file bparser.c
 * @brief B 编码 parser 的 API 实现
 */

#include "bparser.h"
#include "util.h"
#include <string.h>

#define DELIM     ':'  ///< 长度与字节串的分割符
#define LEAD_INT  'i'  ///< 整型结点的起始字符
#define LEAD_LIST 'l'  ///< 列表结点的起始字符
#define LEAD_DICT 'd'  ///< 字典结点的起始字符
#define END       'e'  ///< 非串结点的终止字符

/**
 * @brief parser 状态
 *
 * 内部使用，用来记录当前 parsing 的状态，即字符指针的位置。
 */
struct State
{
    char *start;  ///< 源缓冲区起始地址
    char *curr;   ///< 解析的当前地址
};

/**
 * @brief 从数据流中取出一个长整型
 * @param st 指向状态记录
 * @return 返回匹配的长整型
 */
static inline size_t
parse_get_int(struct State *st)
{
    return strtoul(st->curr, &st->curr, 10);
}

/**
 * @brief 从数据流中取出一个给定长度的字符串
 * @param st 指向状态记录
 * @param length 字符串长度（不含 '\0'）
 * @return 动态分配的字符数组指针
 */
static inline char *
parse_get_str(struct State *st, size_t length)
{
    char *s = calloc(length + 1, sizeof(*s));
    memcpy(s, st->curr, length);  // 有时候会需要拷贝字节流
    st->curr += length;
    return s;
}

/**
 * @brief 从数据流中取出一个字符
 * @param st 指向状态记录
 * @return 取出的字符
 */
static inline char
parse_get_char(struct State *st)
{
    return *st->curr++;
}

/**
 * @brief 退回 N 个字节
 * @param st 指向状态记录
 * @param n 回退字节数
 */
static inline void
parse_back(struct State *st, int n)
{
    st->curr -= n;
}

/**
 * @brief 获取当前数据流位置
 * @param st 指向状态记录
 * @return 位置
 */
static inline size_t
pos(struct State *st)
{
    return st->curr - st->start;
}

/**
 * @brief 较为常用的错误类型
 */
#define error_unexpected_char(st, ch, expect) log("ERROR: unexpected '%c', expect '%c' at %lu", ch, expect, pos(st));

/**
 * @brief 构造一个新的语法结点
 * @param type 语法结点类型标签
 * @return 动态分配的语法结点
 */
static struct BNode *
new_bnode(enum BNodeType type)
{
    struct BNode *bnode = calloc(1, sizeof(*bnode));
    bnode->type = type;
    return bnode;
}

static struct BNode *parse_bcode(struct State *st);

/**
 * @brief 解析字符串（字典键）
 *
 * 这是解析串的子集，可以为解析串和解析字典所复用
 *
 * @param st parser 状态
 * @param len_o 如果不为 NULL, 写入字符串长度
 * @return 动态分配的字符串
 */
static char *
parse_bcode_is_key(struct State *st, size_t *len_o)
{
    size_t len = parse_get_int(st);

    char delim = parse_get_char(st);
    if (DELIM != delim) {
        error_unexpected_char(st, delim, DELIM);
    }

    if (len_o) *len_o = len;

    return parse_get_str(st, len);
}

/**
 * @brief 解析串
 *
 * 与解析字符串（字典键）不同，串可以是二进制语义，要记录长度。
 *
 * @param st parser 状态
 * @return 动态分配的串结点
 */
static struct BNode *
parse_bcode_is_str(struct State *st)
{
    struct BNode *bnode = new_bnode(B_STR);
    bnode->start = st->curr;
    bnode->s_data = parse_bcode_is_key(st, &bnode->s_size);
    bnode->end = st->curr;
    return bnode;
}

/**
 * @brief 解析整型
 *
 * 本函数假设调用者已经消耗了整型的起始字符 'i'.
 * 函数内部消耗终止字符 'e'.
 *
 * @param st parser 状态
 * @return 动态分配的整型结点
 */
static struct BNode *
parse_bcode_is_int(struct State *st)
{
    struct BNode *bnode = new_bnode(B_INT);
    bnode->start = st->curr - 1;

    bnode->i = parse_get_int(st);

    char end = parse_get_char(st);
    if (END != end) {
        error_unexpected_char(st, end, END);
    }

    bnode->end = st->curr;
    return bnode;
}

/**
 * @brief 递归下降地解析字典
 *
 * 本函数假设调用者已经消耗了字典的起始字符 'd'.
 * 函数内部消耗终止字符 'e'.
 *
 * @param st parser 状态
 * @return 动态分配的字典结点
 */
static struct BNode *
parse_bcode_is_dict(struct State *st)
{
    struct BNode *bnode;
    struct BNode **iter = &bnode;

    char *start = st->curr - 1;

    char ch;
    while ((ch = parse_get_char(st)) != END) {
        parse_back(st, 1);
        (*iter) = new_bnode(B_DICT);
        (*iter)->d_key = parse_bcode_is_key(st, NULL);  // 一定是字符串，不需要大小了
        (*iter)->d_val = parse_bcode(st);
        iter = &(*iter)->d_next;
    }

    if (END != ch) {
        error_unexpected_char(st, ch, END);
    }

    bnode->start = start;
    bnode->end = st->curr;
    return bnode;
}

/**
 * @brief 递归下降地解析列表
 *
 * 本函数假设调用者已经消耗了列表的起始字符 'l'.
 * 函数内部消耗终止字符 'e'.
 *
 * @param st parser 状态
 * @return 动态分配的列表结点
 */
static struct BNode *
parse_bcode_is_list(struct State *st)
{
    struct BNode *bnode;
    struct BNode **iter = &bnode;

    char *start = st->curr - 1;

    char ch;
    while ((ch = parse_get_char(st)) != EOF && END != ch) {
        parse_back(st, 1);
        (*iter) = new_bnode(B_LIST);
        (*iter)->l_item = parse_bcode(st);
        iter = &(*iter)->l_next;
    }

    if (END != ch) {
        error_unexpected_char(st, ch, END);
    }

    bnode->start = start;
    bnode->end = st->curr;
    return bnode;
}

/**
 * @brief 解析 B 编码
 *
 * 会向前看一个字节递归下降地进行解析。
 *
 * @param st parser 状态
 * @return 动态分配的B编码语法结点
 */
static struct BNode *
parse_bcode(struct State *st)
{
    char ch = parse_get_char(st);

    if (EOF == ch) {
        panic("ERROR: unexpected end of file at %lu", pos(st));
    }

    switch (ch) {
    case LEAD_INT:   // i<int>e
        return parse_bcode_is_int(st);
    case LEAD_LIST:  // l<bcode>+e
        return parse_bcode_is_list(st);
    case LEAD_DICT:  // d[<key><value>]+e
        return parse_bcode_is_dict(st);
    default:         // <int>:<str>
        parse_back(st, 1);
        return parse_bcode_is_str(st);
    }
}

/**
 * @brief 解析 B 编码
 *
 * 顶层封装，进行错误检查以及屏蔽私有结构体。
 *
 * 用户负责释放抽象语法树。
 *
 * @param bcode 源缓冲区
 * @return 动态生成的语法树根结点
 */
struct BNode *
bparser(char *bcode)
{
    if (bcode == NULL) {
        return NULL;
    }

    struct State st = {
        .start = bcode,
        .curr = bcode,
    };
    return parse_bcode(&st);
}

/**
 * @brief 释放 B 编码的抽象语法树
 * @param pbnode 指向要释放的抽象语法树的根结点
 */
void
free_bnode(struct BNode **pbnode)
{
    struct BNode *bnode = *pbnode;
    *pbnode = NULL;

    if (bnode->type == B_DICT) {
        struct BNode *dict = bnode;
        while (dict) {
            free(dict->d_key);
            free_bnode(&dict->d_val);
            typeof(dict) temp = dict->d_next;
            free(dict);
            dict = temp;
        }
    }
    else if (bnode->type == B_LIST) {
        struct BNode *list = bnode;
        while (list) {
            free_bnode(&list->l_item);
            typeof(list) temp = list->l_next;
            free(list);
            list = temp;
        }
    }
    else if (bnode->type == B_STR) {
        free(bnode->s_data);
        free(bnode);
    }
    else {
        free(bnode);
    }
}
