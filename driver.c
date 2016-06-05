/** @brief 顶层驱动模块
 *  @file driver.c
 */

#include "bparser.h"
#include "butil.h"
#include "util.h"
#include "peer.h"
#include "connect.h"
#include <string.h>
#include <assert.h>
#include <unistd.h>     // read(), write()
#include <errno.h>      // EINPROGRESS
#include <sys/epoll.h>  // epoll_create1(), epoll_ctl(), epoll_wait(), epoll_event
#include <arpa/inet.h>  // inet_ntoa()

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
    memcpy(handshake.hs_peer_id, "-Test-Test-Test-Test", HASH_SIZE);

    if (write(sfd, &handshake, sizeof(handshake)) < sizeof(handshake)) {
        perror("handshake");
    }
};

/**
 * 建立连接, 发送请求一条龙. 可以通过 event 指定发送的具体时间,
 * 其余信息完全通过 mi 自动填写. 使用返回的套接字读取响应, 读完
 * 了就可以直接关闭套接字了.
 */
int
send_msg_to_tracker(struct MetaInfo *mi, int no, const char *event)
{
    struct Tracker *tracker = &mi->trackers[no];
    int sfd = connect_to_tracker(tracker->host, tracker->port);

    // 请求头
    struct HttpRequest *req = create_http_request("GET", tracker->request);

    // info_hash
    {
        char infohash[3 * HASH_SIZE + 1] = { 0 };
        char *curr = infohash;
        for (int i = 0; i < HASH_SIZE; i++) {
            curr += sprintf(curr, "%%%02x", mi->info_hash[i]);
        }
        add_http_request_attr(req, "info_hash", "%s", infohash);
    }

    // TODO 错误做法
    // 获取嵌入请求的侦听端口号
    {
        struct sockaddr_in sockaddr;
        socklen_t socklen = sizeof(sockaddr);
        if (getsockname(sfd, (struct sockaddr *)&sockaddr, &socklen) == -1) {
            perror("getsockname");
        }
        add_http_request_attr(req, "port", "%d", sockaddr.sin_port);
    }

    // 其他一些请求信息
    add_http_request_attr(req, "peer_id"   , "-Test-Test-Test-Test");
    add_http_request_attr(req, "uploaded"  , "0");
    add_http_request_attr(req, "downloaded", "0");
    add_http_request_attr(req, "left"      , "%ld", mi->file_size);

    if (event != NULL) {
        add_http_request_attr(req, "event", "%s", event);
    }

    send_http_request(req, sfd);

    return sfd;
}

/** @brief 选择需要请求的分片
 *
 * 采取最简单的前面的先下载策略。
 * 使用输出参数 msg 是因为这种 msg 一般都是栈上生成的，用完就丢。
 *
 * @param mi 全局信息
 * @param msg 要发送的请求
 * @return 0 - 成功，-1 - 失败
 */
int
select_piece(struct MetaInfo *mi, struct PeerMsg *msg)
{
    // 找到一个没有完成的子分片
    // @todo 应用优化策略

    uint32_t index = 0;
    uint32_t begin = 0;
    uint32_t length = mi->sub_size;

    int sub_idx = 0;
    size_t piece_sz = mi->piece_size;
    size_t sub_cnt = mi->sub_count;

    time_t curr_time = time(NULL);

    // 最简单的最前最优先策略
    for (; index < mi->nr_pieces; index++) {
        if (mi->pieces[index].is_downloaded) {
            continue;
        }

        struct PieceInfo *piece = &mi->pieces[index];

        // 处理最后一个分片的子分片数量
        if (index + 1 == mi->nr_pieces) {
            piece_sz = mi->file_size % mi->piece_size;
            sub_cnt = (piece_sz - 1) / mi->sub_size + 1;
        }

        for (sub_idx = 0; sub_idx < sub_cnt; sub_idx++) {
            if (piece->substate[sub_idx] == SUB_NA) {
                break;
            }
        }

        if (sub_idx == sub_cnt) {
            log("some subpieces of piece %d is being downloaded", index);
            check_substate(mi, index);
            for (sub_idx = 0; sub_idx < sub_cnt; sub_idx++) {
                if (piece->substate[sub_idx] == SUB_DOWNLOAD && difftime(curr_time, piece->subtimer[sub_idx]) > WAIT_THRESHOLD) {
                    log("index %d begin %d exceeds time limit, re-request", index, sub_idx * mi->sub_size);
                    piece->substate[sub_idx] = SUB_NA;
                    break;
                }
            }
        }

        if (sub_idx == sub_cnt) {
            log("none exceeds time limit");
        }
        else {
            break;
        }
    }

    if (index == mi->nr_pieces) {
        log("all pieces have been / is being downloaded");
        return -1;
    }

    begin = sub_idx * mi->sub_size;

    // 处理最后一个子分片的长度
    // 之前已经默认初始化统一大小了
    if (sub_idx + 1 == sub_cnt && (piece_sz % mi->sub_size) != 0) {
        length = (uint32_t)(piece_sz % mi->sub_size);
    }

    unsigned int req_len = 13;
    struct PeerMsg temp = {
        .len = htonl(req_len),
        .id = BT_REQUEST,
        .request.index = index,
        .request.begin = begin,
        .request.length = length
    };

    *msg = temp;

    return 0;
}

