/**
 * @file peer.h
 * @brief 单个 peer 相关操作 API 声明
 */

#ifndef PEER_H
#define PEER_H

#include "metainfo.h"

/**
 * @brief 握手信息要求的字符串
 */
#define PSTR_DEFAULT "BitTorrent protocol"

/**
 * @brief #PSTR_DEFAULT 对应的长度，握手信息要求。
 */
#define PSTRLEN_DEFAULT (sizeof(PSTR_DEFAULT) - 1)

#pragma pack(1)
/**
 * @brief 握手信息
 */
typedef struct {
    uint8_t hs_pstrlen;               ///< BitTorrent 1.0: 19
    char    hs_pstr[19];              ///< BitTorrent 1.0: "BitTorrent protocol"
    uint8_t hs_reserved[8];           ///< 保留区域
    uint8_t hs_info_hash[HASH_SIZE];  ///< info 字典的 SHA1 hash
    char    hs_peer_id[HASH_SIZE];    ///< peer 的名称
} PeerHandShake;

enum {
    BT_CHOKE,
    BT_UNCHOKE,
    BT_INTERESTED,
    BT_NOT_INTERESTED,
    BT_HAVE,
    BT_BITFIELD,
    BT_REQUEST,
    BT_PIECE,
    BT_CANCEL,
};

/**
 * @brief 对应 BT 报文类型的字符串
 */
extern const char *bt_types[];

/**
 * @brief BT 消息
 *
 * 构造模式
 * @code
 * uint32_t len;
 * read(fd, &len, sizeof(len));
 * PeerMsg *msg = malloc(len);
 * read(fd, &msg->id, len);
 * msg->len = len;
 * @endcode
 *
 * len 大部分时候是不需要的，但是可边长的 bitfield 和 block 报文自身不带长度，只能靠 len 计算得出。
 */
struct PeerMsg {
    uint32_t len;                  ///< 后续消息长度（不包含 len 自身的 4 个字节）
    uint8_t id;                    ///< 消息编号
    union {
        struct {
            uint32_t piece_index;  ///< 分片号
        } have;                    ///< HAVE 消息

        uint8_t bitfield[0];       ///< 位域消息，变长

        struct {
            uint32_t index;        ///< 分片号
            uint32_t begin;        ///< 子分片起始偏移量
            uint32_t length;       ///< 子分片长度
        } request;                 ///< REQUEST 消息

        struct {
            uint32_t index;        ///< 分片号
            uint32_t begin;        ///< 子分片起始偏移量
            uint8_t block[0];      ///< 边长数据块
        } piece;                   ///< PIECE 消息

        struct {
            uint32_t index;        ///< 分片号
            uint32_t begin;        ///< 子分片起始偏移量
            uint32_t length;       ///< 子分片长度
        } cancel;                  ///< CANCEL 消息
    };
};
#pragma pack()

/**
 * @brief 描述 peer 信息
 *
 * peer 只记录单次分片请求的信息。因为对单个连接上的数据传输，没有并发的可能，
 * 只能顺序地读取，所以较为稳定的一次请求一次读取的操作，虽然简单，也是合理的。
 * @note 请求报文的传播延迟无法重叠，还是会有效率上的损失。
 */
struct Peer
{
    int fd;                   ///< 连接套接字
    char ip[16];              ///< ip 地址字符串, 最长不过 |255.255.255.255| + '\0' = 16
    unsigned short port;      ///< 端口, 本地字节序
    uint32_t addr;            ///< ip 的整数形式，用于地址比较，直接来自 sin_addr，故是网络字节序
    unsigned char *bitfield;  ///< piece 拥有情况
    int is_choked;            ///< 是否阻塞 peer
    int is_interested;        ///< 是否对 peer 感兴趣
    int get_choked;           ///< 是否被 peer 阻塞
    int get_interested;       ///< peer 是否感兴趣
    int *requested_pieces;    ///< -1 terminated
    int *requested_subpieces; ///< -1 terminated
    int requesting_index;     ///< 请求的分片号，-1 为无效。
    int requesting_begin;     ///< 请求的子分片偏移量（固定长度）
    int contribution;         ///< 检查周期内的数据贡献
    unsigned wanted;          ///< 期望接受的字节数
    struct PeerMsg *msg;      ///< 记录尚未读完的 msg
    struct timespec st;       ///< 记录发送请求的时刻，用于计算分片下载速度，不使用分片的 time 是因为后者可能因超时被更新。
    double speed;             ///< 下载速度
};

/**
 * @brief 获取 BT 报文
 * @param peer 指向 peer 对象
 * @return 输出动态分配的指向 packet 的指针
 */
struct PeerMsg *peer_get_packet(struct Peer *peer);

/**
 * @brief peer 构造
 * @param fd 连接套接字
 * @param nr_pieces 分片大小
 * @return 动态分配的 peer 指针
 */
struct Peer *peer_new(int fd, size_t nr_pieces);

/**
 * @brief 释放 peer
 * @param peer 要释放的 peer, 会改写成 NULL
 */
void peer_free(struct Peer **peer);

/**
 * @brief 向标准输出打印 bitfield
 * @param bytes bitfield 缓冲区
 * @param bit_len bit 数, 一般对应分片数量
 */
void print_bit(unsigned char *bytes, size_t bit_len);

/**
 * @brief 设置 bit
 * @param bytes bit field
 * @param bit_offset bit 偏移
 */
void set_bit(unsigned char *bytes, unsigned bit_offset);

/**
 * @brief 设置 peer 的位域
 * @param peer 要设置的 peer
 * @param bit_offset 位偏移量
 */
void peer_set_bit(struct Peer *peer, unsigned bit_offset);

/**
 * @brief 获取 peer 的位域
 * @param peer 要获取的 peer
 * @param bit_offset 位偏移量
 */
unsigned char peer_get_bit(struct Peer *peer, unsigned bit_offset);

/** @brief 向 peer 发送 BT 消息
 *
 * 提前构造好 msg 并将其发送给指定的 peer，
 * 以 msg 的 len 成员为准发送缓冲区，调用者保证其正确性。
 *
 * @param peer 要发送消息的 peer
 * @param msg 发送的消息
 */
void peer_send_msg(struct Peer *peer, struct PeerMsg *msg);

#endif  // PEER_H
