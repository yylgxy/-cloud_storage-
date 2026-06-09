#include "fcgi_stdio.h"
#include "mysql_utils.h"
#include "cjson_utils.h"
#include "md5_utils.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdlib>

using namespace std;

int main() {
    // 1. 程序启动时只初始化一次单例和数据库信息
    MySQLUtils* db = MySQLUtils::getInstance();
    db->initInfo("127.0.0.1", "root", MySQLUtils::getDbPassword(), "ai_cloud_storage", 3306);

    // 2. 循环处理所有请求 (FastCGI核心)
    while (FCGI_Accept() >= 0) {
        char response[2048] = {0};

        // 3. 每次请求都重新连接数据库
        if (!db->connect()) {
            snprintf(response, sizeof(response), 
                "{\"code\":-1,\"msg\":\"数据库连接失败：%s\"}", 
                db->getError().c_str());
            
            // 🔥 核心修复：全部改成 FCGI_printf
            FCGI_printf("Content-Type: application/json\r\n\r\n");
            FCGI_printf("%s", response);
            continue;
        }

        // 4. 解析请求体长度
        char* content_length_str = getenv("CONTENT_LENGTH");
        if (content_length_str == NULL) {
            snprintf(response, sizeof(response), 
                "{\"code\":-1,\"msg\":\"缺少请求体\"}");
            
            FCGI_printf("Content-Type: application/json\r\n\r\n");
            FCGI_printf("%s", response);
            db->disconnect();
            continue;
        }

        int content_length = atoi(content_length_str);
        char* request_body = new char[content_length + 1];
        // 读取请求体 (FCGI_fread 是正确的)
        FCGI_fread(request_body, 1, content_length, FCGI_stdin);
        request_body[content_length] = '\0';

        // 5. 解析JSON
        cJSON* root = cJSON_Parse(request_body);
        if (root == NULL) {
            snprintf(response, sizeof(response), 
                "{\"code\":-1,\"msg\":\"JSON格式错误\"}");
            
            FCGI_printf("Content-Type: application/json\r\n\r\n");
            FCGI_printf("%s", response);
            delete[] request_body;
            db->disconnect();
            continue;
        }

        cJSON* username_item = cJSON_GetObjectItem(root, "username");
        cJSON* password_item = cJSON_GetObjectItem(root, "password");
        cJSON* nickname_item = cJSON_GetObjectItem(root, "nickname");

        if (!username_item || !password_item || !nickname_item) {
            snprintf(response, sizeof(response), 
                "{\"code\":-1,\"msg\":\"缺少必要参数\"}");
            
            FCGI_printf("Content-Type: application/json\r\n\r\n");
            FCGI_printf("%s", response);
            cJSON_Delete(root);
            delete[] request_body;
            db->disconnect();
            continue;
        }

        const char* username = username_item->valuestring;
        const char* password = password_item->valuestring;
        const char* nickname = nickname_item->valuestring;

        // 6. 密码已由前端MD5加密，直接存储
        string password_md5 = password;

        // 7. 检查用户名
        char check_sql[1024];
        snprintf(check_sql, sizeof(check_sql), "SELECT id FROM user_info WHERE user_name = '%s'", username);
        MYSQL_RES* res = db->query(check_sql);

        if (res == NULL) {
            snprintf(response, sizeof(response), 
                "{\"code\":-1,\"msg\":\"查询用户失败：%s\"}", 
                db->getError().c_str());
            
            FCGI_printf("Content-Type: application/json\r\n\r\n");
            FCGI_printf("%s", response);
            cJSON_Delete(root);
            delete[] request_body;
            db->disconnect();
            continue;
        }

        if (mysql_num_rows(res) > 0) {
            snprintf(response, sizeof(response), 
                "{\"code\":-1,\"msg\":\"用户名已存在\"}");
            
            FCGI_printf("Content-Type: application/json\r\n\r\n");
            FCGI_printf("%s", response);
            db->freeResult(res);
            cJSON_Delete(root);
            delete[] request_body;
            db->disconnect();
            continue;
        }
        db->freeResult(res);

        // 8. 插入用户
        char insert_sql[1024];
        snprintf(insert_sql, sizeof(insert_sql), 
            "INSERT INTO user_info(user_name, password_md5, nickname) VALUES('%s', '%s', '%s')",
            username, password_md5.c_str(), nickname);

        int affected_rows = db->update(insert_sql);
        if (affected_rows < 0) {
            snprintf(response, sizeof(response), 
                "{\"code\":-1,\"msg\":\"注册失败：%s\"}", 
                db->getError().c_str());
        } else {
            snprintf(response, sizeof(response), 
                "{\"code\":0,\"msg\":\"注册成功！密码已MD5加密\"}");
        }

        // 9. 返回结果
        FCGI_printf("Content-Type: application/json\r\n\r\n");
        FCGI_printf("%s", response);

        // 10. 释放资源
        cJSON_Delete(root);
        delete[] request_body;
        db->disconnect();
    }

    MySQLUtils::destroyInstance();
    return 0;
}