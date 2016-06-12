/**
 * @file peer.c
 * @brief 单个 peer 相关操作 API 实现
 */

#include "peer.h"
#include "util.h"
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <arpa/inet.h>

const char *bt_types[] =
{
    "CHOKE",
    "UNCHOKE",
    "INTERESTED",
    "NOT_INTERESTED",
    "HAVE",
    "BITFIELD",
    "REQUEST",
    "PIECE",
    "CANCEL",
};

/**
 * len 不应为 0, keep-alive 由上层检查
 */
struct PeerMsg *
peer_get_packet(struct Peer *peer)
{
    ssize_t s;

    if (peer->wanted == 0) {  // 从头开始的一次 BT 报文读取
        // 至少有多长的载荷是一定要读满的
        s = recv(peer->fd, &peer->wanted, 4, MSG_WAITALL);
        peer->wanted = htonl(peer->wanted);
        if (s < 0) {
            perror("read phase 1");
            return NULL;
        }
        else if (s == 0) {
            log("%s:%u disconnected at recv pkt phase 1", peer->ip, peer->port);
            return NULL;
        }

        assert(s == 4);

        peer->msg = malloc(4 + peer->wanted);
        peer->msg->len = peer->wanted;

        if (peer->msg->len == 0) {  // KEEP-ALIVE
            log("KEEP_ALIVE");
            return peer->msg;
        }

        log("want to receive %d bytes payload from %s:%d", peer->wanted, peer->ip, peer->port);
    }

    // 异步连续读取

    s = read(peer->fd, (char *)&peer->msg->id + peer->msg->len - peer->wanted, peer->wanted);
    if (s < 0) {
        perror("read phase 2");
        free(peer->msg);
        return NULL;
    }
    else if (s == 0) {
        log("%s:%u disconnected at recv pkt phase 2", peer->ip, peer->port);
        free(peer->msg);
        return NULL;
    }

    peer->wanted -= s;
    return peer->msg;
}

/**
 * bitfield 的大小是对 nr_pieces / 8 的上取整.
 */
struct Peer *
peer_new(int fd, size_t nr_pieces)
{
    struct Peer *p = calloc(1, sizeof(*p));
    p->fd = fd;

    // 记录 ip 和端口以减少冗余操作
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    getpeername(fd, (struct sockaddr *)&addr, &addrlen);
    strcpy(p->ip, inet_ntoa(addr.sin_addr));
    p->port = ntohs(addr.sin_port);
    p->addr = addr.sin_addr.s_addr;

    // 初始化状态
    p->get_choked = 1;         // 对方一开始不响应我的请求
    p->get_interested = 0;     // 对方不会请求我
    p->is_choked = 0;          // 我要响应对方的请求（虽然目前不会让对方对我感兴趣）
    p->is_interested = 1;      // 我会请求对方
    p->requesting_index = -1;
    p->requesting_begin = -1;
    p->speed = 0.0;

    size_t bitfield_capacity = (nr_pieces - 1) / 8 + 1;  // 上取整
    p->bitfield = calloc(bitfield_capacity, sizeof(*p->bitfield));
    return p;
}

void
peer_free(struct Peer **p)
{
    struct Peer *peer = *p;
    *p = NULL;

    free(peer->bitfield);
    if (peer->requested_pieces) {
        free(peer->requested_pieces);
    }
    if (peer->requested_subpieces) {
        free(peer->requested_subpieces);
    }
    free(peer);
}

/**
 * @brief 获取位域的字节内掩码
 * @param off 位偏移
 * @return 字节掩码, 独热
 */
static inline unsigned
in_byte_mask_of_(size_t off)
{
    // 8 元组内, 最高位是对应组内偏移 0.
    off = off & 0x7;
    return (1U << (7 - off));
}

/** @brief 获取位域的字节
 * @param off 位偏移
 * @return 字节下标
 */
static inline int
byte_index_of_(unsigned off)
{
    return off >> 3;
}

/** @brief 获取位域的字节内偏移
 *
 * BitTorrent 的规定与常规思维不同, MSB 是 [0], LSB 是 [7],
 * 所以要用 7 减去与出来的值.
 *
 * @param bit_offset 位域偏移
 * @return 字节内偏移
 */
static inline int
in_byte_offset_of_(unsigned bit_offset)
{
    return (7 - (bit_offset & 7));
}

void
set_bit(unsigned char *bytes, unsigned bit_offset)
{
    int byte_index = byte_index_of_(bit_offset);
    bytes[byte_index] |= in_byte_mask_of_(bit_offset);
}

/**
 * @brief 获取 bit
 * @param bytes bit field
 * @param bit_offset bit 偏移
 * @return bit 值
 */
static unsigned char
get_bit(unsigned char *bytes, unsigned bit_offset)
{
    int byte_index = byte_index_of_(bit_offset);
    int in_byte_offset = in_byte_offset_of_(bit_offset);
    return (unsigned char)((bytes[byte_index] & in_byte_mask_of_(bit_offset)) >> in_byte_offset);
}

/**
 * @brief 打印一个字节的 bit
 * @param byte bit field
 * @param bit_len 限长（小于 8）
 */
static void
print_bit_in_byte(unsigned char byte, size_t bit_len)
{
    bit_len = (bit_len < 8) ? bit_len : 8;

    for (size_t offset = 0; offset < bit_len; offset++) {
        char ch = (char)((byte & in_byte_mask_of_(offset)) ? '.' : 'X');
        putchar(ch);
    }
}

void
print_bit(unsigned char *bytes, size_t bit_len)
{
    size_t byte_len = (bit_len - 1) / 8 + 1;  // (bit_len / 8) 上取整
    for (size_t i = 0; i < byte_len; i++) {
        print_bit_in_byte(bytes[i], bit_len);
        bit_len -= 8;
    }
}

void
peer_set_bit(struct Peer *peer, unsigned bit_offset)
{
    set_bit(peer->bitfield, bit_offset);
}

unsigned char
peer_get_bit(struct Peer *peer, unsigned bit_offset)
{
    return get_bit(peer->bitfield, bit_offset);
}

void
peer_send_msg(struct Peer *peer, struct PeerMsg *msg)
{
    write(peer->fd, msg, 4 + msg->len);
}
