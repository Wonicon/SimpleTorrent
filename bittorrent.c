/**
 * @file bittorrent.c
 * @brief This module handles bittorrent protocol
 */

#include "butil.h"
#include "util.h"
#include "peer.h"
#include "connect.h"
#include <string.h>
#include <assert.h>
#include <unistd.h>       // read(), write()
#include <errno.h>        // EINPROGRESS
#include <sys/epoll.h>    // epoll_create1(), epoll_ctl(), epoll_wait(), epoll_event
#include <arpa/inet.h>    // inet_ntoa()
#include <sys/timerfd.h>  // timerfd_settime()
#include <openssl/sha.h>  // SHA1()

/**
 * @brief 报文缓冲区大小
 */
#define BUF_SIZE 4096

/**
 * @brief 发送握手信息
 */
void
send_handshake(int sfd, struct MetaInfo *mi)
{
    PeerHandShake handshake = { .hs_pstrlen = PSTRLEN_DEFAULT };
    strncpy(handshake.hs_pstr, PSTR_DEFAULT, PSTRLEN_DEFAULT);
    memset(handshake.hs_reserved, 0, sizeof(handshake.hs_reserved));
    memcpy(handshake.hs_info_hash, mi->info_hash, sizeof(mi->info_hash));
    memcpy(handshake.hs_peer_id, mi->peer_id, HASH_SIZE);

    if (write(sfd, &handshake, sizeof(handshake)) < sizeof(handshake)) {
        perror("handshake");
    }
}

/**
 * 向 tracker 发送 HTTP GET 请求. 可以通过 event 指定发送的具体时间,
 * 其余信息完全通过 mi 填写.
 */
void
send_msg_to_tracker(struct MetaInfo *mi, struct Tracker *tracker)
{
    // 请求头
    struct HttpRequest *req = create_http_request("GET", tracker->request);

    // info_hash
    char infohash[3 * HASH_SIZE + 1] = { 0 };
    char *curr = infohash;
    for (int i = 0; i < HASH_SIZE; i++) {
        curr += sprintf(curr, "%%%02x", mi->info_hash[i]);
    }
    add_http_request_attr(req, "info_hash", "%s", infohash);

    // 获取嵌入请求的侦听端口号
    add_http_request_attr(req, "port", "%d", mi->port);

    // 其他一些请求信息
    add_http_request_attr(req, "peer_id"   , "%s", mi->peer_id);
    add_http_request_attr(req, "uploaded"  , "%ld", mi->uploaded);
    add_http_request_attr(req, "downloaded", "%ld", mi->downloaded);
    add_http_request_attr(req, "left"      , "%ld", mi->left);

    const char *event = NULL;
    if (tracker->timerfd == 0 && mi->left != 0) {
        // This tracker is to be connected at the first time,
        // as we haven't set timer according to its response.
        // It seems we cannot act as a finished peer with start event.
        // It is reasonalbe. If allowed, the completed event will never be sent.
        event = "start";
    }
    else if (mi->downloaded > 0 && mi->left == 0) {
        assert(tracker->timerfd != 0);
        event = "completed";
    }
    else if (mi->downloaded == mi->file_size && mi->left == mi->file_size) {
        // Impossible, provided when invoke SIGINT
        event = "stopped";
    }

    if (event != NULL) {
        add_http_request_attr(req, "event", "%s", event);
    }

    log("send tracker %s:%s%s with event %s", tracker->host, tracker->port, tracker->request, event);
    send_http_request(req, tracker->sfd);
}

/**
 * @brief 向 peer 发送分片请求，同时更新 peer 和对应子分片的大小
 *
 * 修改 peer 状态，表明它处于下载任务中，同时更新起始时刻，用于之后计算速度。
 *
 * 修改子分片状态，表明它正被下载，同时更新起始时刻，用于之后超时检测。
 *
 * msg 报文的本机字节序会转换为网络字节序。
 *
 * @param mi 全局信息
 * @param peer 指向要发送请求的 peer 的指针
 * @param msg 构造好的请求报文，本机字节序
 */
void
send_request(struct MetaInfo *mi, struct Peer *peer, struct PeerMsg *msg)
{
    uint32_t index = msg->request.index;
    uint32_t begin = msg->request.begin;
    uint32_t length = msg->request.length;

    int sub_idx = begin / mi->sub_size;

    // 在 peer 中记录任务信息，用于可能的撤销操作
    peer->requesting_index = index;
    peer->requesting_begin = begin;

    struct PieceInfo *piece = &mi->pieces[index];
    piece->substate[sub_idx] = SUB_DOWNLOAD;
    clock_gettime(CLOCK_BOOTTIME, &peer->st);

    msg->request.index = htonl(index);
    msg->request.begin = htonl(begin);
    msg->request.length = htonl(length);

    if (write(peer->fd, msg, 4 + 13) < 4 + 13) {
        perror("send request");
    }

    log("send %s [index %d begin %d length %d] to %s:%d",
        bt_types[msg->id], index, begin, length, peer->ip, peer->port);
}