/** @brief 选择可以发送请求的 peer
 * 
 * msg 不要转换字节序
 */
struct Peer *
select_peer(struct MetaInfo *mi, struct PeerMsg *msg)
{
    double max_speed = -1.0;
    struct Peer *fastest_peer = NULL;

    for (int i = 0; i < mi->nr_peers; i++) {
        struct Peer *peer = mi->peers[i];
        if (!peer->get_choked && peer->requesting_index == -1 && peer_get_bit(peer, msg->request.index)) {
            // 可以响应的，没有下载任务的，有分片的
            if (peer->speed > max_speed) {
                max_speed = peer->speed;
                fastest_peer = peer;
            }
        }
    }

    return fastest_peer;
}

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
    assert(piece->substate[sub_idx] == SUB_NA);
    piece->substate[sub_idx] = SUB_DOWNLOAD;
    piece->subtimer[sub_idx] = time(NULL);
    peer->start_time = piece->subtimer[sub_idx];

    msg->request.index = htonl(index);
    msg->request.begin = htonl(begin);
    msg->request.length = htonl(length);

    if (write(peer->fd, msg, 4 + 13) < 4 + 13) {
        perror("send request");
    }

    log("send %s [index %d begin %d length %d] to %s:%d",
        bt_types[msg->id], index, begin, length, peer->ip, peer->port);
}

/** @brief 处理分片消息
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

    if (piece->substate[sub_idx] != SUB_FINISH) {
        fseek(mi->file, msg->piece.index * mi->piece_size + msg->piece.begin, SEEK_SET);
        fwrite(msg->piece.block, 1, msg->len - 9, mi->file);  // 9 是 id, index, begin 的冗余长度。
        piece->substate[sub_idx] = SUB_FINISH;
        if (check_substate(mi, msg->piece.index)) {
            piece->is_downloaded = 1;
        }
        peer->contribution += msg->len - 9;
    }
    else {
        log("discard piece %d subpiece %d from %s:%d due to previous accomplishment",
            msg->piece.index, msg->piece.begin, peer->ip, peer->port);
    }

    // 重置下载状态
    peer->requesting_index = -1;
    peer->requesting_begin = -1;

    // 结算下载速度
    peer->speed = (msg->len - 9) / difftime(time(NULL), peer->start_time);
    log("%s:%d's speed %lfB/s", peer->ip, peer->port, peer->speed);
}

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
        // TODO 根据 bitfield 增加 piece 持有者数量(首先...).
        break;
    case BT_HAVE:
        msg->have.piece_index = ntohl(msg->have.piece_index);
        log("%s:%d has a new piece %d", peer->ip, peer->port, msg->have.piece_index);
        peer_set_bit(peer, msg->have.piece_index);
        // TODO 根据 piece_index 增加 piece 持有者数量(首先...).
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
    default:
        break;
    }
}

/** @brief 处理 tracker 响应的 HTTP 报文，将数据载荷读取到动态空间供上层使用，在内部关闭套接字
 *
 * 考虑到 tracker 的响应数据量不大，内部全部使用 recv + MSG_WAITALL 防止 read 读取不足。
 *
 * @param sfd 与 tracker 的连接套接字
 */
void *
handle_tracker_response(int sfd)
{
    // 解析 HTTP 响应报文
    char response[BUF_SIZE] = { 0 };
    char *curr = response;
    size_t size = 0;
    while (recv(sfd, curr, 1, MSG_WAITALL) == 1) {
        if (*curr++ != '\n') {
            continue;
        }

        printf("%s", response);

        if (!strncmp(response, "Content-Length", 14)) {
            size = strtoul(response + 16, NULL, 10);
        }
        else if (!strcmp(response, "\r\n")) {
            break;
        }

        // Reset
        memset(response, 0, sizeof(response));
        curr = response;
    }

    uint8_t *data = malloc(size);
    if (recv(sfd, data, size, MSG_WAITALL) < size) {
        perror("read");
    }

    close(sfd);  // 对 tracker 来说, 没必要维持长连接.

    return data;
}

