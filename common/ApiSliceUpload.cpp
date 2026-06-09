#include "fcgi_stdio.h"
#include "fastdfs_utils.h"
#include "mysql_utils.h"
#include "redis_utils.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

using namespace std;

#define SLICE_DIR    "/home/fastdfs/upload_slice"
#define SLICE_SIZE   (5 * 1024 * 1024)

// ========== Multipart解析 ==========
static map<string, string> parse_multipart() {
    map<string, string> fields;

    char* content_type = getenv("CONTENT_TYPE");
    if (!content_type) return fields;

    const char* b = strstr(content_type, "boundary=");
    if (!b) return fields;
    string boundary = "--" + string(b + 9);

    string body;
    char buf[8192];
    int n;
    while ((n = FCGI_fread(buf, 1, sizeof(buf), FCGI_stdin)) > 0) {
        body.append(buf, n);
    }

    size_t pos = 0;
    while ((pos = body.find(boundary, pos)) != string::npos) {
        pos += boundary.length();
        if (body.compare(pos, 2, "\r\n") == 0) pos += 2;
        else if (body[pos] == '\n') pos += 1;
        else break;

        size_t header_end = body.find("\r\n\r\n", pos);
        if (header_end == string::npos) break;
        string headers = body.substr(pos, header_end - pos);
        pos = header_end + 4;

        string name;
        size_t np = headers.find("name=\"");
        if (np != string::npos) {
            np += 6;
            size_t ne = headers.find("\"", np);
            if (ne != string::npos)
                name = headers.substr(np, ne - np);
        }

        string filename;
        size_t fp = headers.find("filename=\"");
        if (fp != string::npos) {
            fp += 10;
            size_t fe = headers.find("\"", fp);
            if (fe != string::npos)
                filename = headers.substr(fp, fe - fp);
        }

        size_t data_end = body.find(boundary, pos);
        if (data_end == string::npos) break;

        size_t data_len = data_end - pos;
        if (data_len >= 2 && body[data_end - 2] == '\r' && body[data_end - 1] == '\n')
            data_len -= 2;
        else if (data_len >= 1 && body[data_end - 1] == '\n')
            data_len -= 1;

        string data = body.substr(pos, data_len);

        if (!filename.empty()) {
            char tmpname[] = "/tmp/slice_XXXXXX";
            int fd = mkstemp(tmpname);
            if (fd != -1) {
                write(fd, data.c_str(), data.size());
                close(fd);
                fields[name] = tmpname;
                fields[name + "_name"] = filename;
                fields[name + "_size"] = to_string(data.size());
            }
        } else {
            fields[name] = data;
        }
        pos = data_end;
    }
    return fields;
}

// ========== 清理分片 ==========
static void cleanup_slices(const char* filename, int total) {
    for (int i = 0; i < total; ++i) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s_%d_%d", SLICE_DIR, filename, total, i);
        unlink(path);
    }
}