/**
 * @brief 选择可以发送请求的 peer
 *
 * @oaram is_busy 所有可用 peer 都在工作中。
 *
 * @return 0 成功发送, 1 还有可用的，2 没有可用的
 */
int
select_peer(struct MetaInfo *mi, struct PeerMsg *msg)
{
    // 没有阻塞我方的、没有下载任务的，是 available 的.
    int peer_available = 2;

    for (int i = 0; i < mi->nr_peers; i++) {
        struct Peer *peer = mi->peers[i];
        if (!peer->get_choked && peer->requesting_index == -1) {
            peer_available = 1;
            if (peer_get_bit(peer, msg->request.index)) {
                // 可以响应的，没有下载任务的，有分片的
                send_request(mi, peer, msg);
                return 0;
            }
        }
    }

    return peer_available;
}

/** @brief 选择需要请求的分片
 *
 * 采取最简单的前面的先下载策略。
 * 使用输出参数 msg 是因为这种 msg 一般都是栈上生成的，用完就丢。
 *
 * @param mi 全局信息
 * @param msg 要发送的请求
 * @param end_game 是否允许抢占正在下载的任务
 * @return 0 - 成功，-1 - 已经完成，1 - 全部正在下载，可 end game
 *
 * @todo 应用优化策略：最少优先
 */
int
select_piece(struct MetaInfo *mi, int end_game)
{
    int sub_idx = 0;
    size_t piece_sz = mi->piece_size;
    size_t sub_cnt = mi->sub_count;
    int is_found_downloading_subpiece = 0;  // 是否遭遇了正在下载的分片，用于判断是否启动 end game.
    int is_all_piece_finished = 1;          // 是否所有分片下载完成

    /// @todo 只需要创建一次 base, 另外只要在需要的时候更新排序！
    struct PieceInfo **base = malloc(sizeof(*base) * mi->nr_pieces);
    for (int i = 0; i < mi->nr_pieces; i++) base[i] = &mi->pieces[i];

    int comp(const void *x, const void *y) {
        const struct PieceInfo *left = *(struct PieceInfo **)x;
        const struct PieceInfo *right = *(struct PieceInfo **)y;
        return left->nr_owners - right->nr_owners;
    }
    qsort(base, mi->nr_pieces, sizeof(*base), comp);

    // 最简单的最前最优先策略
    for (int i = 0; i < mi->nr_pieces; i++) {
        uint32_t index = (uint32_t)(base[i] - mi->pieces);  // 由于是连续的，可以保证 piece 的数量不会超过 uint32_t 的范围。
        if (mi->pieces[index].is_downloaded) {
            continue;
        }

        is_all_piece_finished = 0;

        struct PieceInfo *piece = &mi->pieces[index];
        assert(piece == base[i]);

        // 处理最后一个分片的子分片数量
        if (index + 1 == mi->nr_pieces) {
            piece_sz = mi->file_size % mi->piece_size;
            sub_cnt = (piece_sz - 1) / mi->sub_size + 1;
        }

        for (sub_idx = 0; sub_idx < sub_cnt; sub_idx++) {
            if (piece->substate[sub_idx] == SUB_NA || (piece->substate[sub_idx] == SUB_DOWNLOAD && end_game)) {
                //------------------------------------
                // 寻找可以发送请求的 peer 并发送请求
                //------------------------------------

                uint32_t begin = sub_idx * mi->sub_size;
                uint32_t length = mi->sub_size;

                // 处理最后一个子分片的长度
                // 之前已经默认初始化统一大小了
                if (sub_idx + 1 == sub_cnt && (piece_sz % mi->sub_size) != 0) {
                    length = (uint32_t)(piece_sz % mi->sub_size);
                }

                struct PeerMsg msg = {
                    .len = htonl(13),
                    .id = BT_REQUEST,
                    .request.index = index,
                    .request.begin = begin,
                    .request.length = length
                };

                int ret = select_peer(mi, &msg);
                if (piece->substate[sub_idx] == SUB_DOWNLOAD && end_game) {
                    log("override in END GAME!");
                }

                if      (ret == 0) log("successfully request index %u begin %u length %u", index, begin, length);
                else if (ret == 1) break;     // 没有可以满足这个分片请求的 peer, 放弃这个分片的所有子分片
                else if (ret == 2) { free(base); return 0; }  // 没有可以请求的 peer, 结束整个选择过程
                else { err("unexpected return value %d", ret); exit(EXIT_FAILURE); }
            }
            else if (piece->substate[sub_idx] == SUB_DOWNLOAD) {
                is_found_downloading_subpiece = 1;
            }
        }
    }

    free(base);

    if (is_found_downloading_subpiece) {
        log("all pieces is being downloaded, start end game.");
        return 1;  // enable end-game
    }
    else if (is_all_piece_finished) {
        log("all pieces have been downloaded");
        return 2;  // disable request logic
    }
    else {
        return 0;  // subpiece cannot be sent.
    }
}

