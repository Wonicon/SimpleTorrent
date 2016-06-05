#include "bparser.h"
#include "util.h"
#include <string.h>

#define DELIM     ':'
#define LEAD_INT  'i'
#define LEAD_LIST 'l'
#define LEAD_DICT 'd'
#define END       'e'

/**
 * @brief parser 状态
 * 
 * 内部使用，用来记录当前 parsing 的状态，即字符指针的位置。
 */
struct state
{
    char *start;
    char *curr;
};

/**
 * @brief 从数据流中取出一个长整型
 * @param st 指向状态记录
 * @return 返回匹配的长整型
 */
static inline size_t
parse_get_int(struct state *st)
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
parse_get_str(struct state *st, size_t length)
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
parse_get_char(struct state *st)
{
    return *st->curr++;
}

/**
 * @brief 退回 N 个字节
 * @param st 指向状态记录
 * @param n 回退字节数
 */
static inline void
parse_back(struct state *st, int n)
{
    st->curr -= n;
}

/**
 * @brief 获取当前数据流位置
 * @param st 指向状态记录
 * @return 位置
 */
static inline size_t
pos(struct state *st)
{
    return st->curr - st->start;
}

/**
 * @brief 较为常用的错误类型
 */
#define error_unexpected_char(st, ch, expect) panic("ERROR: unexpected '%c', expect '%c' at %lu", ch, expect, pos(st));

static struct BNode *
new_bnode(enum BNodeType type)
{
    struct BNode *bnode = calloc(1, sizeof(*bnode));
    bnode->type = type;
    return bnode;
}

static struct BNode *parse_bcode(struct state *st);

static char *
parse_bcode_is_key(struct state *st, size_t *len_o)
{
    size_t len = parse_get_int(st);

    char delim = parse_get_char(st);
    if (DELIM != delim) {
        error_unexpected_char(st, delim, DELIM);
    }

    if (len_o) *len_o = len;

    return parse_get_str(st, len);
}

static struct BNode *
parse_bcode_is_str(struct state *st)
{
    struct BNode *bnode = new_bnode(B_STR);
    bnode->start = st->curr;
    bnode->s_data = parse_bcode_is_key(st, &bnode->s_size);
    bnode->end = st->curr;
    return bnode;
}

// assume caller consumes the leading 'i'
static struct BNode *
parse_bcode_is_int(struct state *st)
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

// assume caller consumes the leading 'd'
static struct BNode *
parse_bcode_is_dict(struct state *st)
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

// assume caller consumes the leading 'l'
static struct BNode *
parse_bcode_is_list(struct state *st)
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

static struct BNode *
parse_bcode(struct state *st)
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
 * @brief 解析 torrent 字节串，获取抽象语法树
 * @param bcode 指向 torrent 文件的完整数据
 * @return 解析完成的抽象语法树根结点
 *
 * 用户负责释放抽象语法树
 */
struct BNode *
bparser(char *bcode)
{
    struct state st = {
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
