/**
 * @file connect.c
 * @brief 网络连接相关 API 实现
 */

#include "util.h"
#include "metainfo.h"
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <pthread.h>

/**
 * @brief 解析 url 获取应用层协议、主机名、端口号
 * @param url 指向 url 字符串
 * @param method 接收应用层协议名
 * @param host 接收主机名
 * @param port 接收端口号
 * @param request http 请求路径，一般是 /announce
 *
 * 自行保证缓冲区大小，host 和 port 可以使用 NI_MAXHOST 和 NI_MAXSERV.
 * method 一般是 http 或者 udp.
 */
void
parse_url(const char *url, char *method, char *host, char *port, char *request)
{
    char *curr;

    // method: "????://hostname:port/request"
    curr = strstr(url, "://");
    strncpy(method, url, curr - url);
    method[curr - url] = '\0';
    url = curr + 3;

    // hostname: "hostname[:port][/[reqeust]]"
    // TODO ":" 是否会出现在 "/" 后面？
    if ((curr = strstr(url, ":")) != NULL) {
        // hostname:port/request
        strncpy(host, url, curr - url);
        host[curr - url] = '\0';
        url = curr + 1;

        if ((curr = strstr(url, "/")) != NULL) {
            // port/request
            strncpy(port, url, curr - url);
            port[curr - url] = '\0';
            url = curr;
        }
        else {
            // port
            strcpy(port, url);
            url = "/";
        }
    }
    else if ((curr = strstr(url, "/")) != NULL) {
        // hostname/request
        strncpy(host, url, curr - url);
        host[curr - url] = '\0';
        strcpy(port, "80");
        url = curr;
    }
    else {
        // hostname
        strcpy(host, url);
        strcpy(port, "80");
        url = "/";
    }

    strcpy(request, url);
}

/**
 * @brief HTTP 报文缓冲区的最大长度
 */
#define REQUEST_MAX 1024

/**
 * @brief 描述一个 HTTP 请求
 *
 * buf 存储要发送的报文，add_http_request_addr() 以流的形式向报文里追加表项。
 * 每填一个表项，curr 都会前进相应的字节。
 */
struct HttpRequest
{
    char buf[REQUEST_MAX];  ///< 请求报文缓冲区。
    char *curr;             ///< 指向报文缓冲区未填写部分的开头。
    char *delim;            ///< 请求表单的分割符，最开始以 ? 分割请求和表单，之后用 & 分割表项。
};

struct HttpRequest *
create_http_request(const char *method, const char *host)
{
    struct HttpRequest *req = calloc(1, sizeof(*req));
    int n;
    sprintf(req->buf, "%s %s%n", method, host, &n);
    req->curr = req->buf + n;
    req->delim = "?";
    return req;
}

void
add_http_request_attr(struct HttpRequest *req, const char *key, const char *fmt, ...)
{
    req->curr += sprintf(req->curr, "%s%s=", req->delim, key);
    req->delim = "&";

    va_list args;
    va_start(args, fmt);
    req->curr += vsprintf(req->curr, fmt, args);
    va_end(args);
}

int
send_http_request(struct HttpRequest *req, int sfd)
{
    sprintf(req->curr, " HTTP/1.1\r\n\r\n");
    printf("request: %s", req->buf);
    size_t size = strlen(req->buf);
    if (write(sfd, req->buf, size) < size) {
        perror("send http request");
        return -1;
    }
    return 0;
}

/**
 * @brief 将套接字设置成非阻塞的
 * @param sfd 套接字
 * @return 成功返回 0，错误返回 -1.
 */
int
make_nonblocking(int sfd)
{
    int flags = fcntl(sfd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl to get sfd flags");
        return -1;
    }

    flags |= O_NONBLOCK;
    if (fcntl(sfd, F_SETFL, flags) == -1) {
        perror("fcntl to set sfd flags");
        return -1;
    }

    return 0;
}

/**
 * @brief 将套接字设置成阻塞的
 * @param sfd 套接字
 * @return 成功返回 0，错误返回 -1.
 */
int
make_blocking(int sfd)
{
    int flags = fcntl(sfd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl to get sfd flags");
        return -1;
    }

    flags ^= O_NONBLOCK;
    if (fcntl(sfd, F_SETFL, flags) == -1) {
        perror("fcntl to set sfd flags");
        return -1;
    }

    return 0;
}

int
async_connect(int efd, int sfd, const struct sockaddr *addr, socklen_t addrlen)
{
    make_nonblocking(sfd);
    int s = connect(sfd, addr, addrlen);
    make_blocking(sfd);
    if (s == 0) {
        return 0;
    }
    else {
        struct epoll_event ev = {
            .data.fd = sfd,
            .events = EPOLLOUT
        };
        if (epoll_ctl(efd, EPOLL_CTL_ADD, sfd, &ev) == -1) {
            perror("epoll_ctl");
        }
        return errno;
    }
}

/** @brief 异步连接线程
 *
 * 本函数主要是为了规避 getaddrinfo 的阻塞，一个重要的隐含前提是通过 tracker 的 sfd 来传递 efd。
 * 毕竟 sfd 作为连接套接字在调用时是没有意义的。
 *
 * @param arg 实际上是指向 tracker 的指针
 * @return NULL
 */
void *
async_connect_to_tracker_non_block(void *arg)
{
    struct Tracker *tracker = arg;
    const char *host = tracker->host;
    const char *port = tracker->port;
    int efd = tracker->sfd;

    // 过滤出 IPv4 地址，只使用 TCP 连接。
    // 如果通过 DNS 查询，会有 IPv6 地址占据前列，浪费时间等待 connect 失败。
    struct addrinfo hints = {
            .ai_family = AF_INET,
            .ai_socktype = SOCK_STREAM,
            .ai_flags = 0,
            .ai_protocol = 0,
            .ai_addr = NULL,
            .ai_addrlen = 0
    };

    struct addrinfo *result;
    int s = getaddrinfo(host, port, &hints, &result);
    if (s != 0) {
        fprintf(stderr, "getaddrinfo(%s:%s): %s\n", host, port, gai_strerror(s));
        return NULL;
    }

    int sfd = -1;  // socket file descriptor
    for (struct addrinfo *rp = result; rp != NULL; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == -1) {
            perror("socket");
            continue;
        }

        if (sfd == 0) {
            log("fuck");
            exit(-1);
        }

        // Assign socket before connecting to avoid hazard.
        tracker->sfd = sfd;
        if (async_connect(efd, sfd, rp->ai_addr, rp->ai_addrlen) == EINPROGRESS) {
            log("tracker %s fd %d", tracker->host, sfd);
            break;
        }

        perror("connect to tracker");
        close(sfd);
    }

    freeaddrinfo(result);

    if (sfd == -1) {
        fprintf(stderr, "Could not connect.\n");
        return NULL;
    }

    return NULL;
}

void
async_connect_to_tracker(struct Tracker *tracker, int efd)
{
    printf("connecting to %s:%s\n", tracker->host, tracker->port);
    pthread_t tid;
    tracker->sfd = efd;  // 隐含前提，简化参数传递
    pthread_create(&tid, NULL, async_connect_to_tracker_non_block, tracker);
}
