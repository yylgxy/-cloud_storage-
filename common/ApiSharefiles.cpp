#include "fcgi_stdio.h"
#include "mysql_utils.h"
#include "redis_utils.h"
#include "cjson_utils.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace std;

int main() {
    MySQLUtils* db = MySQLUtils::getInstance();
    RedisUtils* redis = RedisUtils::getInstance();
    CJsonUtils* json = CJsonUtils::getInstance();

    db->initInfo("127.0.0.1", "root", "CHANGE_ME", "ai_cloud_storage", 3306);
    redis->initInfo("127.0.0.1", 6379);
    redis->connect();

    while (FCGI_Accept() >= 0) {
        char resp[8192] = {0};
        char token[128] = {0};
        char cmd[32] = {0};
        int page = 1, size = 10, top = 10;

        // 优先处理 GET 请求（QUERY_STRING），否则处理 POST（JSON body）
        char* query_str = getenv("QUERY_STRING");
        cJSON* root = nullptr;
        bool from_get = false;

        if (query_str != nullptr && strlen(query_str) > 0) {
            from_get = true;
            char page_str[16] = "1", size_str[16] = "10", top_str[16] = "10";
            sscanf(query_str, "token=%127[^&]&cmd=%31[^&]&page=%15[^&]&size=%15[^&]&top=%15[^&]",
                   token, cmd, page_str, size_str, top_str);
            page = atoi(page_str);
            size = atoi(size_str);
            top = atoi(top_str);
        }
        else {
            char* content_len = getenv("CONTENT_LENGTH");
            if (!content_len) {
                cJSON* err = cJSON_CreateObject();
                cJSON_AddNumberToObject(err, "code", 1);
                cJSON_AddStringToObject(err, "msg", "缺少请求体");
                snprintf(resp, sizeof(resp), "%s", json->toString(err, false).c_str());
                json->free(err);
                FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
                continue;
            }

            int len = atoi(content_len);
            char* req_body = new char[len + 1];
            FCGI_fread(req_body, 1, len, FCGI_stdin);
            req_body[len] = '\0';

            root = cJSON_Parse(req_body);
            delete[] req_body;

            if (!root) {
                snprintf(resp, sizeof(resp), "{\"code\":1,\"msg\":\"JSON格式错误\"}");
                FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
                continue;
            }

            cJSON* token_item = cJSON_GetObjectItem(root, "token");
            cJSON* cmd_item = cJSON_GetObjectItem(root, "cmd");
            if (token_item) strncpy(token, token_item->valuestring, sizeof(token) - 1);
            if (cmd_item) strncpy(cmd, cmd_item->valuestring, sizeof(cmd) - 1);

            cJSON* page_item = cJSON_GetObjectItem(root, "page");
            cJSON* size_item = cJSON_GetObjectItem(root, "size");
            cJSON* top_item = cJSON_GetObjectItem(root, "top");
            if (page_item) page = page_item->valueint;
            if (size_item) size = size_item->valueint;
            if (top_item) top = top_item->valueint;
        }

        if (strlen(token) == 0 || strlen(cmd) == 0) {
            cJSON* err = cJSON_CreateObject();
            cJSON_AddNumberToObject(err, "code", 1);
            cJSON_AddStringToObject(err, "msg", "缺少必要参数");
            snprintf(resp, sizeof(resp), "%s", json->toString(err, false).c_str());
            json->free(err);
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            if (root) cJSON_Delete(root);
            continue;
        }

        // 验证Token
        char redis_key[128];
        snprintf(redis_key, sizeof(redis_key), "%s", token);
        string redis_cmd_str = "GET ";
        redis_cmd_str += redis_key;
        redisReply* reply = redis->command(redis_cmd_str);

        if (reply == nullptr || reply->type != REDIS_REPLY_STRING) {
            cJSON* err = cJSON_CreateObject();
            cJSON_AddNumberToObject(err, "code", 4);
            cJSON_AddStringToObject(err, "msg", "Token无效或已过期");
            snprintf(resp, sizeof(resp), "%s", json->toString(err, false).c_str());
            json->free(err);
            redis->freeReply(reply);
            if (root) cJSON_Delete(root);
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            continue;
        }

        int user_id = atoi(reply->str);
        redis->freeReply(reply);

        // 查询用户名
        char user_name[32] = {0};
        if (db->connect()) {
            char sql[1024];
            snprintf(sql, sizeof(sql), "SELECT user_name FROM user_info WHERE id = %d LIMIT 1", user_id);
            MYSQL_RES* res = db->query(sql);
            if (res && mysql_num_rows(res) > 0) {
                MYSQL_ROW row = mysql_fetch_row(res);
                strncpy(user_name, row[0], sizeof(user_name) - 1);
            }
            db->freeResult(res);
            db->disconnect();
        }

        if (strlen(user_name) == 0) {
            cJSON* err = cJSON_CreateObject();
            cJSON_AddNumberToObject(err, "code", 1);
            cJSON_AddStringToObject(err, "msg", "用户不存在");
            snprintf(resp, sizeof(resp), "%s", json->toString(err, false).c_str());
            json->free(err);
            if (root) cJSON_Delete(root);
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            continue;
        }

        cJSON* result = cJSON_CreateObject();

        // 处理count命令 - 获取分享文件总数
        if (strcmp(cmd, "count") == 0) {
            int count = 0;
            if (db->connect()) {
                char sql[1024];
                snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM share_file_list WHERE user_name = '%s'", user_name);
                MYSQL_RES* res = db->query(sql);
                if (res && mysql_num_rows(res) > 0) {
                    MYSQL_ROW row = mysql_fetch_row(res);
                    count = atoi(row[0]);
                }
                db->freeResult(res);
                db->disconnect();
            }
            cJSON_AddNumberToObject(result, "code", 0);
            cJSON_AddNumberToObject(result, "count", count);
        }
        // 处理list命令 - 获取分享文件列表
        else if (strcmp(cmd, "list") == 0) {
            int offset = (page - 1) * size;
            cJSON* data = cJSON_CreateArray();

            if (db->connect()) {
                char sql[2048];
                snprintf(sql, sizeof(sql),
                    "SELECT s.share_id, s.user_name, s.file_md5, s.file_name, "
                    "IFNULL(s.extract_code, ''), "
                    "IFNULL(s.expire_time, ''), "
                    "s.pv, s.create_time, "
                    "IFNULL(f.file_size, 0), IFNULL(f.file_url, '') "
                    "FROM share_file_list s "
                    "LEFT JOIN file_info f ON s.file_md5 = f.file_md5 "
                    "ORDER BY s.create_time DESC "
                    "LIMIT %d OFFSET %d",
                    size, offset);
                MYSQL_RES* res = db->query(sql);
                if (res) {
                    MYSQL_ROW row;
                    while ((row = mysql_fetch_row(res)) != nullptr) {
                        cJSON* file = cJSON_CreateObject();
                        cJSON_AddStringToObject(file, "share_id", row[0]);
                        cJSON_AddStringToObject(file, "user_name", row[1]);
                        cJSON_AddStringToObject(file, "file_md5", row[2]);
                        cJSON_AddStringToObject(file, "file_name", row[3]);
                        cJSON_AddStringToObject(file, "extract_code", row[4]);
                        cJSON_AddStringToObject(file, "expire_time", row[5]);
                        cJSON_AddNumberToObject(file, "pv", atoi(row[6]));
                        cJSON_AddStringToObject(file, "create_time", row[7]);
                        cJSON_AddNumberToObject(file, "file_size", atoll(row[8]));
                        cJSON_AddStringToObject(file, "file_url", row[9]);
                        cJSON_AddItemToArray(data, file);
                    }
                    db->freeResult(res);
                }
                db->disconnect();
            }

            cJSON_AddNumberToObject(result, "code", 0);
            cJSON_AddItemToObject(result, "data", data);
        }
        // 处理rank命令 - 获取下载排行榜（按下载量倒序）
        else if (strcmp(cmd, "rank") == 0) {
            cJSON* data = cJSON_CreateArray();

            if (db->connect()) {
                char sql[2048];
                snprintf(sql, sizeof(sql),
                    "SELECT s.share_id, s.user_name, s.file_md5, s.file_name, "
                    "IFNULL(s.extract_code, ''), "
                    "s.pv, s.create_time, "
                    "IFNULL(f.file_size, 0), IFNULL(f.file_url, '') "
                    "FROM share_file_list s "
                    "LEFT JOIN file_info f ON s.file_md5 = f.file_md5 "
                    "ORDER BY s.pv DESC "
                    "LIMIT %d",
                    top);
                MYSQL_RES* res = db->query(sql);
                if (res) {
                    MYSQL_ROW row;
                    while ((row = mysql_fetch_row(res)) != nullptr) {
                        cJSON* file = cJSON_CreateObject();
                        cJSON_AddStringToObject(file, "share_id", row[0]);
                        cJSON_AddStringToObject(file, "user_name", row[1]);
                        cJSON_AddStringToObject(file, "file_md5", row[2]);
                        cJSON_AddStringToObject(file, "file_name", row[3]);
                        cJSON_AddStringToObject(file, "extract_code", row[4]);
                        cJSON_AddNumberToObject(file, "pv", atoi(row[5]));
                        cJSON_AddStringToObject(file, "create_time", row[6]);
                        cJSON_AddNumberToObject(file, "file_size", atoll(row[7]));
                        cJSON_AddStringToObject(file, "file_url", row[8]);
                        cJSON_AddItemToArray(data, file);
                    }
                    db->freeResult(res);
                }
                db->disconnect();
            }

            cJSON_AddNumberToObject(result, "code", 0);
            cJSON_AddItemToObject(result, "data", data);
        }
        else {
            cJSON_AddNumberToObject(result, "code", 1);
            cJSON_AddStringToObject(result, "msg", "无效的命令");
        }

        snprintf(resp, sizeof(resp), "%s", json->toString(result, false).c_str());
        json->free(result);
        if (root) cJSON_Delete(root);

        FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
    }

    RedisUtils::destroyInstance();
    MySQLUtils::destroyInstance();
    CJsonUtils::destroyInstance();
    return 0;
}
