#include "peer.h"
#include "util.h"
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <assert.h>

/**
 * len 不应为 0, keep-alive 由上层检查
 */
struct PeerMsg *
peer_get_packet(int fd)
{
    int len = 0;
    ssize_t s = read(fd, &len, 4);
    if (s < 0) {
        perror("read phase 1");
        return NULL;
    }
    else if (s == 0) {
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        getpeername(fd, (struct sockaddr *)&addr, &addrlen);
        log("%s:%d disconnected at recv pkt phase 1", inet_ntoa(addr.sin_addr), addr.sin_port);
        return NULL;
    }
    else {
        log("read %zd bytes", s);
    }

    assert(s == 4);

    len = htonl(len);

    log("len %d", len);
    struct PeerMsg *msg = malloc(4 + len);
    msg->len = len;

    if (len == 0) {  // KEEP-ALIVE
        return msg;
    }

    s = 0;
    while (s < len) {
        s += read(fd, (char *)&msg->id + s, len - s);
        if (s < 0) {
            perror("read phase 2");
            free(msg);
            return NULL;
        }
        else if (s == 0) {
            struct sockaddr_in addr;
            socklen_t addrlen = sizeof(addr);
            getpeername(fd, (struct sockaddr *)&addr, &addrlen);
            log("%s:%d disconnected at recv pkt phase 2", inet_ntoa(addr.sin_addr), addr.sin_port);
            free(msg);
            return NULL;
        }
        else {
            log("read %zd bytes", s);
        }
    }

    if (msg->id == BT_BITFIELD) {
        log("bitfield received size: %d", len - 1);
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
    int bitfield_capacity = (nr_pieces - 1) / 8 + 1;  // upbound
    log("bitfield capacity %d", bitfield_capacity);
    p->bitfield = calloc(bitfield_capacity, sizeof(*p->bitfield));
    return p;
}

void
peer_free(struct Peer **p)
{
    struct Peer *peer = *p;
    *p = NULL;

    free(peer->bitfield);
    if (peer->requested_pieces) free(peer->requested_pieces);
    if (peer->requested_subpieces) free(peer->requested_subpieces);
    free(peer);
}

/**
 * @brief 获取位域的字节内偏移
 * @param off 位偏移
 * @return 字节掩码, 独热
 */
static inline unsigned
mask_of_(unsigned off)
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
 * @return 字节下表
 */
static inline int
index_of_(unsigned off)
{
    return off >> 3;
}
void
set_bit(unsigned char *buf, unsigned bit_off)
{
    int idx = index_of_(bit_off);
    buf[idx] |= mask_of_(bit_off);
}

void
clr_bit(unsigned char *buf, unsigned bit_off)
{
    int idx = index_of_(bit_off);
    buf[idx] &= ~(mask_of_(bit_off));
}

unsigned char
get_bit(unsigned char *buf, unsigned bit_off)
{
    int idx = index_of_(bit_off);
    int off = 7 - (bit_off & 0x7);
    return (buf[idx] & mask_of_(bit_off)) >> off;
}

void
peer_set_bit(struct Peer *p, unsigned off)
{
    set_bit(p->bitfield, off);
}

void
peer_clr_bit(struct Peer *p, unsigned off)
{
    clr_bit(p->bitfield, off);
}

unsigned char
peer_get_bit(struct Peer *p, unsigned off)
{
    return get_bit(p->bitfield, off);
}