/**
 * @brief check a piece's sha1
 * @param fp file
 * @param piece_idx piece index
 * @param piece_size common piece size, no need to adjust for the last one
 * @param file_size file size
 * @param check correct sha1
 * @return 1 - consistent, 0 - not
 */
int check_piece(FILE *fp, int piece_idx, uint32_t piece_size, uint8_t check[20])
{
    uint8_t *piece = malloc(piece_size);
    fseek(fp, piece_idx * piece_size, SEEK_SET);
    ssize_t nr_read = fread(piece, 1, piece_size, fp);
    if (nr_read < piece_size && ferror(fp)) {
        err("failed to read file");
        exit(EXIT_FAILURE);
    }
    uint8_t md[20];
    log("idx %d size %u, read %ld", piece_idx, piece_size, nr_read);
    SHA1(piece, nr_read, md);
    free(piece);
    return memcmp(check, md, 20) == 0;
}

/**
 * @brief 处理分片消息
 *
 * 收到分片消息后，期望调用者处理字节序。
 * 会将子分片写入到对应的分片文件中，如果一个子分片已经被写入过，则抛弃。
 * 出于简单实现的考虑，子分片采取固定大小，使用位图管理完成进度，
 * 最后一个分片不会在这里进行特殊处理，由发送过程保证最后一个分片长度的正确性。
 *
 * 在这里结算一个子分片的下载速度。
 */
void
handle_piece(struct MetaInfo *mi, struct Peer *peer, struct PeerMsg *msg)
{
    struct PieceInfo *piece = &mi->pieces[msg->piece.index];

    // 保证一致性
    assert(peer->requesting_index == msg->piece.index);
    assert(peer->requesting_begin == msg->piece.begin);

    int sub_idx = msg->piece.begin / mi->sub_size;

    uint32_t dl_size = msg->len - 9;  // 9 是 id, index, begin 的冗余长度。

    if (piece->substate[sub_idx] != SUB_FINISH) {
        fseek(mi->file, msg->piece.index * mi->piece_size + msg->piece.begin, SEEK_SET);
        fwrite(msg->piece.block, 1, dl_size, mi->file);
        fflush(mi->file);  // sub piece may not be write back, cause the final race never end.
        piece->substate[sub_idx] = SUB_FINISH;
        peer->contribution += dl_size;
        mi->downloaded += dl_size;
        mi->left -= dl_size;
        log("downloaded %lu", mi->downloaded);

        if (check_substate(mi, msg->piece.index)) {
            if (check_piece(mi->file, msg->piece.index, mi->piece_size, piece->hash)) {
                piece->is_downloaded = 1;
                set_bit(mi->bitfield, msg->piece.index);
                // 发送 HAVE 消息
                struct PeerMsg have_msg = {.len = htonl(5), .have.piece_index = msg->piece.index};
                for (int i = 0; i < mi->nr_peers; i++) {
                    if (!peer_get_bit(peer, msg->piece.index)) {
                        peer_send_msg(peer, &have_msg);
                        log("send %s %d to %s:%u", bt_types[have_msg.id], have_msg.have.piece_index, peer->ip, peer->port);
                    }
                }
            }
            else {
                log("piece %d mismatch", msg->piece.index);
                memset(piece->substate, SUB_NA, mi->sub_count);
                mi->left -= (msg->piece.index == mi->nr_pieces - 1) ?(mi->file_size % mi->piece_size) : mi->piece_size;
                mi->left += dl_size;  // make up the following subtraction
            }
        }
    }
    else {
        log("discard piece %d subpiece %d from %s:%d due to previous accomplishment",
            msg->piece.index, msg->piece.begin, peer->ip, peer->port);
    }

    // 重置下载状态
    peer->requesting_index = -1;
    peer->requesting_begin = -1;

    // 更新下载速度
    struct timespec ct;
    clock_gettime(CLOCK_BOOTTIME, &ct);
    peer->speed = (dl_size * 1.0e9) / ((ct.tv_sec - peer->st.tv_sec) * 1000000000 + ct.tv_nsec - peer->st.tv_nsec);
}

/**
 * @brief Handle request from peer
 * @param pInfo global information
 * @param pPeer the peer to send piece
 * @param pMsg the request msg
 */
