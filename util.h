#ifndef UTIL_H
#define UTIL_H

#include <stdio.h>
#include <stdlib.h>

/**
 * @brief 严重错误，输出错误提示并退出程序
 */
#define panic(fmt, ...) do { fprintf(stderr, "[%s:%d] " fmt "\n", __FUNCTION__, __LINE__, ## __VA_ARGS__); exit(EXIT_FAILURE); } while(0)

#define log(fmt, ...) \
    fprintf(stdout, "[%s:%s:%d] " fmt "\n", __FILE__, __FUNCTION__, __LINE__, ## __VA_ARGS__)

#define err(fmt, ...) \
    fprintf(stderr, "[%s:%s:%d] " fmt "\n", __FILE__, __FUNCTION__, __LINE__, ## __VA_ARGS__)

/**
 * @brief 打印 20 字节的 sha1 hash
 * param data 指向 hash 的缓冲区
 * param tail 输出后的追加字符串，可用于换行之类的
 */
static inline void
print_hash(void *data, const char *tail)
{
    unsigned char *hashdata = data;
    for (int i = 0; i < 20; i++) {
        printf("%02x", hashdata[i]);
    }
    printf("%s", tail);
}

#endif  // UTIL_H
