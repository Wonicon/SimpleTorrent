#include "peer.h"
#include "util.h"
#include <stdlib.h>
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
    int len;
    ssize_t s = recv(peer->fd, &len, 4, MSG_WAITALL);
    len = htonl(len);
    if (s < 0) {
        perror("read phase 1");
        return NULL;
    }
    else if (s == 0) {
        log("%s:%u disconnected at recv pkt phase 1", peer->ip, peer->port);
        return NULL;
    }

    assert(s == 4);

    struct PeerMsg *msg = malloc(4 + len);
    msg->len = len;

    if (len == 0) {  // KEEP-ALIVE
        log("KEEP_ALIVE");
        return msg;
    }

    s = recv(peer->fd, &msg->id, len, MSG_WAITALL);
    if (s < 0) {
        perror("read phase 2");
        free(msg);
        return NULL;
    }
    else if (s == 0) {
        log("%s:%u disconnected at recv pkt phase 2", peer->ip, peer->port);
        free(msg);
        return NULL;
    }
    else {
        log("recv %s (%zd bytes)", bt_types[msg->id], s);
    }

    return msg;
}

/**
 * bitfield 的大小是对 nr_pieces / 8 的上取整.
 */
struct Peer *
peer_new(int fd, int nr_pieces)
{
    struct Peer *p = calloc(1, sizeof(*p));
    p->fd = fd;

    // 记录 ip 和端口以减少冗余操作
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    getpeername(fd, (struct sockaddr *)&addr, &addrlen);
    strcpy(p->ip, inet_ntoa(addr.sin_addr));
    p->port = ntohs(addr.sin_port);

    int bitfield_capacity = (nr_pieces - 1) / 8 + 1;  // upbound
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
in_byte_mask_of_(unsigned off)
{
    /**
     * 8 元组内, 最高位是对应组内偏移 0.
     */
    off = off & 0x7;
    return (1 << (7 - off));
}

/**
 * @brief 获取位域的字节
 * @param off 位偏移
 * @return 字节下标
 */
static inline int
byte_index_of_(unsigned off)
{
    return off >> 3;
}

/**
 * @brief 获取位域的字节内偏移
 * @param off 位域偏移
 * @return 字节内偏移
 *
 * BitTorrent 的规定与常规思维不同, MSB 是 [0], LSB 是 [7],
 * 所以要用 7 减去与出来的值.
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

void
clr_bit(unsigned char *bytes, unsigned bit_offset)
{
    int byte_index = byte_index_of_(bit_offset);
    bytes[byte_index] &= ~(in_byte_mask_of_(bit_offset));
}

unsigned char
get_bit(unsigned char *bytes, unsigned bit_offset)
{
    int byte_index = byte_index_of_(bit_offset);
    int in_byte_offset = in_byte_offset_of_(bit_offset);
    return (bytes[byte_index] & in_byte_mask_of_(bit_offset)) >> in_byte_offset;
}

void
peer_set_bit(struct Peer *peer, unsigned bit_offset)
{
    set_bit(peer->bitfield, bit_offset);
}

void
peer_clr_bit(struct Peer *peer, unsigned bit_offset)
{
    clr_bit(peer->bitfield, bit_offset);
}

unsigned char
peer_get_bit(struct Peer *peer, unsigned bit_offset)
{
    return get_bit(peer->bitfield, bit_offset);
}
