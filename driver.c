#include "bparser.h"
#include "butil.h"
#include "util.h"
#include "peer.h"
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/epoll.h>
void bp() {}
struct HttpRequest;
struct HttpRequest *create_http_request(const char *method, const char *host);
void add_http_request_attr(struct HttpRequest *req, const char *key, const char *fmt, ...);
int send_http_request(struct HttpRequest *req, int sfd);
void parse_url(const char *url, char *method, char *host, char *port, char *request);
int async_connect(int efd, int sfd, const struct sockaddr *addr, socklen_t addrlen);
int make_blocking(int sfd);

/**
 * @brief 提取 tracker 列表
 * @param mi 存储 tracker 列表
 * @param ast B 编码语法树
 */
void extract_trackers(struct MetaInfo *mi, const struct BNode *ast)
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

        printf("%d trackers\n", mi->nr_trackers);

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

void extract_pieces(struct MetaInfo *mi, const struct BNode *ast)
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
    }

    printf("filesz %ld, piecesz %ld, nr pieces %d\n", mi->file_size, mi->piece_size, mi->nr_pieces);

    const struct BNode *pieces_node = dfs_bcode(ast, "pieces");
    if (pieces_node) {
        mi->pieces = calloc(mi->nr_pieces, sizeof(*mi->pieces));
        const char *hash = pieces_node->s_data;
        for (int i = 0; i < mi->nr_pieces; i++) {
            memcpy(mi->pieces[i], hash, sizeof(*mi->pieces));
            hash += sizeof(*mi->pieces);
        }
    }
}

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

int
connect_to_tracker(const char *host, const char *port);

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
    puts("written");

};

#define BUF_SIZE 4096
/**
 * @brief 处理与 tracker 的交互
 * @param sfd 与tracker 的连接的套接字
 * @param mi 种子文件元信息
 * @param tracker_idx tracker 在元信息中的下标
 *
 * 本函数不主动关闭套接字。
 */
void
tracker_handler(int sfd, struct MetaInfo *mi, int tracker_idx)
{
    struct Tracker *tracker = &mi->trackers[tracker_idx];

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

    send_http_request(req, sfd);

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
        printf("fd %d <> %d.%d.%d.%d:%d\n", fd, p->ip[0], p->ip[1], p->ip[2], p->ip[3], htons(p->port));
        struct sockaddr_in sa;
        sa.sin_family = AF_INET;
        memcpy(&sa.sin_addr.s_addr, p->ip, 4);
        memcpy(&sa.sin_port, &p->port, 2);
        if (async_connect(efd, fd, (void *)&sa, sizeof(sa))) {
            perror("async");
        }
        else {
            exit(EXIT_FAILURE);
        }
    }

    while (1) {
        int n = epoll_wait(efd, events, 100, -1);
        for (int i = 0; i < n; i++) {
            if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)) {

                int result;
                socklen_t result_len = sizeof(result);
                if (getsockopt(events[i].data.fd, SOL_SOCKET, SO_ERROR, &result, &result_len) < 0) {
                    perror("getsockopt");
                }
                fprintf(stderr, "%d: %s\n", events[i].data.fd, strerror(result));

                epoll_ctl(efd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
            }
            else if (events[i].events & EPOLLOUT) {
                printf("%d is connected\n", events[i].data.fd);
                make_blocking(events[i].data.fd);
                send_handshake(events[i].data.fd, mi);
                events[i].events = EPOLLIN;
                epoll_ctl(efd, EPOLL_CTL_MOD, events[i].data.fd, &events[i]);
            }
            else if (events[i].events & EPOLLIN) {
                PeerHandShake handshake = {};
                printf("read fd %d\n", events[i].data.fd);
                ssize_t nr_read = read(events[i].data.fd, &handshake, sizeof(handshake));
                if (nr_read < 0) {
                    perror("read");
                }
                else if (nr_read == 0) {
                    printf("%d disconnected\n", events[i].data.fd);
                }
                else {
                    puts("read");
                    printf("peer-id: %.20s\n", handshake.hs_peer_id);
                }
                epoll_ctl(efd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
            }
        }
        if (n == 0) {
            break;
        }
    }

    free_bnode(&bcode);
    free(data);
}

int
main(int argc, char *argv[])
{
    struct MetaInfo *mi = calloc(1, sizeof(*mi));

    char *bcode = get_torrent_data_from_file(argv[1]);
    struct BNode *ast = bparser(bcode);
    print_bcode(ast, 0, 0);

    extract_trackers(mi, ast);
    extract_pieces(mi, ast);
    make_info_hash(ast, mi->info_hash);
    printf("info_hash: ");
    print_hash(mi->info_hash, "\n");

    char cmd[128];
    for (int i = 0; i < mi->nr_trackers; i++) {
        printf("Use tracker %s://%s:%s%s ? (y/N/q): ",
               mi->trackers[i].method, mi->trackers[i].host, mi->trackers[i].port, mi->trackers[i].request);
        fflush(stdout);
        fgets(cmd, sizeof(cmd), stdin);

        if (!strcmp(cmd, "q\n")) {
            break;
        }

        if (strcmp(cmd, "y\n")) {  // not "y"
            continue;
        }

        int sfd = connect_to_tracker(mi->trackers[i].host, mi->trackers[i].port);
        if (sfd != -1) {
            tracker_handler(sfd, mi, i);
            close(sfd);
        }
    }

    free(bcode);
    free_bnode(&ast);
    free_metainfo(&mi);
}
