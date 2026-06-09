#include "fcgi_stdio.h"
#include "mysql_utils.h"
#include "redis_utils.h"
#include "cjson_utils.h"
#include "md5_utils.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>

using namespace std;

// 生成32位随机Token
string gen_32_token() {
    const string dict = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    string token;
    for(int i = 0; i < 32; i++){
        token += dict[rand() % dict.size()];
    }
    return token;
}

int main() {
    srand((unsigned)time(nullptr));
    // 初始化数据库
    MySQLUtils* db = MySQLUtils::getInstance();
    db->initInfo("127.0.0.1", "root", "CHANGE_ME", "ai_cloud_storage", 3306);

    // 初始化Redis
    RedisUtils* redis = RedisUtils::getInstance();
    redis->initInfo("127.0.0.1", 6379);
    redis->connect();

    // FastCGI 循环处理请求
    while (FCGI_Accept() >= 0) {
        char resp[2048] = {0};

        // 1. 获取请求体长度
        char* content_len = getenv("CONTENT_LENGTH");
        if(!content_len) {
            snprintf(resp, sizeof(resp), "{\"code\":1,\"msg\":\"缺少请求体\"}");
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            continue;
        }

        int len = atoi(content_len);
        char* req_body = new char[len + 1];
        FCGI_fread(req_body, 1, len, FCGI_stdin);
        req_body[len] = '\0';

        // 2. 解析JSON
        cJSON* root = cJSON_Parse(req_body);
        if(!root) {
            snprintf(resp, sizeof(resp), "{\"code\":1,\"msg\":\"JSON格式错误\"}");
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            delete[] req_body;
            continue;
        }

        cJSON* username = cJSON_GetObjectItem(root, "username");
        cJSON* password = cJSON_GetObjectItem(root, "password");

        if(!username || !password) {
            snprintf(resp, sizeof(resp), "{\"code\":1,\"msg\":\"缺少参数\"}");
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            cJSON_Delete(root);
            delete[] req_body;
            continue;
        }

        const char* user_name = username->valuestring;
        const char* user_pwd = password->valuestring;

        // 3. 连接数据库查询用户
        if(!db->connect()) {
            snprintf(resp, sizeof(resp), "{\"code\":1,\"msg\":\"数据库连接失败\"}");
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            cJSON_Delete(root);
            delete[] req_body;
            continue;
        }

        char sql[1024];
        snprintf(sql, sizeof(sql), "SELECT id, password_md5 FROM user_info WHERE user_name = '%s'", user_name);
        MYSQL_RES* res = db->query(sql);

        // 用户不存在
        if(!res || mysql_num_rows(res) == 0) {
            snprintf(resp, sizeof(resp), "{\"code\":2,\"msg\":\"用户不存在\"}");
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            db->freeResult(res); db->disconnect();
            cJSON_Delete(root); delete[] req_body;
            continue;
        }

        MYSQL_ROW row = mysql_fetch_row(res);
        const char* db_pwd = row[1];

        // 密码错误
        if(strcmp(user_pwd, db_pwd) != 0) {
            snprintf(resp, sizeof(resp), "{\"code\":3,\"msg\":\"密码错误\"}");
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            db->freeResult(res); db->disconnect();
            cJSON_Delete(root); delete[] req_body;
            continue;
        }

        // 4. 登录成功，生成Token
        string token = gen_32_token();
        int user_id = atoi(row[0]);

        // ====================== 修复：定义变量 + 存储Redis ======================
        char redis_key[128] = {0};  // 补上变量定义
        char redis_cmd[256] = {0};  // 补上变量定义

        // 直接用 token 作为 Redis Key
        snprintf(redis_key, sizeof(redis_key), "%s", token.c_str());
        
        // 存储：key=token value=user_id
        snprintf(redis_cmd, sizeof(redis_cmd), "SET %s %d", redis_key, user_id);
        redisReply* reply = redis->command(redis_cmd);
        redis->freeReply(reply);

        // 设置过期时间 2小时
        snprintf(redis_cmd, sizeof(redis_cmd), "EXPIRE %s 7200", redis_key);
        reply = redis->command(redis_cmd);
        redis->freeReply(reply);
        // ======================================================================

        // 6. 返回成功响应
        snprintf(resp, sizeof(resp), "{\"code\":0,\"msg\":\"登录成功\",\"token\":\"%s\"}", token.c_str());
        FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);

        // 释放资源
        db->freeResult(res); db->disconnect();
        cJSON_Delete(root); delete[] req_body;
    }

    // 销毁单例
    MySQLUtils::destroyInstance();
    RedisUtils::destroyInstance();
    return 0;
}