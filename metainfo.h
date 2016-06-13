/**
 * @file metainfo.h
 * @brief 操作全局信息的相关 API 声明
 */

#ifndef METAINFO_H
#define METAINFO_H

#include <stdio.h>
#include <time.h>
#include <inttypes.h>

/**
 * @brief SHA1 HASH 的字节数
 */
#define HASH_SIZE 20

struct BNode;

/** @brief 描述 tracker 的相关信息 */
struct Tracker
{
    char method[10];        ///< 协议类型 http | udp
    char host[128];         ///< 主机名（域名）
    char port[10];          ///< 端口（默认 80）
    char request[128];      ///< 请求 url （一般是 /announce, 默认 / ）
    int sfd;                ///< socket file descriptor, 默认为 -1. 主要用于搜索, 会频繁重置.
    int timerfd;            ///< 定时器描述符，用于在 epoll 里处理定时事件。
};

/** 子分片没有开始下载 */
#define SUB_NA 0
/** 子分片下载中 */
#define SUB_DOWNLOAD 1
/** 子分片完成下载 */
#define SUB_FINISH 2
/** 子分片最长等待时间 10s */
#define WAIT_THRESHOLD 10.0

/**
 * @brief 分片信息
 *
 * hash 在一开始构造 metainfo 时从 B 编码树上获取并记录。
 * nr_owners 在处理 bitfield 和 have 报文时进行更新。
 *
 * 使用文件作为保存数据的临时空间，主要出于内存消耗以及
 * 断点续传的考虑，但是计算 hash 不是很方便。
 *
 * @todo 子分片使用何种数据结构比较合理？
 */
struct PieceInfo
{
    unsigned char  hash[HASH_SIZE]; ///< 该分片的 SHA1 摘要。
    int            nr_owners;       ///< 该分片拥有者的数量。
    int            is_downloaded;   ///< 标记该分片是否已经完成下载：1 - 已下载，0 - 未完成。

    unsigned char *substate;        ///< 标记子分片完成情况： SUB_NA - 未下载，SUB_DOWNLOAD - 下载中，SUB_FINISH - 下载完成。
    time_t        *subtimer;        ///< 标记子分片下载等待时间
};

/**
 * @brief 描述处于等待握手状态的 peer 信息
 *
 * 用于记录那些已经发送 / 收到连接请求但是还没有完成握手信息的 peer,
 * 由这个结构体构成的队列将用于回避对同一个 peer 的重复连接。
 */
struct WaitPeer
{
    int fd;               ///< 尚未完成连接或握手的套接字
    union {
        uint32_t addr;    ///< ip 地址，方便比较的形式
        uint8_t ip[4];    ///< ip 地址，方便打印的形式
    };
    uint16_t port;        ///< 端口号，网络字节序
    int direction;        ///< 0: 我方主动连接, 1: 对方主动连接。
};

/**
 * @brief 描述一次运行的全局信息
 *
 * 在结构体中记录侦听套接字 listen_fd, 以方便在
 * epoll 中根据 event 的套接字判断具体事件。
 */
struct MetaInfo
{
    size_t file_size;                   ///< 数据文件大小
    size_t downloaded;                  ///< 已完成文件大小
    size_t left;                        ///< 未完成文件大小
    size_t uploaded;                    ///< 上传文件大小
    FILE *file;                         ///< 下载文件
    unsigned char info_hash[HASH_SIZE]; ///< 整个 info 字典的 sha1 摘要

    uint32_t piece_size;                ///< 分片大小
    size_t nr_pieces;                   ///< 分片数量，由 file_size 和 piece_size 计算得出，上取整
    size_t bitfield_size;               ///< bitfield 的字节大小，即 nr_pieces / 8 上取整
    uint32_t sub_size;                  ///< 子分片的大小，使用统一大小的子分片以简化实现
    size_t sub_count;                   ///< 子分片的数量
    struct PieceInfo *pieces;           ///< 分片信息数组
    uint8_t *bitfield;                  ///< 分片完成情况位图
    uint8_t peer_id[21];                ///< random-generated peer-id, the extra 21th byte is '\0' used by host.

    unsigned short port;                ///< 侦听端口
    int listen_fd;                      ///< 侦听套接字
    int timerfd;                        ///< 发送 KEEP-ALIVE 的定时器
    int nr_peers;                       ///< peers 数组的大小 == 已握手 peer 的数量
    struct Peer **peers;                ///< 已握手 peer 的集合
    int nr_wait_peers;                  ///< 已发出 connect 的 peer 数量
    struct WaitPeer *wait_peers;        ///< 已发出 connect 的 peer 集合
    size_t nr_trackers;                 ///< tracker 数量
    struct Tracker *trackers;           ///< tracker 数组
};

/** @brief 释放全局信息 */
void free_metainfo(struct MetaInfo **pmi);

/**
 * @brief 提取 tracker 列表
 * @param mi 全局信息
 * @param ast B 编码语法树
 */
void extract_trackers(struct MetaInfo *mi, const struct BNode *ast);

/** @brief 获取文件名，读取文件，分析已经完成的块 */
void metainfo_load_file(struct MetaInfo *mi, const struct BNode *ast);

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

/**
 * @brief 根据网络地址搜索 peer
 * @param mi 全局信息
 * @param addr ip 地址，网络字节序
 */
struct Peer *get_peer_by_addr(struct MetaInfo *mi, uint32_t addr);

/**
 * @brief 检查某一分片的子分片状态并打印
 *
 * 打印符号说明：
 *   1. . 已完成
 *   2. 0 正下载
 *   3. X 未下载
 *
 * @param mi 全局信息
 * @param index 分片号
 * @return 如果全部子分片完成返回 1, 否则返回 0.
 */
int check_substate(struct MetaInfo *mi, int index);

/**
 * @brief 根据连接套接字找 tracker
 *
 * 适用于第一手信息是套接字的场合: epoll
 *
 * @param mi 全局信息
 * @param sfd 连接套接字
 * @return 对应的 tracker 指针，没找到返回 NULL
 */
struct Tracker *get_tracker_by_fd(struct MetaInfo *mi, int sfd);

/**
 * @brief 根据定时器描述符找 tracker
 *
 * 定时事件是一个低频事件，所以这个函数在 EPOLLIN 事件里排在最后
 *
 * @param mi 全局信息
 * @param timerfd 定时器描述符
 * @return 对应的 tracker 指针，没找到返回 NULL
 */
struct Tracker *get_tracker_by_timer(struct MetaInfo *mi, int timerfd);

/** @brief 添加等待 peer, 网络字节序 */
void add_wait_peer(struct MetaInfo *mi, int fd, uint32_t addr, uint16_t port, int direction);

/**
 * @brief 根据套接字找到 peer 下标
 *
 * @return 对应的下标，没找到则 -1.
 */
int get_wait_peer_index_by_fd(struct MetaInfo *mi, int fd);

/**
 * @brief 根据地址找到 peer 的套接字，网络字节序
 *
 * @param mi global info
 * @param addr ip address
 *
 * @return 对应的套接字，没找到则 -1.
 */
int get_wait_peer_fd(struct MetaInfo *mi, uint32_t addr);

/** @brief 删除等待 peer */
void rm_wait_peer(struct MetaInfo *mi, int index);

#endif  // METAINFO_H
