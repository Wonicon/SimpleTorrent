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

#endif  // UTIL_H
