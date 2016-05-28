#ifndef PEER_H
#define PEER_H

#include <inttypes.h>

#define PSTR_DEFAULT "BitTorrent protocol"
#define PSTRLEN_DEFAULT (sizeof(PSTR_DEFAULT) - 1)

#pragma pack(0)
typedef struct {
    uint8_t hs_pstrlen;       // BitTorrent 1.0: 19
    char    hs_pstr[19];      // BitTorrent 1.0: "BitTorrent protocol"
    uint8_t hs_reserved[8];
    uint8_t hs_info_hash[20];
    char    hs_peer_id[20];
} PeerHandShake;

// 构造模式：
// uint32_t len;
// read(fd, &len, sizeof(len));
// PeerMsg *msg = malloc(len);
// read(fd, &msg->id, len);
// msg->len = len;
// 可变长度的 bitfield 和 block 还是需要记录长度……
typedef struct {
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
} PeerMsg;
#pragma pack()

#endif  // PEER_H
