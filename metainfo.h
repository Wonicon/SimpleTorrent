/**
 * @file metainfo.h
 * @brief 操作全局信息的相关 API
 */

#ifndef METAINFO_H
#define METAINFO_H

struct BNode;

/**
 * @brief 描述 tracker 的相关信息
 */
struct Tracker
{
    char method[10];              // 协议类型 http | udp
    char host[128];               // 主机名（域名）
    char port[10];                // 端口（默认 80）
    char request[128];            // 请求 url （一般是 /announce, 默认 / ）
};

/**
 * @brief 描述一次运行的全局信息
 */
struct MetaInfo
{
    int nr_trackers;              // tracker 数量
    struct Tracker *trackers;     // tracker 数组
    long file_size;               // 数据文件大小
    long piece_size;              // 分片大小
    int nr_pieces;                // 分片数量，由上两者计算得出，上取整
    unsigned char (*pieces)[20];  // 分片 hash 数组，每项对应每个分片 sha1 摘要
    unsigned char *bitfield;      // 分片完成情况位图
    unsigned char **subpieces;    // 子分片完成情况
    unsigned char info_hash[20];  // 整个 info 字典的 sha1 摘要
    unsigned short port;          // 侦听端口
    int nr_peers;                 // peers 数组的大小 == 已握手 peer 的数量
    struct Peer **peers;          // 已握手 peer 的集合
};

/**
 * @brief 释放全局信息
 */
void free_metainfo(struct MetaInfo **pmi);

/**
 * @brief 提取 tracker 列表
 * @param mi 全局信息
 * @param ast B 编码语法树
 */
void extract_trackers(struct MetaInfo *mi, const struct BNode *ast);

/**
 * @brief 提取分片 hash
 * @param mi 全局信息
 * @param ast B 编码语法树
 */
void extract_pieces(struct MetaInfo *mi, const struct BNode *ast);

/**
 * @brief 增加一个 peer
 * @param mi 全局信息
 * @param p 要加入的 peer, 要求是动态分配的.
 */
void add_peer(struct MetaInfo *mi, struct Peer *p);

/**
 * @brief 根据连接套接字删除 peer
 * @param mi 全局信息
 * @param fd 连接套接字描述符
 */
void del_peer_by_fd(struct MetaInfo *mi, int fd);

/**
 * @brief 根据连接套接字搜索 peer
 * @param mi 全局信息
 * @param fd 连接套接字描述符
 * @return 找到时返回对应的 peer 指针, 否则返回 NULL.
 *
 * 适用于 epoll 场景, 此时套接字描述符是最先确定的数据.
 */
struct Peer *get_peer_by_fd(struct MetaInfo *mi, int fd);

#endif  // METAINFO_H