/** @brief 将 tracker 返回的 peers 异步 connect 并加入 epoll 队列
 *
 * @param efd epoll 描述符
 * @param data B 编码数据
 */
void
handle_peer_list(int efd, void *data)
{
    // 解析 peers 列表, 异步连接 peer.
    struct BNode *bcode = bparser(data);

    if (bcode == NULL) {
        log("it is not bcode data");
        return;
    }

    print_bcode(bcode, 0, 0);
    const struct BNode *peers = dfs_bcode(bcode, "peers");

    if (peers == NULL) {
        log("no peers are found");
        return;
    }

    for (int i = 0; i < peers->s_size; i += 6) {
        struct {
            uint8_t ip[4];
            uint16_t port;
        } *p = (void *)&peers->s_data[i];
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        log("fd %d is assigned for %d.%d.%d.%d:%d", fd, p->ip[0], p->ip[1], p->ip[2], p->ip[3], htons(p->port));
        struct sockaddr_in sa;
        sa.sin_family = AF_INET;
        memcpy(&sa.sin_addr.s_addr, p->ip, 4);
        memcpy(&sa.sin_port, &p->port, 2);
        if (async_connect(efd, fd, (void *)&sa, sizeof(sa)) != EINPROGRESS) {
            perror("async");
        }
    }

    free_bnode(&bcode);
    free(data);
}

/** @brief 处理与 tracker 的交互
 * @param mi 种子文件元信息
 * @param tracker_idx tracker 在元信息中的下标
 *
 * 本函数不主动关闭套接字。
 */