// ========== Main ==========
int main() {
    mkdir(SLICE_DIR, 0755);

    FastdfsUtils* fdfs = FastdfsUtils::getInstance();
    if (!fdfs->init("/etc/fdfs/client.conf")) return 1;

    MySQLUtils* db = MySQLUtils::getInstance();
    db->initInfo("127.0.0.1", "root", "CHANGE_ME", "ai_cloud_storage", 3306);
    if (!db->connect()) return 1;

    RedisUtils* redis = RedisUtils::getInstance();
    redis->initInfo("127.0.0.1", 6379);
    redis->connect();

    while (FCGI_Accept() >= 0) {
        FCGI_printf("Content-Type: application/json\r\n\r\n");

        // ---------- 1. 解析参数 ----------
        auto fields = parse_multipart();
        string token_str = fields["token"];
        string fname_str = fields["filename"];
        string idx_str   = fields["slice_index"];
        string total_str = fields["slice_total"];
        string md5_str   = fields["file_md5"];
        string file_path = fields["file"];

        const char* token     = token_str.empty()  ? nullptr : token_str.c_str();
        const char* filename  = fname_str.empty()  ? nullptr : fname_str.c_str();
        const char* file_md5  = md5_str.empty()    ? nullptr : md5_str.c_str();
        const char* slice_tmp = file_path.empty()  ? nullptr : file_path.c_str();
        int slice_index = idx_str.empty()   ? -1 : atoi(idx_str.c_str());
        int slice_total = total_str.empty() ? 0  : atoi(total_str.c_str());

        if (!token || !filename || !file_md5 || slice_index < 0 || slice_total <= 0 || !slice_tmp) {
            if (slice_tmp) unlink(slice_tmp);
            FCGI_printf("{\"code\":1,\"msg\":\"缺少必要参数\"}");
            FCGI_Finish(); continue;
        }

        if (slice_index >= slice_total) {
            unlink(slice_tmp);
            FCGI_printf("{\"code\":2,\"msg\":\"分片序号超出范围\"}");
            FCGI_Finish(); continue;
        }

        // ---------- 2. 验证Token ----------
        char redis_cmd[256];
        snprintf(redis_cmd, sizeof(redis_cmd), "GET %s", token);
        redisReply* reply = redis->command(redis_cmd);
        if (!reply || reply->type != REDIS_REPLY_STRING) {
            FCGI_printf("{\"code\":4,\"msg\":\"Token无效或已过期\"}");
            if (reply) redis->freeReply(reply);
            unlink(slice_tmp); FCGI_Finish(); continue;
        }
        string user_id_str = reply->str;
        redis->freeReply(reply);

        char sql[2048];
        snprintf(sql, sizeof(sql),
            "SELECT user_name FROM user_info WHERE id=%s LIMIT 1",
            user_id_str.c_str());
        MYSQL_RES* res = db->query(sql);
        string user_name;
        if (res && mysql_num_rows(res) > 0) {
            MYSQL_ROW row = mysql_fetch_row(res);
            user_name = row[0] ? row[0] : "";
        }
        db->freeResult(res);

        if (user_name.empty()) {
            FCGI_printf("{\"code\":5,\"msg\":\"用户不存在\"}");
            unlink(slice_tmp); FCGI_Finish(); continue;
        }

        // ---------- 3. 保存分片到磁盘 ----------
        char slice_path[512];
        snprintf(slice_path, sizeof(slice_path),
            "%s/%s_%d_%d", SLICE_DIR, filename, slice_total, slice_index);

        ifstream exist_check(slice_path);
        if (!exist_check) {
            ifstream src(slice_tmp, ios::binary);
            ofstream dst(slice_path, ios::binary);
            if (src && dst) dst << src.rdbuf();
        }
        exist_check.close();
        unlink(slice_tmp);

        // ---------- 4. Redis记录已上传分片 ----------
        char redis_key[256];
        snprintf(redis_key, sizeof(redis_key), "upload:%s:slices", file_md5);
        snprintf(redis_cmd, sizeof(redis_cmd), "SADD %s %d", redis_key, slice_index);
        reply = redis->command(redis_cmd);
        redis->freeReply(reply);

        snprintf(redis_cmd, sizeof(redis_cmd), "EXPIRE %s 36000", redis_key);
        reply = redis->command(redis_cmd);
        redis->freeReply(reply);

        snprintf(redis_cmd, sizeof(redis_cmd), "SCARD %s", redis_key);
        reply = redis->command(redis_cmd);
        int uploaded = (reply && reply->type == REDIS_REPLY_INTEGER) ? (int)reply->integer : 0;
        redis->freeReply(reply);

        if (uploaded < slice_total) {
            FCGI_printf("{\"code\":0,\"msg\":\"分片接收成功\",\"uploaded\":%d,\"total\":%d}",
                uploaded, slice_total);
            FCGI_Finish(); continue;
        }

        // ========== 5. 全部分片到齐 → 合并 ==========
        char merge_path[512];
        snprintf(merge_path, sizeof(merge_path), "%s/%s_%d_merged",
            SLICE_DIR, filename, slice_total);

        ofstream merged(merge_path, ios::binary);
        if (!merged) {
            FCGI_printf("{\"code\":50,\"msg\":\"合并文件创建失败\"}");
            FCGI_Finish(); continue;
        }

        long long file_size = 0;
        bool merge_ok = true;
        for (int i = 0; i < slice_total; ++i) {
            char part[512];
            snprintf(part, sizeof(part), "%s/%s_%d_%d", SLICE_DIR, filename, slice_total, i);
            ifstream ifs(part, ios::binary);
            if (!ifs) {
                merge_ok = false;
                break;
            }
            string content((istreambuf_iterator<char>(ifs)), istreambuf_iterator<char>());
            ifs.close();
            file_size += content.size();
            merged.write(content.c_str(), content.size());
        }
        merged.close();

        if (!merge_ok) {
            unlink(merge_path);
            cleanup_slices(filename, slice_total);
            FCGI_printf("{\"code\":51,\"msg\":\"合并失败：分片缺失\"}");
            FCGI_Finish(); continue;
        }

        // ---------- 6. 上传FastDFS ----------
        string file_id = fdfs->uploadFile(merge_path);
        if (file_id.empty()) {
            unlink(merge_path);
            cleanup_slices(filename, slice_total);
            FCGI_printf("{\"code\":52,\"msg\":\"FastDFS上传失败\"}");
            FCGI_Finish(); continue;
        }

        // ---------- 7. 写数据库 ----------
        string file_url = "/" + file_id;

        // 检查MySQL连接，断开则重连
        if (!db->isConnected()) {
            db->connect();
        }

        snprintf(sql, sizeof(sql),
            "INSERT INTO file_info(file_md5, file_size, file_id, file_url, ref_count, create_time) "
            "VALUES('%s', %lld, '%s', '%s', 1, NOW()) "
            "ON DUPLICATE KEY UPDATE ref_count=ref_count+1",
            file_md5, file_size, file_id.c_str(), file_url.c_str());
        if (db->update(sql) < 0) {
            unlink(merge_path);
            cleanup_slices(filename, slice_total);
            FCGI_printf("{\"code\":1,\"msg\":\"数据库写入失败(file_info)\"}");
            FCGI_Finish(); continue;
        }

        snprintf(sql, sizeof(sql),
            "INSERT IGNORE INTO user_file_list(user_name, file_md5, file_name, share_status, pv, create_time) "
            "VALUES('%s', '%s', '%s', 0, 0, NOW())",
            user_name.c_str(), file_md5, filename);
        if (db->update(sql) < 0) {
            unlink(merge_path);
            cleanup_slices(filename, slice_total);
            FCGI_printf("{\"code\":1,\"msg\":\"数据库写入失败(user_file_list)\"}");
            FCGI_Finish(); continue;
        }

        snprintf(sql, sizeof(sql),
            "INSERT INTO user_file_count(user_name, file_count) VALUES('%s', 1) "
            "ON DUPLICATE KEY UPDATE file_count=file_count+1",
            user_name.c_str());
        if (db->update(sql) < 0) {
            unlink(merge_path);
            cleanup_slices(filename, slice_total);
            FCGI_printf("{\"code\":1,\"msg\":\"数据库写入失败(user_file_count)\"}");
            FCGI_Finish(); continue;
        }

        // ---------- 8. 清理 ----------
        unlink(merge_path);
        cleanup_slices(filename, slice_total);

        snprintf(redis_cmd, sizeof(redis_cmd), "DEL %s", redis_key);
        reply = redis->command(redis_cmd);
        redis->freeReply(reply);

        FCGI_printf("{\"code\":0,\"msg\":\"上传成功\",\"url\":\"%s\",\"file_name\":\"%s\"}",
            file_url.c_str(), filename);
        FCGI_Finish();
    }

    FastdfsUtils::destroyInstance();
    MySQLUtils::destroyInstance();
    RedisUtils::destroyInstance();
    return 0;
}
