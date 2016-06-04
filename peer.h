/**
 * @file peer.h
 * @brief 单个 peer 相关操作 API 声明
 */

#ifndef PEER_H
#define PEER_H

#include "metainfo.h"
#include <inttypes.h>

#define PSTR_DEFAULT "BitTorrent protocol"
#define PSTRLEN_DEFAULT (sizeof(PSTR_DEFAULT) - 1)

struct PeerMsg;

/** @brief 描述 peer 信息
 *
 * peer 只记录单次分片请求的信息。因为对单个连接上的数据传输，没有并发的可能，
 * 只能顺序地读取，所以较为稳定的一次请求一次读取的操作，虽然简单，也是合理的。
 */
struct Peer
{
    int fd;                   ///< 连接套接字
    char ip[16];              ///< ip 地址字符串, 最长不过 |255.255.255.255| + '\0' = 16
    unsigned short port;      ///< 端口, 本地字节序
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
    int wanted;               ///< 期望接受的字节数
    struct PeerMsg *msg;      ///< 记录尚未读完的 msg
};

#pragma pack(1)
typedef struct {
    uint8_t hs_pstrlen;       // BitTorrent 1.0: 19
    char    hs_pstr[19];      // BitTorrent 1.0: "BitTorrent protocol"
    uint8_t hs_reserved[8];
    uint8_t hs_info_hash[HASH_SIZE];
    char    hs_peer_id[HASH_SIZE];
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

// 构造模式：
// uint32_t len;
// read(fd, &len, sizeof(len));
// PeerMsg *msg = malloc(len);
// read(fd, &msg->id, len);
// msg->len = len;
// 可变长度的 bitfield 和 block 还是需要记录长度……
struct PeerMsg {
    uint32_t len;
    uint8_t id;
    union {
        struct {
            uint32_t piece_index;
        } have;

        uint8_t bitfield[0];

        struct {
            uint32_t index;
            uint32_t begin;
            uint32_t length;
        } request;

        struct {
            uint32_t index;
            uint32_t begin;
            uint8_t block[0];
        } piece;

        struct {
            uint32_t index;
            uint32_t begin;
            uint32_t length;
        } cancel;
    };
};
#pragma pack()

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
struct Peer *peer_new(int fd, int nr_pieces);

/**
 * @brief 释放 peer
 */
void peer_free(struct Peer **peer);

/**
 * @brief 向标准输出打印 bitfield
 * @param bytes bitfield 缓冲区
 * @param bit_len bit 数, 一般对应分片数量
 */
void print_bit(unsigned char *bytes, unsigned bit_len);

void set_bit(unsigned char *bytes, unsigned bit_offset);

unsigned char get_bit(unsigned char *bytes, unsigned bit_offset);

void peer_set_bit(struct Peer *peer, unsigned bit_offset);

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
