#include "redis_utils.h"
#include <cstring>

using namespace std;

// 初始化静态单例指针
RedisUtils* RedisUtils::instance = nullptr;

// 构造函数
RedisUtils::RedisUtils() : redis(nullptr) {}

// 析构函数
RedisUtils::~RedisUtils() {
    disconnect();
}

// 获取单例
RedisUtils* RedisUtils::getInstance() {
    if (instance == nullptr) {
        instance = new RedisUtils();
    }
    return instance;
}

// 初始化连接信息
void RedisUtils::initInfo(const string& host, unsigned int port, const string& password, int db) {
    this->host = host;
    this->port = port;
    this->password = password;
    this->db = db;
}

// 连接Redis
bool RedisUtils::connect() {
    // 如果已有连接，先关闭
    if (redis != nullptr) {
        disconnect();
    }

    // 建立连接（超时时间1.5秒）
    struct timeval timeout = {1, 500000};
    redis = redisConnectWithTimeout(host.c_str(), port, timeout);

    // 连接失败
    if (redis == nullptr || redis->err) {
        if (redis) {
            redisFree(redis);
            redis = nullptr;
        }
        return false;
    }

    // 如果有密码，执行认证
    if (!password.empty()) {
        const char* args[2] = {"AUTH", password.c_str()};
        size_t argvlen[2] = {4, password.size()};
        redisReply* reply = (redisReply*)redisCommandArgv(redis, 2, args, argvlen);
        if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
            freeReply(reply);
            disconnect();
            return false;
        }
        freeReply(reply);
    }

    // 选择数据库
    redisReply* reply = (redisReply*)redisCommand(redis, "SELECT %d", db);
    if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
        freeReply(reply);
        disconnect();
        return false;
    }
    freeReply(reply);

    return true;
}

// 执行Redis命令
redisReply* RedisUtils::command(const string& cmd) {
    if (redis == nullptr) {
        return nullptr;
    }
    return (redisReply*)redisCommand(redis, cmd.c_str());
}

// 释放命令结果
void RedisUtils::freeReply(redisReply* reply) {
    if (reply != nullptr) {
        freeReplyObject(reply);
    }
}

// 关闭连接
void RedisUtils::disconnect() {
    if (redis != nullptr) {
        redisFree(redis);
        redis = nullptr;
    }
}

// 获取错误信息
string RedisUtils::getError() const {
    if (redis == nullptr) {
        return "Not connected to Redis";
    }
    if(redis->err) {
        return redis->errstr ? string(redis->errstr) : "Unknown error";
    }
    return "No error";
}

// 销毁单例
void RedisUtils::destroyInstance() {
    if (instance != nullptr) {
        delete instance;
        instance = nullptr;
    }
}