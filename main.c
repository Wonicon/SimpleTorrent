/**
 * @file bittorrent.c
 * @brief This module handles bittorrent protocol
 */

#include "butil.h"
#include "util.h"
#include "peer.h"
#include "connect.h"
#include <sys/epoll.h>    // epoll_create1(), epoll_ctl(), epoll_wait(), epoll_event
#include <arpa/inet.h>    // inet_ntoa()
#include <sys/timerfd.h>  // timerfd_settime()
#include <signal.h>       // sigaction()
#include <unistd.h>

/**
 * @brief 报文缓冲区大小
 */
#define BUF_SIZE 4096

/**
 * @brief global metainfo, describing the current downloading task.
 */
struct MetaInfo *mi = NULL;

void bt_handler(struct MetaInfo *mi, int efd);

void send_msg_to_tracker(struct MetaInfo *mi, struct Tracker *tracker);

/**
 * @brief send stopped message to trackers when exit from SIGINT
 */
void exit_handler(int signum)
{
    if (mi == NULL || mi->trackers == NULL) {
        err("either meta info or trackers are not constructed");
        exit(EXIT_FAILURE);
    }

    // Indicate STOPPED event!
    mi->downloaded = mi->left = mi->file_size;

    int efd = epoll_create1(0);
    if (efd == -1) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    // 异步连接 tracker
    int nr_trackers = 0;
    for (int i = 0; i < mi->nr_trackers; i++) {
        struct Tracker *tracker = &mi->trackers[i];
        if (tracker->timerfd > 0) {
            // only connect accessible tracker.
            // @todo this solution ignores the tracker sent the start message which hasn't response.
            nr_trackers++;
            async_connect_to_tracker(tracker, efd);
        }
    }

    struct epoll_event events[10];
    while (nr_trackers > 0) {
        int n = epoll_wait(efd, events, 10, -1);
        for (int i = 0; i < n; i++) {
            typeof(events[i].events) event = events[i].events;
            int fd = events[i].data.fd;
            if (event & EPOLLOUT) {
                struct Tracker *tracker;
                if ((tracker = get_tracker_by_fd(mi, fd)) != NULL) {
                    send_msg_to_tracker(mi, tracker);
                    nr_trackers--;
                    epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL);
                }
                else {
                    err("fd %d is not a tracker", fd);
                }
            }
            else if (event * (EPOLLERR | EPOLLHUP)) {
                err("fd %d err", fd);
            }
        }
    }

    close(efd);
    exit(EXIT_SUCCESS);
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

/**
 * @brief 入口函数，完成种子文件的解析，全局信息的生成以及进入消息处理逻辑。
 */
int
main(int argc, char *argv[])
{
    if (argc < 3) {
        printf("Usage: %s <torrent> <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    setbuf(stdout, NULL);

    // Register SIGINT callback
    struct sigaction act = {
        .sa_handler = exit_handler,
    };
    if (sigaction(SIGINT, &act, NULL) == -1) {
        perror("sigaction");
    }

    // 解析种子文件
    char *bcode = get_torrent_data_from_file(argv[1]);
    struct BNode *ast = bparser(bcode);
    puts("Parsed Bencode:");
    print_bcode(ast, 0, 0);

    // 创建并初始化 MetaInfo 对象
    mi = calloc(1, sizeof(*mi));

    // 计算 info hash, 此后不需要源数据
    make_info_hash(ast, mi->info_hash);
    free(bcode);

    // 提取相关信息
    extract_trackers(mi, ast);
    extract_pieces(mi, ast);
    metainfo_load_file(mi, ast);

    // 不再需要种子信息
    free_bnode(&ast);

    // 创建用于定时发送 keep-alive 消息的定时器
    mi->timerfd = timerfd_create(CLOCK_REALTIME, 0);
    log("mi timer FD %d", mi->timerfd);
    struct itimerspec ts = { { 60, 0 }, { 60, 0} };  // 首次超时 1min, 持续间隔 1min.
    timerfd_settime(mi->timerfd, 0, &ts, NULL);

    // 创建侦听 peer 的套接字
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd == -1) {
        perror("create listen socket");
        exit(EXIT_FAILURE);
    }
    mi->port = (uint16_t)atoi(argv[2]);
    struct sockaddr_in addr = {
        .sin_addr.s_addr = INADDR_ANY,
        .sin_family = AF_INET,
        .sin_port = htons(mi->port),
    };
    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind listen socket");
    }
    if (listen(sfd, 0) == -1) {
        perror("listen socket");
    }
    mi->listen_fd = sfd;
    log("listen fd %d", sfd);

    // 输出相关信息

    printf("info_hash: ");
    for (int i = 0; i < HASH_SIZE; i++) {
        printf("%02x", mi->info_hash[i]);
    }
    printf("\n");

    puts("Tracker list:");
    for (int i = 0; i < mi->nr_trackers; i++) {
        printf("%d. %s://%s:%s%s\n", i,
               mi->trackers[i].method,
               mi->trackers[i].host,
               mi->trackers[i].port,
               mi->trackers[i].request);
    }

    int efd = epoll_create1(0);
    if (efd == -1) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    // 侦听定时事件
    struct epoll_event ev = {
        .data.fd = mi->timerfd,
        .events = EPOLLIN
    };
    epoll_ctl(efd, EPOLL_CTL_ADD, mi->timerfd, &ev);

    // 侦听连接请求
    ev.data.fd = mi->listen_fd;
    ev.events = EPOLLIN;
    epoll_ctl(efd, EPOLL_CTL_ADD, mi->listen_fd, &ev);

    // 异步连接 tracker
    for (int i = 0; i < mi->nr_trackers; i++) {
        struct Tracker *tracker = &mi->trackers[i];
        async_connect_to_tracker(tracker, efd);
    }

    // 开始侦听，处理事件
    bt_handler(mi, efd);

    free_metainfo(&mi);

    return 0;
}
