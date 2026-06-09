#include <fcgiapp.h>
#include "mysql_utils.h"
#include "redis_utils.h"
#include "cjson_utils.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <regex>
#include <atomic>

using namespace std;

// ========== 配置常量 ==========
static constexpr int NUM_WORKERS = 8;         // 工作线程数
static constexpr int MD5_CACHE_TTL = 3600;     // MD5缓存有效期（秒）
static constexpr int LOCK_TTL = 5;             // 分布式锁过期时间（秒）
static constexpr int MAX_LOCK_RETRIES = 30;    // 等待锁最大重试次数
static constexpr int RETRY_INTERVAL_MS = 50;   // 等待锁重试间隔（毫秒）

// ========== 工具函数 ==========

// 校验MD5是否为32位十六进制
static bool is_valid_md5(const char* md5) {
    if (!md5 || strlen(md5) != 32) return false;
    for (int i = 0; i < 32; ++i) {
        if (!isxdigit(static_cast<unsigned char>(md5[i]))) return false;
    }
    return true;
}

// 简易URL解码
static string url_decode(const string& src) {
    string res;
    res.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '%' && i + 2 < src.size()) {
            char hex[3] = {src[i+1], src[i+2], 0};
            res += static_cast<char>(strtol(hex, nullptr, 16));
            i += 2;
        } else if (src[i] == '+') {
            res += ' ';
        } else {
            res += src[i];
        }
    }
    return res;
}

// 检查文件名是否合法（不含路径穿越等危险字符）
static bool is_valid_filename(const char* name) {
    if (!name || strlen(name) == 0 || strlen(name) > 255) return false;
    const char* forbidden = "/\\:*?\"<>|";
    for (int i = 0; forbidden[i]; ++i) {
        if (strchr(name, forbidden[i])) return false;
    }
    return true;
}