void handle_request(struct MetaInfo *pInfo, struct Peer *pPeer, struct PeerMsg *pMsg) {
    FILE *fp = pInfo->file;
    uint32_t piece_size = pInfo->piece_size;
    uint32_t index = pMsg->request.index;
    uint32_t begin = pMsg->request.begin;
    uint32_t length = pMsg->request.length;

    log("%s:%u request index %u begin %u length %u", pPeer->ip, pPeer->port, index, begin, length);

    // Check whether we have that piece.
    // If we allow seeking non-existing piece, it might exceed the file boundary.
    if (!pInfo->pieces[index].is_downloaded) {
        log("give up");
        return;
    }

    // Construct piece message.
    struct PeerMsg *response = malloc(4 + 9 + length);
    response->len = htonl(9 + length);
    response->id = BT_PIECE;
    response->piece.index = htonl(index);
    response->piece.begin = htonl(begin);
    fseek(fp, index * piece_size + begin, SEEK_SET);
    if (fread(response->piece.block, 1, length, fp) < length) {
        err("index %u begin %u length %u is not feasible", index, length, length);
    }

    // Send message
    if (write(pPeer->fd, response, 4 + 9 + length) < 4 + 9 + length) {
        err("damn");
    }

    free(response);
}

/**
 * @brief 处理 BT 消息
 * @param mi 全局信息
 * @param peer 指向发送消息的 peer
 * @param msg peer 发送的消息
 */
void
handle_msg(struct MetaInfo *mi, struct Peer *peer, struct PeerMsg *msg)
{
    // 忽略 KEEP-ALIVE
    if (msg->len == 0) {
        return;
    }

    log("recv %s msg from %s:%d", bt_types[msg->id], peer->ip, peer->port);

    switch (msg->id) {
    case BT_BITFIELD:
        print_bit(msg->bitfield, mi->nr_pieces);
        putchar('\n');
        memcpy(peer->bitfield, msg->bitfield, mi->bitfield_size);
        for (uint32_t i = 0; i < mi->nr_pieces; i++) {
            mi->pieces[i].nr_owners += peer_get_bit(peer, i);
        }
        break;
    case BT_HAVE:
        msg->have.piece_index = ntohl(msg->have.piece_index);
        log("%s:%d has a new piece %d", peer->ip, peer->port, msg->have.piece_index);
        peer_set_bit(peer, msg->have.piece_index);
        mi->pieces[ msg->have.piece_index ].nr_owners += 1;

        print_bit(peer->bitfield, mi->nr_pieces);
        putchar('\n');
        break;
    case BT_PIECE:
        msg->piece.index = htonl(msg->piece.index);
        msg->piece.begin = htonl(msg->piece.begin);
        log("receive a subpiece at piece %d, begin %d, len %d",
            msg->piece.index, msg->piece.begin, msg->len - 9);
        handle_piece(mi, peer, msg);
        break;
    case BT_UNCHOKE:
        peer->get_choked = 0;
        break;
    case BT_CHOKE:
        peer->get_choked = 1;
        break;
    case BT_INTERESTED:
        peer->get_interested = 1;
        break;
    case BT_NOT_INTERESTED:
        peer->get_interested = 0;
        break;
    case BT_REQUEST:
        msg->request.index = ntohl(msg->request.index);
        msg->request.begin = ntohl(msg->request.begin);
        msg->request.length = ntohl(msg->request.length);
        handle_request(mi, peer, msg);
        break;
    case BT_CANCEL:
        /// @todo 处理 CANCEL 消息
        break;
    default:
        break;
    }
}

/**
 * @brief 处理 tracker 响应的 HTTP 报文，将解析后的语法树返回给上层使用，在内部关闭套接字
 *
 * 考虑到 tracker 的响应数据量不大，内部全部使用 recv + MSG_WAITALL 防止 read 读取不足。
 *
 * @param sfd 与 tracker 的连接套接字
 * @return 解析后的语法树，非法时返回 NULL
 */
struct BNode *
handle_tracker_response(int sfd)
{
    // 解析 HTTP 响应报文
    char response[BUF_SIZE] = { 0 };
    char *curr = response;
    size_t size = 0;
    int is_html = 0;
    while (recv(sfd, curr, 1, MSG_WAITALL) == 1) {
        if (*curr++ != '\n') {
            continue;
        }

        printf("%s", response);

#define CONTENT_LENGTH "Content-Length"
#define CONTENT_TYPE "Content-Type"
        if (!strncmp(response, CONTENT_LENGTH, sizeof(CONTENT_LENGTH) - 1)) {
            size = strtoul(response + 16, NULL, 10);
        }
        else if (!strncmp(response, CONTENT_TYPE, sizeof(CONTENT_TYPE) - 1)) {
            if (strstr(response, "text/html") != NULL) {
                is_html = 1;
            }
        }
        else if (!strcmp(response, "\r\n")) {
            break;
        }

        // Reset
        memset(response, 0, sizeof(response));
        curr = response;
    }

    if (size == 0) {
        return NULL;
    }

    char *data = malloc(size);
    if (recv(sfd, data, size, MSG_WAITALL) < size) {
        perror("read");
    }

    struct BNode *bcode = NULL;
    if (is_html) {
        printf("%s", data);
    }
    else {
        bcode = bparser(data);
    }

    free(data);

    return bcode;
}

