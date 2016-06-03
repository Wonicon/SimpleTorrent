#include "bparser.h"
#include "butil.h"
#include "util.h"
#include "peer.h"
#include "connect.h"
#include "metainfo.h"
#include <string.h>
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
    memcpy(handshake.hs_peer_id, "-Test-Test-Test-Test", 20);

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
        char infohash[3 * 20 + 1] = { 0 };
        char *curr = infohash;
        for (int i = 0; i < 20; i++) {
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

void
handle_msg(struct MetaInfo *mi, struct Peer *peer, struct PeerMsg *msg)
{
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
    }
}

/**
 * @brief 处理与 tracker 的交互
 * @param mi 种子文件元信息
 * @param tracker_idx tracker 在元信息中的下标
 *
 * 本函数不主动关闭套接字。
 */
void
tracker_handler(struct MetaInfo *mi, int tracker_idx)
{
    int sfd = send_msg_to_tracker(mi, tracker_idx, NULL);

    // 解析 HTTP 响应报文
    char response[BUF_SIZE] = { 0 };
    char *curr = response;
    long size = 0;
    while (read(sfd, curr, 1) == 1) {
        if (*curr++ != '\n') {
            continue;
        }
        printf("%s", response);

        if (!strncmp(response, "Content-Length", 14)) {
            size = strtol(response + 16, NULL, 10);
        }
        else if (!strcmp(response, "\r\n")) {
            break;
        }

        // Reset
        memset(response, 0, sizeof(response));
        curr = response;
    }

    char *data = calloc(size, sizeof(*data));
    if (read(sfd, data, size) < size) {
        perror("read");
    }

    close(sfd);  // 对 tracker 来说, 没必要维持长连接.

    /*
     * 解析 peers 列表, 异步连接 peer.
     */

    struct BNode *bcode = bparser(data);
    print_bcode(bcode, 0, 0);
    const struct BNode *peers = dfs_bcode(bcode, "peers");

    int efd = epoll_create1(0);
    if (efd == -1) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }
    struct epoll_event *events = calloc(100, sizeof(*events));

    struct {
        unsigned char ip[4];
        short port;
    } *p;

    for (int i = 0; i < peers->s_size; i += 6) {
        p = (void *)&peers->s_data[i];
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

    /*
     * 报文处理状态机
     */

    while (1) {
        int n = epoll_wait(efd, events, 100, -1);
        for (int i = 0; i < n; i++) {
            if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)) {
                /*
                 * 异步 connect 错误处理
                 */
                int result;
                socklen_t result_len = sizeof(result);
                if (getsockopt(events[i].data.fd, SOL_SOCKET, SO_ERROR, &result, &result_len) < 0) {
                    perror("getsockopt");
                }
                err("%d: %s", events[i].data.fd, strerror(result));
                close(events[i].data.fd);
                epoll_ctl(efd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
            }
            else if (events[i].events & EPOLLOUT) {
                /*
                 * connect 完成
                 * EPOLLOUT 表明套接字可写, 对于刚刚调用过 connect 的套接字来讲,
                 * 即意味着连接成功建立.
                 */
                struct sockaddr_in addr;
                socklen_t addrlen = sizeof(addr);
                getpeername(events[i].data.fd, (struct sockaddr *)&addr, &addrlen);
                log("%s is connected at %u", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
                send_handshake(events[i].data.fd, mi);
                log("handshaking with %s:%d", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
                events[i].events = EPOLLIN;
                epoll_ctl(efd, EPOLL_CTL_MOD, events[i].data.fd, &events[i]);
            }
            else if (events[i].events & EPOLLIN) {
                /*
                 * 接受并处理 BT 报文
                 *
                 * 关于握手报文与 BT 报文的区分方法:
                 *
                 *   握手报文和 BT 报文无法从数据格式上进行区分, 但是一个没有完成
                 *   握手的 peer 是不会发送 BT 报文的. 我们将完成握手的 peer 记录
                 *   在一个集合 peers 中(抽象). 对于当前的可读套接字, 如果它不在
                 *   peers 集合里, 则是没有完成握手的 peer, 那么它送来的数据, 只
                 *   可能是握手信息或者 FIN 报文.
                 */
                struct Peer *peer;
                if ((peer = get_peer_by_fd(mi, events[i].data.fd)) == NULL) {
                    /*
                     * 完成握手过程
                     */
                    PeerHandShake handshake = {};
                    ssize_t nr_read = read(events[i].data.fd, &handshake, sizeof(handshake));
                    if (nr_read == 0) {
                        struct sockaddr_in addr;
                        socklen_t addrlen = sizeof(addr);
                        getpeername(events[i].data.fd, (struct sockaddr *)&addr, &addrlen);
                        log("handshaking failed, disconnect %s:%u", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
                        epoll_ctl(efd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                        close(events[i].data.fd);
                    }
                    else {
                        struct Peer *peer = peer_new(events[i].data.fd, mi->nr_pieces);
                        add_peer(mi, peer);
                        log("successfully handshake with %s:%u", peer->ip, peer->port);
                    }
                }
                else {
                    /*
                     * 处理具体的 BT 报文
                     *
                     * 虽然有多个 BT 报文凑到一个 TCP 报文段里的情况, 但是这里只处理一个报文.
                     * 由于报文变长, 所以要注意保持数据的一致性.
                     */
                    log("handling %s:%u :", peer->ip, peer->port);
                    struct PeerMsg *msg = peer_get_packet(peer);
                    if (msg == NULL) {
                        log("remove peer %s:%d", peer->ip, peer->port);
                        epoll_ctl(efd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                        close(peer->fd);
                        del_peer_by_fd(mi, peer->fd);
                    }
                    handle_msg(mi, peer, msg);
                    free(msg);
                }
            }

            puts("===============================================================");
        }
        if (n == 0) {
            break;
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
    long size = ftell(fp);
    char *bcode = calloc(size, sizeof(*bcode));

    rewind(fp);
    if (fread(bcode, 1, size, fp) < size) {
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
    make_info_hash(ast, mi->info_hash);

    printf("info_hash: ");
    print_hash(mi->info_hash, "\n");

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