// ========== 单请求处理 ==========
static void process_request(FCGX_Request& req, RedisUtils& redis) {
    char resp[4096] = {0};
    char token[128] = {0};
    char file_md5[128] = {0};
    char file_name[256] = {0};
    string body;

    // ---------- 1. 解析参数（支持GET和POST）----------
    const char* query_str = FCGX_GetParam("QUERY_STRING", req.envp);
    const char* content_len_str = FCGX_GetParam("CONTENT_LENGTH", req.envp);
    int content_len = content_len_str ? atoi(content_len_str) : 0;

    if (query_str && strlen(query_str) > 0) {
        // GET: token=xxx&file_md5=xxx&filename=xxx
        char raw_token[128] = {0}, raw_md5[128] = {0}, raw_name[256] = {0};
        sscanf(query_str, "token=%127[^&]&file_md5=%127[^&]&filename=%255[^&]",
               raw_token, raw_md5, raw_name);
        strncpy(token, url_decode(raw_token).c_str(), sizeof(token) - 1);
        // file_md5 不含特殊字符，无需解码
        strncpy(file_md5, raw_md5, sizeof(file_md5) - 1);
        strncpy(file_name, url_decode(raw_name).c_str(), sizeof(file_name) - 1);
    } else if (content_len > 0) {
        // POST: JSON body
        body.resize(content_len + 1, 0);
        FCGX_GetStr(&body[0], content_len, req.in);

        cJSON* root = cJSON_Parse(body.c_str());
        if (root) {
            cJSON* t = cJSON_GetObjectItem(root, "token");
            cJSON* m = cJSON_GetObjectItem(root, "file_md5");
            cJSON* f = cJSON_GetObjectItem(root, "filename");
            if (t && t->valuestring) strncpy(token, t->valuestring, sizeof(token) - 1);
            if (m && m->valuestring) strncpy(file_md5, m->valuestring, sizeof(file_md5) - 1);
            if (f && f->valuestring) strncpy(file_name, f->valuestring, sizeof(file_name) - 1);
        }
        cJSON_Delete(root);
    }

    // ---------- 2. 参数校验 ----------
    if (strlen(token) == 0 || strlen(file_md5) == 0 || strlen(file_name) == 0) {
        FCGX_FPrintF(req.out,
            "Content-Type: application/json\r\n\r\n"
            "{\"code\":1,\"msg\":\"缺少必要参数\"}");
        return;
    }
    if (!is_valid_md5(file_md5)) {
        FCGX_FPrintF(req.out,
            "Content-Type: application/json\r\n\r\n"
            "{\"code\":2,\"msg\":\"MD5格式错误\"}");
        return;
    }
    if (!is_valid_filename(file_name)) {
        FCGX_FPrintF(req.out,
            "Content-Type: application/json\r\n\r\n"
            "{\"code\":3,\"msg\":\"文件名不合法\"}");
        return;
    }

    // ---------- 3. Token认证 ----------
    string redis_cmd = "GET ";
    redis_cmd += token;
    redisReply* reply = redis.command(redis_cmd);

    if (!reply || reply->type != REDIS_REPLY_STRING) {
        FCGX_FPrintF(req.out,
            "Content-Type: application/json\r\n\r\n"
            "{\"code\":4,\"msg\":\"Token无效或已过期\"}");
        redis.freeReply(reply);
        return;
    }

    string user_id_str(reply->str);
    redis.freeReply(reply);

    // ---------- 4. 连接MySQL，查询用户名 ----------
    MySQLUtils db;
    db.initInfo("127.0.0.1", "root", "CHANGE_ME", "ai_cloud_storage", 3306);
    if (!db.connect()) {
        FCGX_FPrintF(req.out,
            "Content-Type: application/json\r\n\r\n"
            "{\"code\":50,\"msg\":\"数据库连接失败\"}");
        return;
    }

    char sql[1536];
    snprintf(sql, sizeof(sql),
        "SELECT user_name FROM user_info WHERE id=%s LIMIT 1",
        user_id_str.c_str());
    MYSQL_RES* res = db.query(sql);
    string user_name;
    if (res && mysql_num_rows(res) > 0) {
        MYSQL_ROW row = mysql_fetch_row(res);
        user_name = row[0] ? row[0] : "";
    }
    db.freeResult(res);

    if (user_name.empty()) {
        db.disconnect();
        FCGX_FPrintF(req.out,
            "Content-Type: application/json\r\n\r\n"
            "{\"code\":5,\"msg\":\"用户不存在\"}");
        return;
    }

    // ---------- 5. 查Redis缓存 ----------
    string md5_cache_key = "md5:";
    md5_cache_key += file_md5;

    string file_url;
    redis_cmd = "GET " + md5_cache_key;
    reply = redis.command(redis_cmd);

    if (reply && reply->type == REDIS_REPLY_STRING) {
        // Redis缓存命中 → 直接秒传成功
        file_url = reply->str;
        redis.freeReply(reply);
    } else {
        redis.freeReply(reply);

        // ---------- 6. 分布式锁（SETNX）防并发重复查询MySQL ----------
        string lock_key = "lock:md5:";
        lock_key += file_md5;

        redis_cmd = "SETNX " + lock_key + " 1";
        reply = redis.command(redis_cmd);
        bool got_lock = (reply && reply->type == REDIS_REPLY_INTEGER && reply->integer == 1);
        redis.freeReply(reply);

        if (got_lock) {
            // 设置锁过期（防止死锁）
            redis_cmd = "EXPIRE " + lock_key + " " + to_string(LOCK_TTL);
            reply = redis.command(redis_cmd);
            redis.freeReply(reply);

            // 双重检查：持有锁期间可能已有其他线程更新了缓存
            redis_cmd = "GET " + md5_cache_key;
            reply = redis.command(redis_cmd);
            if (reply && reply->type == REDIS_REPLY_STRING) {
                file_url = reply->str;
                redis.freeReply(reply);
            } else {
                redis.freeReply(reply);

                // 查MySQL
                snprintf(sql, sizeof(sql),
                    "SELECT file_url FROM file_info WHERE file_md5='%s' LIMIT 1",
                    file_md5);
                res = db.query(sql);
                if (res && mysql_num_rows(res) > 0) {
                    MYSQL_ROW row = mysql_fetch_row(res);
                    file_url = row[0] ? row[0] : "";
                }
                db.freeResult(res);

                // 写入缓存
                if (!file_url.empty()) {
                    redis_cmd = "SET " + md5_cache_key + " " + file_url
                              + " EX " + to_string(MD5_CACHE_TTL);
                    reply = redis.command(redis_cmd);
                    redis.freeReply(reply);
                }
            }

            // 释放锁
            redis_cmd = "DEL " + lock_key;
            reply = redis.command(redis_cmd);
            redis.freeReply(reply);
        } else {
            // 没拿到锁 → 等锁持有者填充缓存
            for (int i = 0; i < MAX_LOCK_RETRIES; ++i) {
                this_thread::sleep_for(chrono::milliseconds(RETRY_INTERVAL_MS));
                redis_cmd = "GET " + md5_cache_key;
                reply = redis.command(redis_cmd);
                if (reply && reply->type == REDIS_REPLY_STRING) {
                    file_url = reply->str;
                    redis.freeReply(reply);
                    break;
                }
                redis.freeReply(reply);
            }
        }
    }

    // ---------- 7. 结果处理 ----------
    if (file_url.empty()) {
        db.disconnect();
        FCGX_FPrintF(req.out,
            "Content-Type: application/json\r\n\r\n"
            "{\"code\":6,\"msg\":\"需要上传\"}");
        return;
    }

    // 8. 增加file_info引用计数
    snprintf(sql, sizeof(sql),
        "UPDATE file_info SET ref_count=ref_count+1 WHERE file_md5='%s'",
        file_md5);
    db.update(sql);

    // 9. 插入个人文件列表
    snprintf(sql, sizeof(sql),
        "INSERT IGNORE INTO user_file_list(user_name, file_md5, file_name, share_status, pv, create_time) "
        "VALUES('%s', '%s', '%s', 0, 0, NOW())",
        user_name.c_str(), file_md5, file_name);
    db.update(sql);

    // 10. 更新个人文件计数
    snprintf(sql, sizeof(sql),
        "INSERT INTO user_file_count(user_name, file_count) VALUES('%s', 1) "
        "ON DUPLICATE KEY UPDATE file_count=file_count+1",
        user_name.c_str());
    db.update(sql);

    db.disconnect();

    // 11. 返回成功
    FCGX_FPrintF(req.out,
        "Content-Type: application/json\r\n\r\n"
        "{\"code\":0,\"msg\":\"秒传成功\",\"url\":\"%s\"}",
        file_url.c_str());
}

// ========== 入口：多线程FCGI ==========
int main() {
    FCGX_Init();

    vector<thread> workers;
    for (int i = 0; i < NUM_WORKERS; ++i) {
        workers.emplace_back([]() {
            // 每个线程独立持有Redis连接
            RedisUtils redis;
            redis.initInfo("127.0.0.1", 6379);
            redis.connect();

            FCGX_Request request;
            FCGX_InitRequest(&request, 0, 0);

            while (FCGX_Accept_r(&request) >= 0) {
                process_request(request, redis);
                FCGX_Finish_r(&request);
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    return 0;
}