void
tracker_handler(struct MetaInfo *mi, int tracker_idx)
{
    int efd = epoll_create1(0);
    if (efd == -1) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    int sfd = send_msg_to_tracker(mi, tracker_idx, NULL);

    char *data = handle_tracker_response(sfd);

    handle_peer_list(efd, data);

    /*
     * 报文处理状态机
     */

    char *bar = "---------------------------------------------------------------";
    struct epoll_event *events = calloc(100, sizeof(*events));
    while (1) {
        int n = epoll_wait(efd, events, 100, 5000);  // 超时限制 5s

        if (n < 0) {
            break;
        }

        // 处理接收逻辑
        for (int i = 0; i < n; i++) {
            puts(bar);
            if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)) {  // 异步 connect 错误处理
                int result;
                socklen_t result_len = sizeof(result);
                if (getsockopt(events[i].data.fd, SOL_SOCKET, SO_ERROR, &result, &result_len) < 0) {
                    perror("getsockopt");
                }
                err("%d: %s", events[i].data.fd, strerror(result));
                close(events[i].data.fd);
                epoll_ctl(efd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
            }
            else if (events[i].events & EPOLLOUT) {  // connect 完成
                // EPOLLOUT 表明套接字可写, 对于刚刚调用过 connect 的套接字来讲,
                // 即意味着连接成功建立.
                struct sockaddr_in addr;
                socklen_t addrlen = sizeof(addr);
                getpeername(events[i].data.fd, (struct sockaddr *)&addr, &addrlen);
                log("%s is connected at %u", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
                send_handshake(events[i].data.fd, mi);
                log("handshaking with %s:%d", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
                events[i].events = EPOLLIN;
                epoll_ctl(efd, EPOLL_CTL_MOD, events[i].data.fd, &events[i]);
            }
            else if (events[i].events & EPOLLIN) {  // 接受并处理 BT 报文
                /*
                 * 关于握手报文与 BT 报文的区分方法:
                 *
                 *   握手报文和 BT 报文无法从数据格式上进行区分, 但是一个没有完成
                 *   握手的 peer 是不会发送 BT 报文的. 我们将完成握手的 peer 记录
                 *   在一个集合 peers 中(抽象). 对于当前的可读套接字, 如果它不在
                 *   peers 集合里, 则是没有完成握手的 peer, 那么它送来的数据, 只
                 *   可能是握手信息或者 FIN 报文.
                 */
                struct Peer *peer;
                if ((peer = get_peer_by_fd(mi, events[i].data.fd)) == NULL) {  // 完成握手过程
                    PeerHandShake handshake = {};
                    ssize_t nr_read = recv(events[i].data.fd, &handshake, sizeof(handshake), MSG_WAITALL);
                    if (nr_read == 0) {
                        struct sockaddr_in addr;
                        socklen_t addrlen = sizeof(addr);
                        getpeername(events[i].data.fd, (struct sockaddr *)&addr, &addrlen);
                        log("handshaking failed, disconnect %s:%u", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

                        epoll_ctl(efd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                        close(events[i].data.fd);
                    }
                    else {
                        peer = peer_new(events[i].data.fd, mi->nr_pieces);
                        add_peer(mi, peer);
                        log("successfully handshake with %s:%u", peer->ip, peer->port);

                        // 出于简单实现的考虑，暂时先无条件发送 UNCHOKE 和 INTERESTED 报文。

                        // UNCHOKE 和 INTERESTED 都没有数据载荷，区别只有 id.
                        // 故我们使用同一块缓冲区，修改 id 后进行发送.
                        // 数据结构本身的大小超过 5 字节，但是有意义的报文内容只占 5 字节。
                        struct PeerMsg msg = { .len = htonl(1) };
                        uint8_t msg_type[] = { BT_UNCHOKE, BT_INTERESTED };

                        for (int k = 0; k < sizeof(msg_type) / sizeof(msg_type[0]); k++) {
                            msg.id = msg_type[k];
                            if (write(peer->fd, &msg, 5) < 5) {
                                perror("send msg");
                            }
                            log("send %s to %s:%d", bt_types[msg.id], peer->ip, peer->port);
                        }
                    }
                }
                else {  // 处理具体的 BT 报文
                    // 虽然有多个 BT 报文凑到一个 TCP 报文段里的情况, 但是这里只处理一个报文.
                    // 由于报文变长, 所以要注意保持数据的一致性.
                    log("handling %s:%u :", peer->ip, peer->port);
                    struct PeerMsg *msg = peer_get_packet(peer);
                    if (msg == NULL) {
                        log("remove peer %s:%d", peer->ip, peer->port);
                        epoll_ctl(efd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                        close(peer->fd);

                        // 不需要撤销分片的下载状态，因为超时后自会重置下载状态

                        del_peer_by_fd(mi, peer->fd);
                        log("%d peers left", mi->nr_peers);
                    }
                    else if (peer->wanted == 0) {  // 读取了完整的 BT 消息
                        handle_msg(mi, peer, msg);
                        free(msg);
                    }
                    else {  // 尚未读取完整
                        continue;
                    }
                }
            }
        }


        int len = htonl(0);
        for (int i = 0; i < mi->nr_peers; i++) {
            write(mi->peers[i]->fd, &len, 4);
        }

        // 处理发送逻辑
        struct PeerMsg msg;
        struct Peer *peer;
        if (select_piece(mi, &msg) != -1) {
            if ((peer = select_peer(mi, &msg)) != NULL) {
                send_request(mi, peer, &msg);
            }
        }

        int work_cnt = 0;
        for (int i = 0; i < mi->nr_peers; i++) {
            if (mi->peers[i]->requesting_index != -1) {
                work_cnt++;
            }
        }
        log("%d / %d peers working", work_cnt, mi->nr_peers);
        for (int i = 0; i < mi->nr_peers; i++) {
            struct Peer *pr = mi->peers[i];
            printf("%s:%d %s %s %d\n", pr->ip, pr->port,
                   pr->get_choked ? "choke" : "unchoke",
                   pr->get_interested ? "interest" : "not",
                   pr->contribution);
        }
    }
}

/**
 * @brief 将种子文件完全载入内存
 * @param torrent 种子文件名
 * @return 种子文件数据 [动态缓冲区]
 */
char *
get_torrent_data_from_file(const char *torrent)
{
    FILE *fp = fopen(torrent, "rb");

    fseek(fp, 0, SEEK_END);
    ssize_t size = ftell(fp);
    char *bcode = calloc((size_t)size, sizeof(*bcode));

    rewind(fp);
    if (fread(bcode, 1, (size_t)size, fp) < size) {
        panic("reading is not sufficient");
    }

    fclose(fp);

    return bcode;
}

int
main(int argc, char *argv[])
{
    struct MetaInfo *mi = calloc(1, sizeof(*mi));

    char *bcode = get_torrent_data_from_file(argv[1]);
    struct BNode *ast = bparser(bcode);

    puts("Parsed Bencode:");
    print_bcode(ast, 0, 0);

    extract_trackers(mi, ast);
    extract_pieces(mi, ast);
    metainfo_load_file(mi, ast);
    make_info_hash(ast, mi->info_hash);

    printf("info_hash: ");
    for (int i = 0; i < HASH_SIZE; i++) {
        printf("%02x", mi->info_hash[i]);
    }
    printf("\n");

    sleep(1);

    puts("Tracker list:");
    for (int i = 0; i < mi->nr_trackers; i++) {
        printf("%d. %s://%s:%s%s\n", i,
               mi->trackers[i].method, mi->trackers[i].host, mi->trackers[i].port, mi->trackers[i].request);
    }

    printf("input the tracker number: ");
    fflush(stdout);
    int no;
    scanf("%d", &no);
    tracker_handler(mi, no);

    free(bcode);
    free_bnode(&ast);
    free_metainfo(&mi);
}
