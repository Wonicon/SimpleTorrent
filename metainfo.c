/**
 * @file metainfo.c
 * @brief 操作全局信息的相关 API 实现
 */

#include "metainfo.h"
#include "bparser.h"
#include "butil.h"
#include "peer.h"
#include "connect.h"
#include "util.h"
#include <string.h>
#include <openssl/sha.h>
#include <netinet/in.h>

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

    const struct BNode *announce_list = query_bcode_by_key(ast, "announce-list");

    if (!announce_list) {
        // 没有 announce-list, 那么就使用 announce
        const struct BNode *announce = query_bcode_by_key(ast, "announce");
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

        log("%lu trackers", mi->nr_trackers);

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
metainfo_load_file(struct MetaInfo *mi, const struct BNode *ast)
{
    const struct BNode *name = query_bcode_by_key(ast, "name");

    log("filename: %s", name->s_data);

    FILE *fp = fopen(name->s_data, "rb");

    if (fp != NULL) {  // 已有下载文件，检查分片 SHA1
        uint8_t *piece = malloc(mi->piece_size);  // piece data buffer
        uint8_t md[HASH_SIZE];  // sha1 buffer

        size_t nr_read;
        int piece_index = 0;
        int correct_piece_count = 0;
        while ((nr_read = fread(piece, 1, mi->piece_size, fp)) != 0) {  // EOF 退出，容许不足
            SHA1(piece, nr_read, md);

            printf("piece %d: %lu bytes", piece_index, nr_read);

            if (memcmp(md, mi->pieces[piece_index].hash, HASH_SIZE) == 0) {  // 分片正确
                mi->pieces[piece_index].is_downloaded = 1;
                correct_piece_count++;
                mi->downloaded += nr_read;
                set_bit(mi->bitfield, piece_index);
                printf(" ok");
            }

            printf("\n");

            piece_index++;
        }

        if (correct_piece_count == mi->nr_pieces) {
            log("file has been downloaded");
            mi->file = fp;
            return;
        }
        else {  // 有不正确的分片，或者文件不完整，重新以可写方式打开。
            fclose(fp);
            mi->file = fopen(name->s_data, "rb+");  // read, write, no trunc
        }
    }
    else {  // 没有下载文件，放心 trunc
        mi->file = fopen(name->s_data, "wb");
    }

    mi->left = mi->file_size - mi->downloaded;
}

void
extract_pieces(struct MetaInfo *mi, const struct BNode *ast)
{
    // 提取分片信息

    const struct BNode *length_node = query_bcode_by_key(ast, "length");
    if (length_node) {
        mi->file_size = (size_t)length_node->i;
    }

    const struct BNode *piece_length_node = query_bcode_by_key(ast, "piece length");
    if (piece_length_node) {
        mi->piece_size = (uint32_t)piece_length_node->i;
    }

    if (mi->file_size && mi->piece_size) {
        mi->nr_pieces = (mi->file_size - 1) / mi->piece_size + 1;
        mi->bitfield_size = (mi->nr_pieces - 1) / 8 + 1;
        mi->sub_size = 0x4000;
        mi->sub_count = (mi->piece_size - 1) / mi->sub_size + 1;

        mi->bitfield = calloc(mi->bitfield_size, 1);
    }

    log("filesz %ld, piecesz %d, nr pieces %lu, bitfield len %lu",
            mi->file_size, mi->piece_size, mi->nr_pieces, mi->bitfield_size);

    log("sub_size %d, sub_count %lu", mi->sub_size, mi->sub_count);

    const struct BNode *pieces_node = query_bcode_by_key(ast, "pieces");
    if (pieces_node) {
        mi->pieces = calloc(mi->nr_pieces, sizeof(*mi->pieces));
        const char *hash = pieces_node->s_data;
        for (int i = 0; i < mi->nr_pieces; i++) {
            memcpy(mi->pieces[i].hash, hash, HASH_SIZE);
            hash += HASH_SIZE;
            // 最后一个分片可能会造成空间冗余，即子分片不足 sub_count, 但是没有副作用。
            mi->pieces[i].substate = calloc(mi->sub_count, sizeof(*mi->pieces[i].substate));
            mi->pieces[i].subtimer = calloc(mi->sub_count, sizeof(*mi->pieces[i].subtimer));
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
 * 被删除的指针空闲没有回收, 在未来增加新
 * peer 时靠 realloc 重新尾部的冗余空间.
 */
void
del_peer_by_fd(struct MetaInfo *mi, int fd)
{
    struct Peer *peer = NULL;
    struct Peer **peers = mi->peers;
    int i, n = mi->nr_peers;

    if (n == 0) {
        return;
    }

    for (i = 0; i < n; i++) {
        peer = peers[i];
        if (fd == peer->fd) {
            break;
        }
    }

    if (peer != NULL) {
        free(peer);
    }

    if (i < mi->nr_peers -1) {
        // 只把要删除的 peer 的后面的 peers 往前拷贝一个。数量计算如下：
        // |[i + 1, n - 1]| = (n - 1) - (i + 1) + 1 = n - i - 1;
        memmove(peers + i, peers + i + 1, sizeof(*peers) * (n - i - 1));
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

/**
 * 参数的 addr 和 port 是网络字节序。
 * peer 自己的 addr 在构造时是网络字节序，但是 port 是本机字节序。
 */
struct Peer *
get_peer_by_addr(struct MetaInfo *mi, uint32_t addr, uint16_t port)
{
    port = ntohs(port);
    for (int i = 0; i < mi->nr_peers; i++) {
        struct Peer *peer = mi->peers[i];
        if (addr == mi->peers[i]->addr && port == mi->peers[i]->port) {
            return peer;
        }
    }

    return NULL;
}

int
check_substate(struct MetaInfo *mi, int index)
{
    size_t sub_cnt = (index != mi->nr_pieces - 1) ? mi->sub_count :
        (((mi->file_size % mi->piece_size) - 1) / mi->sub_size + 1);

    int is_finished = 1;
    char ch;
    for (int i = 0; i < sub_cnt; i++) {
        switch (mi->pieces[index].substate[i]) {
        case SUB_NA:       ch = 'X'; is_finished = 0; break;
        case SUB_DOWNLOAD: ch = 'O'; is_finished = 0; break;
        case SUB_FINISH:   ch = '.'; break;
        default:           ch = '#'; break;
        }
        putchar(ch);
    }
    putchar('\n');

    return is_finished;
}

struct Tracker *
get_tracker_by_fd(struct MetaInfo *mi, int sfd)
{
    if (sfd == -1) {
        return NULL;
    }

    for (int i = 0; i < mi->nr_trackers; i++) {
        if (mi->trackers[i].sfd == sfd) {
            return &mi->trackers[i];
        }
    }

    return NULL;
}

struct Tracker *
get_tracker_by_timer(struct MetaInfo *mi, int timerfd)
{
    for (int i = 0; i < mi->nr_trackers; i++) {
        if (mi->trackers[i].timerfd == timerfd) {
            return &mi->trackers[i];
        }
    }

    return NULL;
}

void
add_wait_peer(struct MetaInfo *mi, int fd, uint32_t addr, uint16_t port, int direction)
{
    mi->wait_peers = realloc(mi->wait_peers, (mi->nr_wait_peers + 1) * sizeof(struct WaitPeer));
    struct WaitPeer *p = &mi->wait_peers[mi->nr_wait_peers];
    p->fd = fd;
    p->addr = addr;
    p->port = port;
    p->direction = direction;
    mi->nr_wait_peers++;
}

int
get_wait_peer_index_by_fd(struct MetaInfo *mi, int fd)
{
    for (int i = 0; i < mi->nr_wait_peers; i++) {
        if (mi->wait_peers[i].fd == fd) {
            return i;
        }
    }
    return -1;
}

int
get_wait_peer_fd(struct MetaInfo *mi, uint32_t addr, uint16_t port, int direction)
{
    for (int i = 0; i < mi->nr_wait_peers; i++) {
        struct WaitPeer *p = &mi->wait_peers[i];
        if (p->addr == addr && (p->port == port || p->direction != direction)) {
            return p->fd;
        }
    }

    return -1;
}

/**
 * 如果要删除的元素刚好是最后一个，只需要减少总数量就行了。
 */
void
rm_wait_peer(struct MetaInfo *mi, int index)
{
    printf("\n");
    if (index < mi->nr_wait_peers - 1) {
        memmove(mi->wait_peers + index, mi->wait_peers + index + 1,
               sizeof(struct WaitPeer) * (mi->nr_wait_peers - index - 1));
    }
    mi->nr_wait_peers--;
}
