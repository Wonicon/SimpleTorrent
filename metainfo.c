#include "metainfo.h"
#include "bparser.h"
#include "butil.h"
#include "peer.h"
#include "connect.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
free_metainfo(struct MetaInfo **pmi)
{
    struct MetaInfo *mi = *pmi;
    *pmi = NULL;
    if (mi->trackers) {
        free(mi->trackers);
    }
    if (mi->pieces) {
        free(mi->pieces);
    }
    free(mi);
}

/**
 * 观察实际的种子文件, 发现如果有 announce-list, 那么
 * announce 往往是其中的第一项. 但是 announce-list 本身
 * 不是必须的, 所以在没有 announce-list 是解析 announce
 * 否则直接使用 announce-list 忽略 announce.
 */
void
extract_trackers(struct MetaInfo *mi, const struct BNode *ast)
{
    // 提取 tracker 列表

    const struct BNode *announce_list = dfs_bcode(ast, "announce-list");

    if (!announce_list) {
        // 没有 announce-list, 那么就使用 announce
        const struct BNode *announce = dfs_bcode(ast, "announce");
        mi->nr_trackers = 1;
        mi->trackers = calloc(mi->nr_trackers, sizeof(*mi->trackers));
        parse_url(announce->s_data,
                  mi->trackers[0].method,
                  mi->trackers[0].host,
                  mi->trackers[0].port,
                  mi->trackers[0].request);
    }
    else {
        mi->nr_trackers = 0;

        for (const struct BNode *iter = announce_list; iter; iter = iter->l_next) {
            mi->nr_trackers++;
        }

        log("%d trackers", mi->nr_trackers);

        mi->trackers = calloc(mi->nr_trackers, sizeof(*mi->trackers));
        struct Tracker *tracker = &mi->trackers[0];
        for (const struct BNode *iter = announce_list; iter; iter = iter->l_next) {
            parse_url(iter->l_item->l_item->s_data,
                      tracker->method,
                      tracker->host,
                      tracker->port,
                      tracker->request);
            tracker++;
        }
    }
}

void
extract_pieces(struct MetaInfo *mi, const struct BNode *ast)
{
    // 提取分片信息

    const struct BNode *length_node = dfs_bcode(ast, "length");
    if (length_node) {
        mi->file_size = length_node->i;
    }

    const struct BNode *piece_length_node = dfs_bcode(ast, "piece length");
    if (piece_length_node) {
        mi->piece_size = piece_length_node->i;
    }

    if (mi->file_size && mi->piece_size) {
        mi->nr_pieces = (mi->file_size - 1) / mi->piece_size + 1;
        mi->bitfield_size = (mi->nr_pieces - 1) / 8 + 1;
        mi->sub_size = 0x4000;
        mi->sub_count = (mi->piece_size - 1) / mi->sub_size + 1;
    }

    log("filesz %ld, piecesz %d, nr pieces %d, bitfield len %d",
            mi->file_size, mi->piece_size, mi->nr_pieces, mi->bitfield_size);

    log("sub_size %d, sub_count %d", mi->sub_size, mi->sub_count);

    const struct BNode *pieces_node = dfs_bcode(ast, "pieces");
    if (pieces_node) {
        mi->pieces = calloc(mi->nr_pieces, sizeof(*mi->pieces));
        const char *hash = pieces_node->s_data;
        for (int i = 0; i < mi->nr_pieces; i++) {
            memcpy(mi->pieces[i].hash, hash, HASH_SIZE);
            hash += sizeof(*mi->pieces);
            // 最后一个分片可能会造成空间冗余，即子分片不足 sub_count, 但是没有副作用。
            mi->pieces[i].substate = calloc(mi->sub_count, sizeof(*mi->pieces[i].substate));
        }
    }
}

void
add_peer(struct MetaInfo *mi, struct Peer *p)
{
    mi->peers = realloc(mi->peers, sizeof(*p) * (mi->nr_peers + 1));
    mi->peers[mi->nr_peers] = p;
    mi->nr_peers++;
}

/**
 * 很 low 的完全拷贝法.
 * 被删除的指针空闲没有回收, 在未来增加新
 * peer 时靠 realloc 重新尾部的冗余空间.
 */
void
del_peer_by_fd(struct MetaInfo *mi, int fd)
{
    for (int fast = 0, slow = 0; fast < mi->nr_peers; fast++, slow++) {
        if (fd == mi->peers[fast]->fd) {
            peer_free(&mi->peers[fast]);
            fast++;
        }
        mi->peers[slow] = mi->peers[fast];
    }
    mi->nr_peers--;
}

struct Peer *
get_peer_by_fd(struct MetaInfo *mi, int fd)
{
    for (int i = 0; i < mi->nr_peers; i++) {
        if (mi->peers[i]->fd == fd) {
            return mi->peers[i];
        }
    }
    return NULL;
}

void
print_substate(struct MetaInfo *mi, int index)
{
    int sub_cnt = (index != mi->nr_pieces) ? mi->sub_count :
        (((mi->file_size % mi->piece_size) - 1) / mi->sub_size + 1);

    char ch;
    for (int i = 0; i < sub_cnt; i++) {
        switch (mi->pieces[index].substate[i]) {
        case SUB_NA:       ch = 'X'; break;
        case SUB_DOWNLOAD: ch = 'O'; break;
        case SUB_FINISH:   ch = '.'; break;
        default:           ch = '#'; break;
        }
        putchar(ch);
    }

    putchar('\n');
}
