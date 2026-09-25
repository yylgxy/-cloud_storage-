#include "fcgi_stdio.h"
#include "mysql_utils.h"
#include "redis_utils.h"
#include "cjson_utils.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>

using namespace std;

// 共享文件热度排行榜的 Redis ZSET 键名。
// member = share_id，score = 该分享的下载量（pv）。
// 选 ZSET 而不是 List 的原因：ZSET 天然按 score 有序，ZREVRANGE 取 TopN 的复杂度
// 是 O(log N + M)，不需要把整张表捞出来再排序。
static const char* RANK_KEY = "rank:share:hot";

int main() {
    MySQLUtils* db = MySQLUtils::getInstance();
    RedisUtils* redis = RedisUtils::getInstance();
    CJsonUtils* json = CJsonUtils::getInstance();

    db->initInfo("127.0.0.1", "root", MySQLUtils::getDbPassword(), "ai_cloud_storage", 3306);
    redis->initInfo("127.0.0.1", 6379);
    redis->connect();

    while (FCGI_Accept() >= 0) {
        string resp;
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
                resp = json->toString(err, false);
                json->free(err);
                FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp.c_str());
                continue;
            }

            int len = atoi(content_len);
            char* req_body = new char[len + 1];
            FCGI_fread(req_body, 1, len, FCGI_stdin);
            req_body[len] = '\0';

            root = cJSON_Parse(req_body);
            delete[] req_body;

            if (!root) {
                resp = "{\"code\":1,\"msg\":\"JSON格式错误\"}";
                FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp.c_str());
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
            resp = json->toString(err, false);
            json->free(err);
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp.c_str());
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
            resp = json->toString(err, false);
            json->free(err);
            redis->freeReply(reply);
            if (root) cJSON_Delete(root);
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp.c_str());
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
            resp = json->toString(err, false);
            json->free(err);
            if (root) cJSON_Delete(root);
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp.c_str());
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
        // ====================== rank：热度排行榜 ======================
        // 排序工作在 Redis ZSET 里完成，MySQL 只负责按 share_id 补齐元数据。
        else if (strcmp(cmd, "rank") == 0) {
            if (top <= 0)   top = 10;
            if (top > 100)  top = 100;   // 防止 top 参数被用来拖库

            cJSON* data = cJSON_CreateArray();

            // ---------- 1. ZSET 为空时从 MySQL 一次性回填 ----------
            // Redis 重启、或首次部署该功能时 ZSET 是空的。这里做自愈回填，
            // 否则排行榜会凭空变成空列表，看起来像是功能坏了。
            redisReply* zcard = redis->command(string("ZCARD ") + RANK_KEY);
            long long card = (zcard && zcard->type == REDIS_REPLY_INTEGER) ? zcard->integer : 0;
            redis->freeReply(zcard);

            if (card == 0 && db->connect()) {
                MYSQL_RES* backfill = db->query("SELECT share_id, pv FROM share_file_list");
                if (backfill) {
                    MYSQL_ROW r;
                    while ((r = mysql_fetch_row(backfill)) != nullptr) {
                        if (r[0] == nullptr) continue;
                        string zcmd = "ZADD ";
                        zcmd += RANK_KEY;
                        zcmd += " ";
                        zcmd += (r[1] != nullptr ? r[1] : "0");
                        zcmd += " ";
                        zcmd += r[0];
                        redisReply* zr = redis->command(zcmd);
                        redis->freeReply(zr);
                    }
                    db->freeResult(backfill);
                }
                db->disconnect();
            }

            // ---------- 2. 从 ZSET 取 TopN（O(log N + M)）----------
            char zcmd[256];
            snprintf(zcmd, sizeof(zcmd), "ZREVRANGE %s 0 %d WITHSCORES", RANK_KEY, top - 1);
            redisReply* zres = redis->command(zcmd);

            vector<string>    ids;
            vector<long long> scores;
            if (zres && zres->type == REDIS_REPLY_ARRAY) {
                for (size_t i = 0; i + 1 < zres->elements; i += 2) {
                    ids.push_back(zres->element[i]->str);
                    scores.push_back(atoll(zres->element[i + 1]->str));
                }
            }
            redis->freeReply(zres);

            // ---------- 3. 一次 IN 查询补齐详情 ----------
            // 不用"循环里逐个查"（N+1），而是一次把所有 share_id 捞回来，
            // 再按 ZSET 给出的名次在内存里重排。
            if (!ids.empty() && db->connect()) {
                string in_list;
                for (size_t i = 0; i < ids.size(); ++i) {
                    if (i > 0) in_list += ",";
                    in_list += "'" + ids[i] + "'";     // share_id 由服务端生成，形如 share_1717_0042
                }

                map<string, size_t> rank_order;
                for (size_t i = 0; i < ids.size(); ++i) rank_order[ids[i]] = i;

                char rank_sql[8192];
                snprintf(rank_sql, sizeof(rank_sql),
                    "SELECT s.share_id, s.user_name, s.file_md5, s.file_name, "
                    "IFNULL(s.extract_code, ''), IFNULL(s.expire_time, ''), "
                    "s.create_time, IFNULL(f.file_size, 0), IFNULL(f.file_url, '') "
                    "FROM share_file_list s "
                    "LEFT JOIN file_info f ON s.file_md5 = f.file_md5 "
                    "WHERE s.share_id IN (%s)",
                    in_list.c_str());

                vector<cJSON*> slots(ids.size(), nullptr);
                MYSQL_RES* rres = db->query(rank_sql);
                if (rres) {
                    MYSQL_ROW row;
                    while ((row = mysql_fetch_row(rres)) != nullptr) {
                        map<string, size_t>::iterator it = rank_order.find(row[0]);
                        if (it == rank_order.end()) continue;
                        size_t idx = it->second;

                        cJSON* file = cJSON_CreateObject();
                        cJSON_AddStringToObject(file, "share_id", row[0]);
                        cJSON_AddStringToObject(file, "user_name", row[1]);
                        cJSON_AddStringToObject(file, "file_md5", row[2]);
                        cJSON_AddStringToObject(file, "file_name", row[3]);
                        cJSON_AddStringToObject(file, "extract_code", row[4]);
                        cJSON_AddStringToObject(file, "expire_time", row[5]);
                        cJSON_AddStringToObject(file, "create_time", row[6]);
                        cJSON_AddNumberToObject(file, "file_size", atoll(row[7]));
                        cJSON_AddStringToObject(file, "file_url", row[8]);
                        // 下载量直接取 ZSET 的 score，比 MySQL 里的 pv 更新更及时
                        cJSON_AddNumberToObject(file, "pv", (int)scores[idx]);
                        slots[idx] = file;
                    }
                    db->freeResult(rres);
                }
                db->disconnect();

                for (size_t i = 0; i < slots.size(); ++i) {
                    if (slots[i] != nullptr) {
                        cJSON_AddItemToArray(data, slots[i]);
                    }
                }
            }

            cJSON_AddNumberToObject(result, "code", 0);
            cJSON_AddItemToObject(result, "data", data);
            cJSON_AddStringToObject(result, "order_by", "redis-zset:rank:share:hot");
        }
        else {
            cJSON_AddNumberToObject(result, "code", 1);
            cJSON_AddStringToObject(result, "msg", "无效的命令");
        }

        resp = json->toString(result, false);
        json->free(result);
        if (root) cJSON_Delete(root);

        FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp.c_str());
    }

    RedisUtils::destroyInstance();
    MySQLUtils::destroyInstance();
    CJsonUtils::destroyInstance();
    return 0;
}