/**
 * @brief 将 tracker 返回的 peers 异步 connect 并加入 epoll 队列
 *
 * @param mi 全局信息
 * @param efd epoll 描述符
 * @param bcode B 编码数据
 */
void
handle_peer_list(struct MetaInfo *mi, int efd, struct BNode *bcode)
{
    const struct BNode *peers = query_bcode_by_key(bcode, "peers");

    if (peers == NULL) {
        log("no peers are found");
        return;
    }

    for (int i = 0; i < peers->s_size; i += 6) {
        struct {
            union {
                uint8_t ip[4];
                uint32_t addr;
            };
            uint16_t port;
        } *p = (void *)&peers->s_data[i];

        //--------------------------------------------
        // 防止从多个 tracker 连接同一个 peer
        // 只能检查我方主动连接的 peer, 因为对方主动连接的，
        // 即便 ip 地址相同，端口号也是动态分配的，只能通过
        // peer-id 检查。
        //--------------------------------------------

        if (get_peer_by_addr(mi, p->addr, p->port) != NULL) {
            log("already handshaked with peer %d.%d.%d.%d:%d", p->ip[0], p->ip[1], p->ip[2], p->ip[3], ntohs(p->port));
            continue;
        }

        if (get_wait_peer_fd(mi, p->addr, p->port) != -1) {
            log("already connecting to peer %d.%d.%d.%d:%d", p->ip[0], p->ip[1], p->ip[2], p->ip[3], ntohs(p->port));
            continue;
        }

        int fd = socket(AF_INET, SOCK_STREAM, 0);

        // 将地址信息加入到等待 peer 集合以备之后的查重工作
        add_wait_peer(mi, fd, p->addr, p->port, 0);

        log("fd %d is assigned for %d.%d.%d.%d:%d", fd, p->ip[0], p->ip[1], p->ip[2], p->ip[3], ntohs(p->port));
        struct sockaddr_in sa;
        sa.sin_family = AF_INET;
        memcpy(&sa.sin_addr.s_addr, p->ip, 4);
        memcpy(&sa.sin_port, &p->port, 2);
        if (async_connect(efd, fd, (void *)&sa, sizeof(sa)) != EINPROGRESS) {
            perror("async");
        }
    }
}

/**
 * @brief 提取 interval 信息并设置定时
 *
 * @param tracker 指向发送响应的 tracker
 * @param bcode 指向解析后的 B 编码语法树
 * @param efd epoll file descriptor，用于加入 timer fd
 */
void
handle_interval(struct Tracker *tracker, struct BNode *bcode, int efd)
{
    const struct BNode *interval = query_bcode_by_key(bcode, "interval");
    if (interval == NULL) {
        fprintf(stderr, "interval not found\n");
        return;
    }

    tracker->timerfd = timerfd_create(CLOCK_REALTIME, 0);
    if (tracker->timerfd == -1) {
        perror("timerfd");
    }
    log("tracker %s timer FD %d", tracker->host, tracker->timerfd);

    // 单次定时器，靠重新获取报文来重新定时
    struct itimerspec ts = {
        .it_interval = { .tv_sec = 0, .tv_nsec = 0 },
        .it_value = { .tv_sec = interval->i, .tv_nsec = 0 }
    };

    if (timerfd_settime(tracker->timerfd, 0, &ts, NULL) == -1) {
        perror("settime");
    }

    struct epoll_event ev = {
        .data.fd = tracker->timerfd,
        .events = EPOLLIN
    };
    epoll_ctl(efd, EPOLL_CTL_ADD, tracker->timerfd, &ev);
}

/**
 * @brief 处理出错套接字
 *
 * 程序所使用的描述符主要有两种：socket fd 和 timer fd.
 *
 * 这里的错误处理逻辑针对 socket fd. Timer fd 相对来说并不那么容易出错。
 * 函数会首先判断套接字是 tracker 的还是 peer 的，以输出错误信息并更新对应的队列。
 *
 * @param mi 全局信息
 * @param error_fd 出错的套接字
 */
void
handle_error(struct MetaInfo *mi, int error_fd)
{
    int result;
    socklen_t result_len = sizeof(result);
    struct Peer *peer;
    struct Tracker *tracker;
    int wp_idx;

    if (getsockopt(error_fd, SOL_SOCKET, SO_ERROR, &result, &result_len) < 0) {
        perror("getsockopt");
    }

    if ((tracker = get_tracker_by_fd(mi, error_fd)) != NULL) {
        // tracker 列表不需要修改，无法连接的 tracker 留在列表里不会产生冲突。
        err("%s:%s%s: %s", tracker->host, tracker->port, tracker->request, strerror(result));
    }
    else if ((peer = get_peer_by_fd(mi, error_fd)) != NULL) {
        err("rm peer %s:%u: %s", peer->ip, peer->port, strerror(result));
        del_peer_by_fd(mi, error_fd);
    }
    else if ((wp_idx = get_wait_peer_index_by_fd(mi, error_fd)) != -1) {
        // 更新 wait peers 列表，将连接失败的从队列删除。
        struct in_addr addr = { .s_addr = mi->wait_peers[wp_idx].addr };
        err("rm wait peer %s:%u: %s", inet_ntoa(addr), mi->wait_peers[wp_idx].port, strerror(result));
        rm_wait_peer(mi, wp_idx);
    }
    else {
        err("unexpected fd %d", error_fd);
        exit(EXIT_FAILURE);
    }
}

