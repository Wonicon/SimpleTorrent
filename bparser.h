#ifndef BPARSER_H
#define BPARSER_H

struct BNode;

struct Tracker
{
    char method[10];
    char host[128];
    char port[10];
    char request[128];
};

struct MetaInfo
{
    long size;                    // torrent 文件大小
    int nr_trackers;              // tracker 数量
    struct Tracker *trackers;     // tracker 数组
    long file_size;               // 数据文件大小
    long piece_size;              // 分片大小
    int nr_pieces;                // 分片数量，由上两者计算得出，上取整
    unsigned char (*pieces)[20];  // 分片 hash 数组，每项对应每个分片 sha1 摘要
    unsigned char info_hash[20];  // 整个 info 字典的 sha1 摘要
};

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
