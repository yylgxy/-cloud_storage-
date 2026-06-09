#include "fcgi_stdio.h"
#include "fastdfs_utils.h"
#include "mysql_utils.h"
#include "redis_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>

#define MYSQL_HOST "127.0.0.1"
#define MYSQL_USER "root"
#define MYSQL_PASS "CHANGE_ME"
#define MYSQL_DB "ai_cloud_storage"
#define REDIS_HOST "127.0.0.1"
#define REDIS_PORT 6379
#define REDIS_PASS ""
#define CENTOS_IP "192.168.150.101"

// ---------- 解析 multipart/form-data ----------
std::map<std::string, std::string> parse_multipart() {
    std::map<std::string, std::string> fields;

    char* content_type = getenv("CONTENT_TYPE");
    if (!content_type) return fields;

    // 提取 boundary
    const char* b = strstr(content_type, "boundary=");
    if (!b) return fields;
    std::string boundary = "--" + std::string(b + 9);

    // 读取整个请求体（FastCGI stdin）
    std::string body;
    char buf[4096];
    int n;
    while ((n = FCGI_fread(buf, 1, sizeof(buf), stdin)) > 0) {
        body.append(buf, n);
    }

    size_t pos = 0;
    while ((pos = body.find(boundary, pos)) != std::string::npos) {
        pos += boundary.length();

        // 跳过换行
        if (body.compare(pos, 2, "\r\n") == 0) pos += 2;
        else if (body[pos] == '\n') pos += 1;
        else break;

        // 查找头部结束
        size_t header_end = body.find("\r\n\r\n", pos);
        if (header_end == std::string::npos) break;
        std::string headers = body.substr(pos, header_end - pos);
        pos = header_end + 4;

        // 提取 name
        std::string name;
        size_t np = headers.find("name=\"");
        if (np != std::string::npos) {
            np += 6;
            size_t ne = headers.find("\"", np);
            if (ne != std::string::npos)
                name = headers.substr(np, ne - np);
        }

        // 提取 filename（如果是文件字段）
        std::string filename;
        size_t fp = headers.find("filename=\"");
        if (fp != std::string::npos) {
            fp += 10;
            size_t fe = headers.find("\"", fp);
            if (fe != std::string::npos)
                filename = headers.substr(fp, fe - fp);
        }

        // 数据结束位置（下一个 boundary）
        size_t data_end = body.find(boundary, pos);
        if (data_end == std::string::npos) break;

        // 去掉尾部 \r\n
        size_t data_len = data_end - pos;
        if (data_len >= 2 && body[data_end - 2] == '\r' && body[data_end - 1] == '\n')
            data_len -= 2;
        else if (data_len >= 1 && body[data_end - 1] == '\n')
            data_len -= 1;

        std::string data = body.substr(pos, data_len);

        if (!filename.empty()) {
            // 文件字段：写入临时文件
            char tmpname[] = "/tmp/upload_XXXXXX";
            int fd = mkstemp(tmpname);
            if (fd != -1) {
                write(fd, data.c_str(), data.size());
                close(fd);
                fields[name] = tmpname;                    // 临时文件路径
                fields[name + "_name"] = filename;        // 原始文件名
                fields[name + "_size"] = std::to_string(data.size()); // 文件大小
            }
        } else {
            // 普通表单字段
            fields[name] = data;
        }

        pos = data_end; // 继续下一个 boundary
    }

    return fields;
}

