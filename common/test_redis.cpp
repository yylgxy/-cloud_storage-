#include <iostream>
#include "redis_utils.h"
#include <cstring> // strcmp函数需要这个头文件

using namespace std;

int main() {
    RedisUtils* redis = RedisUtils::getInstance();

    // 初始化Redis信息（默认无密码，端口6379，数据库0）
    redis->initInfo("127.0.0.1");

    // 连接Redis
    if (redis->connect()) {
        cout << " Redis 连接成功！" << endl;
    } else {
        cout << " Redis 连接失败：" << redis->getError() << endl;
        RedisUtils::destroyInstance();
        return -1;
    }

    // 测试SET命令
    redisReply* reply = redis->command("SET test_key hello_redis");
    if (reply && reply->type == REDIS_REPLY_STATUS && strcmp(reply->str, "OK") == 0) {
        cout << " SET 命令执行成功" << endl;
    } else {
        cout << " SET 命令失败：" << redis->getError() << endl;
    }
    redis->freeReply(reply);

    // 测试GET命令
    reply = redis->command("GET test_key");
    if (reply && reply->type == REDIS_REPLY_STRING) {
        cout << " GET 结果：" << reply->str << endl;
    } else {
        cout << " GET 命令失败：" << redis->getError() << endl;
    }
    redis->freeReply(reply);

    // 测试DEL命令
    reply = redis->command("DEL test_key");
    if (reply && reply->type == REDIS_REPLY_INTEGER && reply->integer == 1) {
        cout << " DEL 命令执行成功" << endl;
    } else {
        cout << " DEL 命令失败：" << redis->getError() << endl;
    }
    redis->freeReply(reply);

    // 销毁实例
    redis = nullptr;
    RedisUtils::destroyInstance();
    cout << endl << " Redis 工具类测试完成！" << endl;

    return 0;
}