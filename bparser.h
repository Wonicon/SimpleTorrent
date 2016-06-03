#ifndef BPARSER_H
#define BPARSER_H

struct BNode;

// 解析 B 编码数据获取抽象语法树
struct BNode *bparser(char *bcode);

// 释放 B 编码的抽象语法树
void free_bnode(struct BNode **pbnode);

/*
 * <bcode> : <str>
 *         | i <int> e
 *         | l <bcode>+ e
 *         | d [<str><bcode>]+ e
 *         ;
 * 
 * <str> : <int>:<chars>
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

struct BNode
{
    enum BNodeType type;
    union
    {
        struct {
            struct BNode *l_item;
            struct BNode *l_next;
        };
        struct {
            char *d_key;
            struct BNode *d_val;
            struct BNode *d_next;
        };
        struct {
            long s_size;
            char *s_data;
        };
        long i;
    };
    char *start;
    char *end;
};

#endif //  BPARSER_H