// ---------- 主程序 ----------
int main() {
    FastdfsUtils* fdfs = FastdfsUtils::getInstance();
    MySQLUtils* mysql = MySQLUtils::getInstance();
    RedisUtils* redis = RedisUtils::getInstance();

    if (!fdfs->init("/etc/fdfs/client.conf")) return 1;
    mysql->initInfo(MYSQL_HOST, MYSQL_USER, MYSQL_PASS, MYSQL_DB);
    if (!mysql->connect()) return 1;
    redis->initInfo(REDIS_HOST, REDIS_PORT, REDIS_PASS, 0);
    if (!redis->connect()) return 1;

    while (FCGI_Accept() >= 0) {
        FCGI_printf("Content-Type: application/json;charset=UTF-8\r\n\r\n");

        // ---------- 参数获取 ----------
        // 1. 先解析 multipart（如果是 POST 表单）
        auto fields = parse_multipart();

        // 从 multipart 中取值
        std::string token_str, md5_str, file_name_str, file_path_str, file_size_str;
        if (fields.count("token"))      token_str     = fields["token"];
        if (fields.count("file_md5"))   md5_str       = fields["file_md5"];
        if (fields.count("file")) {
            file_path_str = fields["file"];                 // 临时文件路径
            file_name_str = fields["file_name"];            // 原始文件名（上传字段的 _name）
            file_size_str = fields["file_size"];            // 文件大小（上传字段的 _size）
        }

        // 2. 如果 multipart 里没有 token / md5，再从 URL 参数中获取（兼容旧接口）
        char* query = getenv("QUERY_STRING");
        if (query) {
            if (token_str.empty()) {
                char* p = strstr(query, "token=");
                if (p) token_str = p + 6;
            }
            if (md5_str.empty()) {
                char* p = strstr(query, "file_md5=");
                if (p) md5_str = p + 9;
            }
        }

        // 3. 如果 multipart 中无 file 字段，可能是旧 nginx upload 模块传过来的环境变量（极少情况）
        if (file_path_str.empty()) {
            char* env_file_path = getenv("file_path");
            if (env_file_path) file_path_str = env_file_path;
        }
        if (file_name_str.empty()) {
            char* env_file_name = getenv("file_name");
            if (env_file_name) file_name_str = env_file_name;
        }
        if (file_size_str.empty()) {
            char* env_file_size = getenv("file_size");
            if (env_file_size) file_size_str = env_file_size;
        }

        // 转换为 C 风格指针，方便后续使用
        const char* token     = token_str.empty()     ? nullptr : token_str.c_str();
        const char* file_md5  = md5_str.empty()       ? nullptr : md5_str.c_str();
        const char* file_name = file_name_str.empty() ? nullptr : file_name_str.c_str();
        const char* file_path = file_path_str.empty() ? nullptr : file_path_str.c_str();
        const char* file_size = file_size_str.empty() ? "0"     : file_size_str.c_str();

        // ---------- 参数校验 ----------
        if (!token || !file_md5 || !file_name || !file_path) {
            FCGI_printf("{\"debug\":\"token=%s,md5=%s,name=%s,path=%s\",\"code\":1,\"msg\":\"缺少必要参数\"}",
                token?token:"null", file_md5?file_md5:"null",
                file_name?file_name:"null", file_path?file_path:"null");
            FCGI_Finish(); continue;
        }

        // ---------- Redis 验证 token ----------
        char redis_cmd[256];
        snprintf(redis_cmd, sizeof(redis_cmd), "GET %s", token);
        redisReply* reply = redis->command(redis_cmd);
        if (!reply || reply->type != REDIS_REPLY_STRING) {
            FCGI_printf("{\"code\":1,\"msg\":\"token无效或已过期\"}");
            if(reply) redis->freeReply(reply);
            unlink(file_path); FCGI_Finish(); continue;
        }
        std::string user_id_str = reply->str;
        redis->freeReply(reply);

        // 查询实际用户名
        char sql[2048];
        std::string username;
        snprintf(sql, sizeof(sql),
            "SELECT user_name FROM user_info WHERE id=%s LIMIT 1",
            user_id_str.c_str());
        MYSQL_RES* user_res = mysql->query(sql);
        if (user_res && mysql_num_rows(user_res) > 0) {
            MYSQL_ROW row = mysql_fetch_row(user_res);
            username = row[0] ? row[0] : "";
        }
        mysql->freeResult(user_res);

        if (username.empty()) {
            FCGI_printf("{\"code\":1,\"msg\":\"用户不存在\"}");
            unlink(file_path); FCGI_Finish(); continue;
        }

        // ---------- 上传到 FastDFS ----------
        std::string file_id = fdfs->uploadFile(file_path);
        if (file_id.empty()) {
            FCGI_printf("{\"code\":1,\"msg\":\"文件上传失败\"}");
            unlink(file_path); FCGI_Finish(); continue;
        }

        // ---------- 构造 URL 并写数据库 ----------
        // 使用相对路径，通过 Host 请求头自动适配域名
        std::string file_url = "/" + file_id;

        // 检查MySQL连接，断开则重连
        if (!mysql->isConnected()) {
            mysql->connect();
        }

        snprintf(sql, sizeof(sql),
            "INSERT INTO file_info (file_md5, file_size, file_id, file_url, ref_count, create_time) "
            "VALUES ('%s', %s, '%s', '%s', 1, NOW()) "
            "ON DUPLICATE KEY UPDATE ref_count = ref_count + 1",
            file_md5, file_size, file_id.c_str(), file_url.c_str());
        if (mysql->update(sql) < 0) {
            FCGI_printf("{\"code\":1,\"msg\":\"数据库写入失败(file_info): %s\"}", mysql->getError().c_str());
            unlink(file_path); FCGI_Finish(); continue;
        }

        snprintf(sql, sizeof(sql),
            "INSERT INTO user_file_list (user_name, file_md5, file_name, share_status, pv, create_time) "
            "VALUES ('%s', '%s', '%s', 0, 0, NOW())",
            username.c_str(), file_md5, file_name);
        if (mysql->update(sql) < 0) {
            FCGI_printf("{\"code\":1,\"msg\":\"数据库写入失败(user_file_list): %s\"}", mysql->getError().c_str());
            unlink(file_path); FCGI_Finish(); continue;
        }

        snprintf(sql, sizeof(sql),
            "INSERT INTO user_file_count (user_name, file_count) VALUES ('%s', 1) "
            "ON DUPLICATE KEY UPDATE file_count = file_count + 1",
            username.c_str());
        if (mysql->update(sql) < 0) {
            FCGI_printf("{\"code\":1,\"msg\":\"数据库写入失败(user_file_count): %s\"}", mysql->getError().c_str());
            unlink(file_path); FCGI_Finish(); continue;
        }

        // 删除临时文件
        unlink(file_path);

        FCGI_printf("{\"code\":0,\"msg\":\"上传成功\",\"url\":\"%s\",\"file_name\":\"%s\"}",
            file_url.c_str(), file_name);
        FCGI_Finish();
    }

    FastdfsUtils::destroyInstance();
    MySQLUtils::destroyInstance();
    RedisUtils::destroyInstance();
    return 0;
}