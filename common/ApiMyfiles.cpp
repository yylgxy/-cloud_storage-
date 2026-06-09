#include "fcgi_stdio.h"
#include "mysql_utils.h"
#include "redis_utils.h"
#include "cjson_utils.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace std;

int main() {
    RedisUtils* redis = RedisUtils::getInstance();
    redis->initInfo("127.0.0.1", 6379);
    redis->connect();

    while (FCGI_Accept() >= 0) {
        char resp[4096] = {0};
        char* content_len = getenv("CONTENT_LENGTH");

        if (!content_len) {
            snprintf(resp, sizeof(resp), "{\"code\":1,\"msg\":\"缺少请求体\"}");
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            continue;
        }

        int len = atoi(content_len);
        char* req_body = new char[len + 1];
        FCGI_fread(req_body, 1, len, FCGI_stdin);
        req_body[len] = '\0';

        cJSON* root = cJSON_Parse(req_body);
        if (!root) {
            snprintf(resp, sizeof(resp), "{\"code\":1,\"msg\":\"JSON格式错误\"}");
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            delete[] req_body;
            continue;
        }

        cJSON* token_item = cJSON_GetObjectItem(root, "token");
        cJSON* cmd_item = cJSON_GetObjectItem(root, "cmd");

        if (!token_item || !cmd_item) {
            snprintf(resp, sizeof(resp), "{\"code\":1,\"msg\":\"缺少必要参数\"}");
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            cJSON_Delete(root);
            delete[] req_body;
            continue;
        }

        const char* token = token_item->valuestring;
        const char* cmd = cmd_item->valuestring;

        // 验证Token
        char redis_key[128], redis_cmd[256];
        snprintf(redis_key, sizeof(redis_key), "%s", token);
        snprintf(redis_cmd, sizeof(redis_cmd), "GET %s", redis_key);
        redisReply* reply = redis->command(redis_cmd);

        if (reply == nullptr || reply->type != REDIS_REPLY_STRING) {
            snprintf(resp, sizeof(resp), "{\"code\":4,\"msg\":\"Token无效或已过期\"}");
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            redis->freeReply(reply);
            cJSON_Delete(root);
            delete[] req_body;
            continue;
        }

        int user_id = atoi(reply->str);
        redis->freeReply(reply);

        // 查询用户名
        string user_name;
        {
            MySQLUtils* db = MySQLUtils::getInstance();
            db->initInfo("127.0.0.1", "root", "CHANGE_ME", "ai_cloud_storage", 3306);
            if (db->connect()) {
                char sql[1024];
                snprintf(sql, sizeof(sql), "SELECT user_name FROM user_info WHERE id = %d LIMIT 1", user_id);
                MYSQL_RES* res = db->query(sql);
                if (res && mysql_num_rows(res) > 0) {
                    MYSQL_ROW row = mysql_fetch_row(res);
                    user_name = row[0];
                }
                db->freeResult(res);
                db->disconnect();
            }
            MySQLUtils::destroyInstance();
        }

        if (user_name.empty()) {
            snprintf(resp, sizeof(resp), "{\"code\":1,\"msg\":\"用户不存在\"}");
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            cJSON_Delete(root);
            delete[] req_body;
            continue;
        }

        // 处理count命令 - 返回文件数、总下载量、存储空间
        if (strcmp(cmd, "count") == 0) {
            int count = 0, total_pv = 0;
            long long total_storage = 0;
            {
                MySQLUtils* db = MySQLUtils::getInstance();
                db->initInfo("127.0.0.1", "root", "CHANGE_ME", "ai_cloud_storage", 3306);
                if (db->connect()) {
                    // 文件数
                    char sql[1024];
                    snprintf(sql, sizeof(sql), "SELECT file_count FROM user_file_count WHERE user_name = '%s'", user_name.c_str());
                    MYSQL_RES* res = db->query(sql);
                    if (res && mysql_num_rows(res) > 0) {
                        MYSQL_ROW row = mysql_fetch_row(res);
                        count = atoi(row[0]);
                    }
                    db->freeResult(res);

                    // 总下载量
                    snprintf(sql, sizeof(sql), "SELECT IFNULL(SUM(pv),0) FROM user_file_list WHERE user_name = '%s'", user_name.c_str());
                    res = db->query(sql);
                    if (res && mysql_num_rows(res) > 0) {
                        MYSQL_ROW row = mysql_fetch_row(res);
                        total_pv = atoi(row[0]);
                    }
                    db->freeResult(res);

                    // 存储空间
                    snprintf(sql, sizeof(sql),
                        "SELECT IFNULL(SUM(f.file_size),0) FROM user_file_list u "
                        "JOIN file_info f ON u.file_md5 = f.file_md5 "
                        "WHERE u.user_name = '%s'", user_name.c_str());
                    res = db->query(sql);
                    if (res && mysql_num_rows(res) > 0) {
                        MYSQL_ROW row = mysql_fetch_row(res);
                        total_storage = atoll(row[0]);
                    }
                    db->freeResult(res);

                    db->disconnect();
                }
                MySQLUtils::destroyInstance();
            }
            snprintf(resp, sizeof(resp), "{\"code\":0,\"count\":%d,\"pv\":%d,\"storage\":%lld}", count, total_pv, total_storage);
        }
        // 处理normal命令
        else if (strcmp(cmd, "normal") == 0) {
            cJSON* page_item = cJSON_GetObjectItem(root, "page");
            cJSON* size_item = cJSON_GetObjectItem(root, "size");

            if (!page_item || !size_item) {
                snprintf(resp, sizeof(resp), "{\"code\":1,\"msg\":\"缺少分页参数\"}");
                FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
                cJSON_Delete(root);
                delete[] req_body;
                continue;
            }

            int page = page_item->valueint;
            int size = size_item->valueint;
            int offset = (page - 1) * size;

            cJSON* data = cJSON_CreateArray();
            {
                MySQLUtils* db = MySQLUtils::getInstance();
                db->initInfo("127.0.0.1", "root", "CHANGE_ME", "ai_cloud_storage", 3306);
                if (db->connect()) {
                    char sql[1024];
                    snprintf(sql, sizeof(sql),
                        "SELECT u.file_name, f.file_size, f.file_url, u.pv, u.create_time, u.file_md5, u.share_status "
                        "FROM user_file_list u "
                        "JOIN file_info f ON u.file_md5 = f.file_md5 "
                        "WHERE u.user_name = '%s' "
                        "ORDER BY u.create_time DESC "
                        "LIMIT %d OFFSET %d",
                        user_name.c_str(), size, offset);
                    MYSQL_RES* res = db->query(sql);
                    if (res) {
                        MYSQL_ROW row;
                        while ((row = mysql_fetch_row(res)) != nullptr) {
                            cJSON* file = cJSON_CreateObject();
                            cJSON_AddStringToObject(file, "file_name", row[0]);
                            cJSON_AddNumberToObject(file, "file_size", atoll(row[1]));
                            cJSON_AddStringToObject(file, "file_url", row[2]);
                            cJSON_AddNumberToObject(file, "pv", atoi(row[3]));
                            cJSON_AddStringToObject(file, "create_time", row[4]);
                            cJSON_AddStringToObject(file, "file_md5", row[5] ? row[5] : "");
                            cJSON_AddNumberToObject(file, "share_status", row[6] ? atoi(row[6]) : 0);
                            cJSON_AddItemToArray(data, file);
                        }
                        db->freeResult(res);
                    }
                    db->disconnect();
                }
                MySQLUtils::destroyInstance();
            }

            char* data_str = cJSON_PrintUnformatted(data);
            snprintf(resp, sizeof(resp), "{\"code\":0,\"data\":%s}", data_str);
            free(data_str);
            cJSON_Delete(data);
        }
        else {
            snprintf(resp, sizeof(resp), "{\"code\":1,\"msg\":\"无效的命令\"}");
        }

        FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
        cJSON_Delete(root);
        delete[] req_body;
    }

    RedisUtils::destroyInstance();
    return 0;
}