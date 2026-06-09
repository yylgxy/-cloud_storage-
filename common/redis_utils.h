#ifndef REDIS_UTILS_H
#define REDIS_UTILS_H

#include <hiredis/hiredis.h>
#include <string>

class RedisUtils {
private:
    static RedisUtils* instance;  // 单例实例指针
    redisContext* redis;          // Redis连接句柄

    std::string host;             // Redis主机地址
    unsigned int port = 6379;     // Redis端口
    std::string password;         // Redis密码（无密码留空）
    int db = 0;                       // Redis数据库号（默认0）

    // 禁止拷贝和赋值
    RedisUtils(const RedisUtils&) = delete;
    RedisUtils& operator=(const RedisUtils&) = delete;

public:
    // 构造函数和析构函数公开，允许线程局部实例
    RedisUtils();
    ~RedisUtils();
    // 获取单例实例
    static RedisUtils* getInstance();

    // 初始化Redis连接信息
    void initInfo(const std::string& host,
                 unsigned int port = 6379,
                 const std::string& password = "",
                 int db = 0);

    // 连接Redis
    bool connect();

    // 执行任意Redis命令（返回结果指针，用完必须调用freeReply释放）
    redisReply* command(const std::string& cmd);

    // 释放命令执行结果
    void freeReply(redisReply* reply);

    // 关闭Redis连接
    void disconnect();

    // 获取错误信息
    std::string getError() const;

    // 销毁单例实例
    static void destroyInstance();
};

#endif