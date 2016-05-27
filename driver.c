#include "bparser.h"
#include "butil.h"
#include "util.h"
#include <string.h>
#include <unistd.h>
#include <netdb.h>

struct HttpRequest;
struct HttpRequest *create_http_request(const char *method, const char *host);
void add_http_request_attr(struct HttpRequest *req, const char *key, const char *fmt, ...);
int send_http_request(struct HttpRequest *req, int sfd);
void parse_url(const char *url, char *method, char *host, char *port, char *request);

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
            char *p = response + 16;
            size = strtol(p, &p, 10);
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
    struct {
        unsigned char ip[4];
        short port;
    } *p;
    for (int i = 0; i < peers->s_size; i += 6) {
        p = (void *)&peers->s_data[i];
        printf("%d.%d.%d.%d:%d\n", p->ip[0], p->ip[1], p->ip[2], p->ip[3], htons(p->port));
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa;
        sa.sin_family = AF_INET;
        memcpy(&sa.sin_addr.s_addr, p->ip, 4);
        memcpy(&sa.sin_port, &p->port, 2);
        if (connect(fd, (void *)&sa, sizeof(sa))) {
            perror("connect");
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
