#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <netdb.h>

/**
 * @brief 与 tracker 建立连接
 * @param host 主机名（域名或 IPv4 地址字符串）
 * @param port 端口号
 * @return 连接套接字，连接失败返回 -1
 */
int
connect_to_tracker(const char *host, const char *port)
{
    printf("connecting to %s:%s\n", host, port);

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
        return -1;
    }

    int sfd = -1;  // socket file descriptor
    struct addrinfo *rp;  // iterator for result list
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == -1) {
            perror("");
            continue;
        }

        if (connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1) {
            printf("connected to %s:%s\n", host, port);
            break;
        }

        perror("connect");
        close(sfd);
    }

    if (rp == NULL) {
        fprintf(stderr, "Could not connect.\n");
        return -1;
    }

    freeaddrinfo(result);
    return sfd;
}

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

#define REQUEST_MAX 1024

struct HttpRequest
{
    char buf[REQUEST_MAX];
    char *curr;
    char *delim;
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
    size_t size = strlen(req->buf);
    if (write(sfd, req->buf, size) < size) {
        perror("send http request");
        return -1;
    }
    return 0;
}
