/** 
 * @file connect.h
 * @brief 网络连接相关 API 声明
 */

#ifndef CONNECT_H
#define CONNECT_H

#include <sys/socket.h>

/**
 * @brief 标志一个 HTTP 请求的句柄
 */
struct HttpRequest;

/**
 * @brief 创建一个 HTTP 请求
 * @param method 请求方法, 主要用 GET
 * @param host   主机名
 * @return 动态分配的实例句柄
 */
struct HttpRequest *create_http_request(const char *method, const char *host);

/**
 * @brief 增加一个请求属性
 * @param req HTTP 请求句柄
 * @param key 属性键
 * @param fmt ... 属性值 [可格式化]
 */
void add_http_request_attr(struct HttpRequest *req, const char *key, const char *fmt, ...);

/**
 * @brief 发送一个 HTTP 请求
 * @param req HTTP 请求句柄
 * @param sfd 连接套接字
 * @return 成功返回 0, 失败返回 -1
 */
int send_http_request(struct HttpRequest *req, int sfd);

/**
 * @brief 解析 URL
 * @param url 指向 URL 字符串
 * @param method  [OUT] URL 使用的协议
 * @param host    [OUT] URL 的主机
 * @param port    [OUT] URL 的端口号
 * @param request [OUT] URL 剩余部分, 可用于 HTTP 报文的请求串
 */
void parse_url(const char *url, char *method, char *host, char *port, char *request);

/**
 * @brief 异步 connect
 * @param efd epoll 描述符
 * @param sfd 连接套接字
 * @param addr 连接地址
 * @param addrlen 地址结构体长度
 * @return 如果立即 connect 返回 0，异步连接时应该返回 errno，应当是 EINPROGRESS
 *
 * 如果 connect 能够立即完成，直接返回；否则，将描述符加入 epoll,
 * 侦听 EPOLLOUT 事件，同时要关注 EPOLLERR 和 EPOLLHUP 处理实际错误。
 */
int async_connect(int efd, int sfd, const struct sockaddr *addr, socklen_t addrlen);

/** @brief 异步地与 tracker 建立连接，调用后连接并不立即建立
 * @param host 主机名（域名或 IPv4 地址字符串）
 * @param port 端口号
 */
void async_connect_to_tracker(struct Tracker *tracker, int efd);

#endif  // CONNECT_H