/**
 * @brief 执行连接建立后的相关操作
 *
 * 与 tracker 和 peer 通过 connect 方式建立连接后（相对的，还有通过 accept 与 peer 建立连接的情形），
 * 按照协议要求，要主动发送消息（HTTP 请求，握手）。
 * 本函数首先根据套接字确定套接字所属的对象，然后发送对应的消息。
 *
 * @param mi 全局信息
 * @param sfd 连接套接字
 */
void
handle_ready(struct MetaInfo *mi, int sfd)
{
    struct Tracker *tracker;
    if ((tracker = get_tracker_by_fd(mi, sfd)) != NULL) {
        log("connected to %s:%s%s", tracker->host, tracker->port, tracker->request);
        send_msg_to_tracker(mi, tracker);
    }
    else if (get_wait_peer_index_by_fd(mi, sfd) != -1) {
        // 这里不进行检查，因为可以肯定是 tracker
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        getpeername(sfd, (struct sockaddr *)&addr, &addrlen);
        log("%s is connected at %u", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
        send_handshake(sfd, mi);
        log("handshaking with %s:%d", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    }
    else {
        log("already-deleted socket %d", sfd);
    }
}

/**
 * @brief 完成握手消息的处理
 * @return 0: 正常, -1: 连接断开
 */
int
finish_handshake(struct MetaInfo *mi, int sfd)
{
    int wp_idx = get_wait_peer_index_by_fd(mi, sfd);
    if (wp_idx == -1) {
        log("unexpected fd %d for handshaking", sfd);
        return -1;
    }

    struct WaitPeer *wp = &mi->wait_peers[wp_idx];

    if (wp->msg == NULL) {
        assert(wp->wanted == 0);
        wp->wanted = sizeof(PeerHandShake);
        wp->msg = malloc(sizeof(PeerHandShake));
    }

    //-------------------
    // 异步读取握手消息
    //-------------------

    ssize_t nr_read = read(sfd, wp->msg + sizeof(PeerHandShake) - wp->wanted, wp->wanted);
    if (nr_read == 0) {
        // 在 EPOLLIN 事件里还能读出 0, 基本是 FIN 了
        log("disconnect during read handshake from %u.%u.%u.%u:%u", wp->ip[0], wp->ip[1], wp->ip[2], wp->ip[3], wp->port);
        log("handshaking failed");
        close(sfd);
        rm_wait_peer(mi, wp_idx);
        return -1;
    }

    wp->wanted -= nr_read;

    if (wp->wanted > 0) {
        return 0;
    }

    //-------------------------------------
    // 完成握手消息读取，执行添加 peer 逻辑
    //-------------------------------------

    struct WaitPeer p = *wp;
    PeerHandShake *hs = (void *)p.msg;
    rm_wait_peer(mi, wp_idx);

    //-------------------------------------
    // 检查 peer 是否重复
    //-------------------------------------

    // 防止自己和自己连接
    if (memcmp(mi->peer_id, hs->hs_peer_id, HASH_SIZE) == 0) {
        close(sfd);
        return -1;
    }

    // 防止和已有 peer 重复
    for (int i = 0; i < mi->nr_peers; i++) {
        if (memcmp(hs->hs_peer_id, mi->peers[i]->peer_id, HASH_SIZE) == 0) {
            close(sfd);
            return -1;
        }
    }

    // 将对方加入到正式 peers 列表中

    struct Peer *peer = peer_new(sfd, mi->nr_pieces);
    memcpy(peer->peer_id, hs->hs_peer_id, HASH_SIZE);
    add_peer(mi, peer);

    log("handshaked with %u.%u.%u.%u:%u",
        p.ip[0], p.ip[1], p.ip[2], p.ip[3], ntohs(p.port));

    // 如果是对方主动连接，则我方要返回 handshake
    if (p.direction == 1) {
        send_handshake(p.fd, mi);
    }

    // 发送 bitfield
    struct PeerMsg *bitfield_msg = calloc(4 + 1 + mi->bitfield_size, 1);
    bitfield_msg->len = htonl((1 + mi->bitfield_size));
    bitfield_msg->id = BT_BITFIELD;
    memcpy(bitfield_msg->bitfield, mi->bitfield, mi->bitfield_size);
    peer_send_msg(peer, bitfield_msg);
    log("send %s to %s:%u", bt_types[bitfield_msg->id], peer->ip, peer->port);
    free(bitfield_msg);

    // 出于简单实现的考虑，暂时先无条件发送 UNCHOKE 和 INTERESTED 报文。

    // UNCHOKE 和 INTERESTED 都没有数据载荷，区别只有 id.
    // 故我们使用同一块缓冲区，修改 id 后进行发送.
    // 数据结构本身的大小超过 5 字节，但是有意义的报文内容只占 5 字节。
    struct PeerMsg msg = { .len = htonl(1) };
    uint8_t msg_type[] = { BT_UNCHOKE, BT_INTERESTED };

    for (int i = 0; i < sizeof(msg_type) / sizeof(msg_type[0]); i++) {
        msg.id = msg_type[i];
        if (write(peer->fd, &msg, 5) < 5) {
            perror("send msg");
        }
        log("send %s to %s:%d", bt_types[msg.id], peer->ip, peer->port);
    }

    return 0;
}

/**
 * @brief handle the coming peer
 * @param mi metainfo
 * @param ev the current epoll event
 * @param efd epoll file descriptor
 */
void handle_coming_peer(struct MetaInfo *mi, struct epoll_event *ev, int efd)
{
    struct sockaddr_in peer_addr, local_addr;
    socklen_t peer_len = sizeof(peer_addr), local_len = sizeof(local_addr);

    int fd = accept(mi->listen_fd, (struct sockaddr *)&peer_addr, &peer_len);
    getsockname(fd, (void *)&local_addr, &local_len);
    log("one peer wants to connect, assigned connnection fd %d", fd);
    log("peer  %s:%u", inet_ntoa(peer_addr.sin_addr), ntohs(peer_addr.sin_port));
    log("local %s:%u", inet_ntoa(local_addr.sin_addr), ntohs(local_addr.sin_port));

    add_wait_peer(mi, fd, peer_addr.sin_addr.s_addr, peer_addr.sin_port, 1);   // 1 - connecting from

    // 侦听握手消息
    ev->data.fd = fd;
    ev->events = EPOLLIN;
    epoll_ctl(efd, EPOLL_CTL_ADD, fd, ev);
}

/**
 * @brief 处理所有网络报文
 *
 * 使用 epoll 侦听各个描述符的事件，根据事件属性和描述符的所属采取相应的操作。
 * 主要涉及的描述符类型：
 * 1. 与 tracker 的连接套接字
 * 2. 与 peer 的连接套接字
 * 3. tracker 的回访定时器
 * 4. 本机的 keep-alive 定时器
 *
 * 目前只对 peer 的 bt 消息做异步接受，其他报文基本要求同步地完全接受。
 *
 * 最多每 5s 处理一次发送报文的逻辑。目前来看可能出现 write 写满导致的阻塞。
 *
 * @param mi 种子文件元信息
 * @param efd epoll file descriptor
 */
void
bt_handler(struct MetaInfo *mi, int efd)
{
    /*
     * 报文处理状态机
     */

    char *bar = "---------------------------------------------------------------";
    struct epoll_event *events = calloc(100, sizeof(*events));
    int end_game = 0;
    while (1) {
        int n = epoll_wait(efd, events, 100, -1);  // 超时限制 5s

        // 处理接收逻辑
        for (int i = 0; i < n; i++) {
            puts(bar);
            struct epoll_event *ev = &events[i];
            log("handle fd %d", ev->data.fd);

            if (ev->events & (EPOLLERR | EPOLLHUP)) {  // 异步 connect 错误处理
                log("handle error");
                handle_error(mi, ev->data.fd);
                close(ev->data.fd);
                epoll_ctl(efd, EPOLL_CTL_DEL, ev->data.fd, NULL);
                continue;
            }

            if (ev->events & EPOLLOUT) {  // connect 完成
                log("handle connect");
                // EPOLLOUT 表明套接字可写, 对于刚刚调用过 connect 的套接字来讲，
                // 即意味着连接成功建立.
                handle_ready(mi, ev->data.fd);
                // 对于新建立的连接，之后都是要接收数据的，所以统一修改侦听 EPOLLIN.
                ev->events = EPOLLIN;
                epoll_ctl(efd, EPOLL_CTL_MOD, ev->data.fd, ev);
                continue;
            }

            if (!(ev->events & EPOLLIN)) {
                log("unexpected event %x", ev->events);
                exit(EXIT_FAILURE);
            }

            //----------------------
            // 接受并处理 BT 报文
            //----------------------

            // 关于握手报文与 BT 报文的区分方法:
            //   握手报文和 BT 报文无法从数据格式上进行区分, 但是一个没有完成
            //   握手的 peer 是不会发送 BT 报文的. 我们将完成握手的 peer 记录
            //   在一个集合 peers 中(抽象). 对于当前的可读套接字, 如果它不在
            //   peers 集合里, 则是没有完成握手的 peer, 那么它送来的数据, 只
            //   可能是握手信息或者 FIN 报文.

            // peers, trackers 等查询函数有线性时间复杂度，而 peer 的 BT 消息最为频繁，
            // 所以优先考虑套接字属于 peer 可以减少开销。

            struct Peer *peer;
            struct Tracker *tracker;

            // 处理 BT 消息
            if ((peer = get_peer_by_fd(mi, ev->data.fd)) != NULL) {
                // 虽然有多个 BT 报文凑到一个 TCP 报文段里的情况, 但是这里只处理一个报文.
                // 由于报文变长, 所以要注意保持数据的一致性.
                log("handling %s:%u :", peer->ip, peer->port);
                struct PeerMsg *msg = peer_get_packet(peer);
                if (msg == NULL) {
                    log("remove peer %s:%d", peer->ip, peer->port);
                    epoll_ctl(efd, EPOLL_CTL_DEL, ev->data.fd, NULL);
                    close(peer->fd);
                    // 不需要撤销分片的下载状态，因为超时后自会重置下载状态，
                    // 还有可能本 peer 负责的子分片已经超时，导致 分片状态被修改，
                    // 再次修改会导致不一致。
                    del_peer_by_fd(mi, peer->fd);
                }
                else if (peer->wanted == 0) {  // 读取了完整的 BT 消息
                    handle_msg(mi, peer, msg);
                    free(msg);
                }

                continue;
            }

            // 定时事件：发送 KEEP ALIVE
            if (ev->data.fd == mi->timerfd) {
                log("keep-alive");
                uint64_t expiration;
                read(mi->timerfd, &expiration, 8);  // 消耗定时器数据才能重新等待
                int len = htonl(0);
                for (int k = 0; k < mi->nr_peers; k++) {
                    write(mi->peers[k]->fd, &len, 4);
                }

                continue;
            }

            // peer 主动建立连接请求
            if (ev->data.fd == mi->listen_fd) {
                handle_coming_peer(mi, ev, efd);
                continue;
            }

            // tracker 的响应
            if ((tracker = get_tracker_by_fd(mi, ev->data.fd)) != NULL) {
                log("handle tracker response");
                struct BNode *bcode = handle_tracker_response(tracker->sfd);
                if (bcode != NULL) {
                    print_bcode(bcode, 0, 0);
                    handle_peer_list(mi, efd, bcode);
                    handle_interval(tracker, bcode, efd);
                    free_bnode(&bcode);
                }
                epoll_ctl(efd, EPOLL_CTL_DEL, tracker->sfd, NULL);
                close(tracker->sfd);  // tracker 的连接只用一次
                tracker->sfd = -1;

                continue;
            }

            // 定时回访 tracker
            if ((tracker = get_tracker_by_timer(mi, ev->data.fd)) != NULL) {
                log("timer event for %s:%s%s", tracker->host, tracker->port, tracker->request);
                if (close(tracker->timerfd) == -1)
                    perror("close tracker timer fd");
                if (epoll_ctl(efd, EPOLL_CTL_DEL, tracker->timerfd, NULL) == -1)
                    perror("epoll delete tracker timer fd");
                async_connect_to_tracker(tracker, efd);

                continue;
            }

            // 处理 peer 握手消息
            if (finish_handshake(mi, ev->data.fd) == -1) {
                // 连接已断开，没有必要再侦听
                epoll_ctl(efd, EPOLL_CTL_DEL, ev->data.fd, NULL);
            }
        }

        // 处理发送逻辑
        if (end_game != 2 && mi->left != 0) {
            int ret = select_piece(mi, end_game);
            // 不放过这可以 end-game 的第一次请求机会
            if (end_game == 0 && ret == end_game) {
                select_piece(mi, ret);
            }
            end_game = ret;
        }

        // 统计信息
        int work_cnt = 0;
        for (int i = 0; i < mi->nr_peers; i++) {
            if (mi->peers[i]->requesting_index != -1) {
                work_cnt++;
            }
        }
        log("%d / %d peers working", work_cnt, mi->nr_peers);
        log("peers >>>");
        for (int i = 0; i < mi->nr_peers; i++) {
            struct Peer *pr = mi->peers[i];
            log("%16s:%-5d %7s %s  %10d  %6d  %.2lfKB/s", pr->ip, pr->port,
                   pr->get_choked ? "choke" : "unchoke",
                   pr->get_interested ? "int" : "not",
                   pr->contribution, pr->wanted, pr->speed / 1000.0);
        }
        log("peers <<<");

        log("wait peers >>>");
        for (int i = 0; i < mi->nr_wait_peers; i++) {
            struct WaitPeer *p = &mi->wait_peers[i];
            struct in_addr ia = { .s_addr = p->addr };
            log("%2d  %16s:%-5d  %d", p->fd, inet_ntoa(ia), ntohs(p->port), p->direction);
        }
        log("wait peers <<<");

        if (mi->slow) { sleep(4); }
    }
}
