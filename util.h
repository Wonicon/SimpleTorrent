/**
 * @file util.h
 * @brief 一些工具函数（宏）声明 / 定义
 */

#ifndef UTIL_H
#define UTIL_H

#include <stdio.h>
#include <stdlib.h>

/**
 * @brief 严重错误，输出错误提示并退出程序
 */
#define panic(fmt, ...) do { fprintf(stderr, "[%s:%d] " fmt "\n", __FUNCTION__, __LINE__, ## __VA_ARGS__); exit(EXIT_FAILURE); } while(0)

/**
 * @brief 打印日志
 */
#define log(fmt, ...) \
    fprintf(stdout, "[%s:%s:%d] " fmt "\n", __FILE__, __FUNCTION__, __LINE__, ## __VA_ARGS__)

/**
 * @brief 打印错误
 */
#define err(fmt, ...) \
    fprintf(stderr, "[%s:%s:%d] " fmt "\n", __FILE__, __FUNCTION__, __LINE__, ## __VA_ARGS__)

#endif  // UTIL_H
