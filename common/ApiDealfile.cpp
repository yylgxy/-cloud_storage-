#include "fcgi_stdio.h"
#include "fastdfs_utils.h"
#include "mysql_utils.h"
#include "redis_utils.h"
#include "cjson_utils.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <time.h>
#include <stdlib.h>

using namespace std;

int main() {
    MySQLUtils* db = MySQLUtils::getInstance();
    RedisUtils* redis = RedisUtils::getInstance();
    CJsonUtils* json = CJsonUtils::getInstance();
    FastdfsUtils* fdfs = FastdfsUtils::getInstance();

    db->initInfo("127.0.0.1", "root", MySQLUtils::getDbPassword(), "ai_cloud_storage", 3306);
    redis->initInfo("127.0.0.1", 6379);
    redis->connect();
    fdfs->init("/etc/fdfs/client.conf");
    srand((unsigned int)time(nullptr));

    while (FCGI_Accept() >= 0) {
        char resp[2048] = {0};
        char token[128] = {0};
        char file_md5[128] = {0};
        char cmd[32] = {0};
        char user_id_str[32] = {0};
        char sql[1024] = {0};
        char* query_str = getenv("QUERY_STRING");
        cJSON* root = nullptr;
        redisReply* reply = nullptr;
        MYSQL_RES* res = nullptr;
        MYSQL_ROW row;
        char file_name[256] = {0};

        if (query_str == nullptr) {
            root = cJSON_CreateObject();
            json->addInt(root, "code", 1);
            json->addString(root, "msg", "请求参数为空");
            snprintf(resp, sizeof(resp), "%s", json->toString(root).c_str());
            json->free(root);
        }
        else {
            sscanf(query_str, "token=%127[^&]&file_md5=%127[^&]&cmd=%31[^&]", token, file_md5, cmd);
            
            string redis_cmd = "GET ";
            redis_cmd += token;
            reply = redis->command(redis_cmd);
            if (!reply || reply->type != REDIS_REPLY_STRING) {
                root = cJSON_CreateObject();
                json->addInt(root, "code", 1);
                json->addString(root, "msg", "Token无效或已过期");
                snprintf(resp, sizeof(resp), "%s", json->toString(root).c_str());
                json->free(root);
                redis->freeReply(reply);
            }
            else {
                strncpy(user_id_str, reply->str, sizeof(user_id_str) - 1);
                redis->freeReply(reply);

                if (!db->connect()) {
                    root = cJSON_CreateObject();
                    json->addInt(root, "code", 1);
                    json->addString(root, "msg", "数据库连接失败");
                    snprintf(resp, sizeof(resp), "%s", json->toString(root).c_str());
                    json->free(root);
                }
                else {
                    // 从user_info查询实际用户名
                    char user_name[32] = {0};

                    // 检查MySQL连接，断开则重连
                    if (!db->isConnected()) {
                        db->connect();
                    }

                    snprintf(sql, sizeof(sql),
                        "SELECT user_name FROM user_info WHERE id=%s LIMIT 1",
                        user_id_str);
                    res = db->query(sql);
                    if (res && mysql_num_rows(res) > 0) {
                        row = mysql_fetch_row(res);
                        strncpy(user_name, row[0], sizeof(user_name) - 1);
                    }
                    db->freeResult(res);

                    if (strlen(user_name) == 0) {
                        root = cJSON_CreateObject();
                        json->addInt(root, "code", 1);
                        json->addString(root, "msg", "用户不存在");
                        snprintf(resp, sizeof(resp), "%s", json->toString(root).c_str());
                        json->free(root);
                        db->disconnect();
                        FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
                        continue;
                    }

                    root = cJSON_CreateObject();

                    // ====================== 删除操作（含实际文件删除）======================
                    if (strcmp(cmd, "del") == 0) {
                        // 先查询 file_info 获取当前引用计数和文件ID
                        char file_id[256] = {0};
                        int ref_count = 0;
                        snprintf(sql, sizeof(sql), "SELECT file_id, ref_count FROM file_info WHERE file_md5='%s'", file_md5);
                        res = db->query(sql);
                        if (res && mysql_num_rows(res) > 0) {
                            row = mysql_fetch_row(res);
                            if (row[0]) strncpy(file_id, row[0], sizeof(file_id) - 1);
                            if (row[1]) ref_count = atoi(row[1]);
                        }
                        db->freeResult(res);

                        // 删除用户文件记录
                        snprintf(sql, sizeof(sql), "DELETE FROM user_file_list WHERE user_name='%s' AND file_md5='%s'", user_name, file_md5);
                        int affected1 = db->update(sql);

                        if (affected1 > 0) {
                            // 减少引用计数
                            snprintf(sql, sizeof(sql), "UPDATE file_info SET ref_count=ref_count-1 WHERE file_md5='%s'", file_md5);
                            db->update(sql);

                            // 减少用户文件计数
                            snprintf(sql, sizeof(sql), "UPDATE user_file_count SET file_count=file_count-1 WHERE user_name='%s'", user_name);
                            db->update(sql);

                            // 清理分享记录
                            snprintf(sql, sizeof(sql), "DELETE FROM share_file_list WHERE user_name='%s' AND file_md5='%s'", user_name, file_md5);
                            db->update(sql);

                            // 如果只有最后一个人引用，删除实际文件
                            if (ref_count <= 1 && strlen(file_id) > 0) {
                                fdfs->deleteFile(file_id);
                                snprintf(sql, sizeof(sql), "DELETE FROM file_info WHERE file_md5='%s'", file_md5);
                                db->update(sql);
                            }

                            json->addInt(root, "code", 0);
                            json->addString(root, "msg", "文件删除成功");
                        } else {
                            json->addInt(root, "code", 1);
                            json->addString(root, "msg", "文件删除失败：未找到对应记录或用户名不匹配");
                        }
                    }
                    // ====================== 取消分享 ======================
                    else if (strcmp(cmd, "unshare") == 0) {
                        snprintf(sql, sizeof(sql), "UPDATE user_file_list SET share_status=0 WHERE user_name='%s' AND file_md5='%s'", user_name, file_md5);
                        db->update(sql);
                        snprintf(sql, sizeof(sql), "DELETE FROM share_file_list WHERE user_name='%s' AND file_md5='%s'", user_name, file_md5);
                        int affected = db->update(sql);

                        if (affected > 0) {
                            json->addInt(root, "code", 0);
                            json->addString(root, "msg", "取消分享成功");
                        } else {
                            json->addInt(root, "code", 1);
                            json->addString(root, "msg", "取消分享失败：未找到分享记录");
                        }
                    }
                    // ====================== 分享操作（带提取码+有效期+分享链接）======================
                    else if (strcmp(cmd, "share") == 0) {
                        memset(file_name, 0, sizeof(file_name));

                        // 解析可选参数 expire（1h/24h/7d/30d/never）和 extract（on/off/自定义）
                        char expire_opt[16] = "never";
                        char extract_opt[32] = "off";
                        char tmp_expire[16] = {0}, tmp_extract[32] = {0};
                        if (sscanf(query_str, "%*[^&]&%*[^&]&%*[^&]&expire=%15[^&]&extract=%31[^&]", tmp_expire, tmp_extract) >= 1) {
                            strncpy(expire_opt, tmp_expire, sizeof(expire_opt) - 1);
                            strncpy(extract_opt, tmp_extract, sizeof(extract_opt) - 1);
                        }

                        // 更新分享状态
                        snprintf(sql, sizeof(sql), "UPDATE user_file_list SET share_status=1 WHERE user_name='%s' AND file_md5='%s'", user_name, file_md5);
                        db->update(sql);

                        // 查询文件名
                        snprintf(sql, sizeof(sql), "SELECT file_name FROM user_file_list WHERE user_name='%s' AND file_md5='%s'", user_name, file_md5);
                        res = db->query(sql);
                        if (res != nullptr) {
                            row = mysql_fetch_row(res);
                            if (row != nullptr && row[0] != nullptr) {
                                strncpy(file_name, row[0], sizeof(file_name)-1);
                            } else {
                                strcpy(file_name, "unknown_file");
                            }
                            db->freeResult(res);
                        }

                        // 生成share_id
                        char share_id[64] = {0};
                        int rand_num = rand() % 10000;
                        snprintf(share_id, sizeof(share_id), "share_%ld_%04d", time(nullptr), rand_num);

                        // 生成提取码
                        char extract_code[16] = {0};
                        if (strcmp(extract_opt, "on") == 0) {
                            // 随机6位数字
                            snprintf(extract_code, sizeof(extract_code), "%06d", rand() % 1000000);
                        } else if (strcmp(extract_opt, "off") != 0 && strlen(extract_opt) > 0) {
                            // 使用自定义提取码
                            strncpy(extract_code, extract_opt, sizeof(extract_code) - 1);
                        }
                        // 默认为空（无提取码）

                        // 计算过期时间
                        char expire_time_str[64] = "NULL";
                        char expire_display[32] = "永久有效";
                        if (strcmp(expire_opt, "1h") == 0) {
                            snprintf(expire_time_str, sizeof(expire_time_str), "DATE_ADD(NOW(), INTERVAL 1 HOUR)");
                            strcpy(expire_display, "1小时");
                        } else if (strcmp(expire_opt, "24h") == 0) {
                            snprintf(expire_time_str, sizeof(expire_time_str), "DATE_ADD(NOW(), INTERVAL 24 HOUR)");
                            strcpy(expire_display, "24小时");
                        } else if (strcmp(expire_opt, "7d") == 0) {
                            snprintf(expire_time_str, sizeof(expire_time_str), "DATE_ADD(NOW(), INTERVAL 7 DAY)");
                            strcpy(expire_display, "7天");
                        } else if (strcmp(expire_opt, "30d") == 0) {
                            snprintf(expire_time_str, sizeof(expire_time_str), "DATE_ADD(NOW(), INTERVAL 30 DAY)");
                            strcpy(expire_display, "30天");
                        }
                        // never = 永久有效（NULL）

                        // 生成分享链接
                        char share_url[256] = {0};
                        snprintf(share_url, sizeof(share_url), "http://127.0.0.1/share?shareid=%s", share_id);

                        // 构造INSERT（expire_time用SQL函数或NULL）
                        char insert_sql[2048];
                        if (strcmp(expire_opt, "never") == 0) {
                            snprintf(insert_sql, sizeof(insert_sql),
                                "INSERT INTO share_file_list(share_id, user_name, file_md5, file_name, extract_code, expire_time, share_url, pv) "
                                "VALUES('%s', '%s', '%s', '%s', '%s', NULL, '%s', 0)",
                                share_id, user_name, file_md5, file_name,
                                strlen(extract_code) > 0 ? extract_code : "",
                                share_url);
                        } else {
                            snprintf(insert_sql, sizeof(insert_sql),
                                "INSERT INTO share_file_list(share_id, user_name, file_md5, file_name, extract_code, expire_time, share_url, pv) "
                                "VALUES('%s', '%s', '%s', '%s', '%s', %s, '%s', 0)",
                                share_id, user_name, file_md5, file_name,
                                strlen(extract_code) > 0 ? extract_code : "",
                                expire_time_str, share_url);
                        }

                        int insert_ok = db->update(insert_sql);

                        if (insert_ok > 0) {
                            json->addInt(root, "code", 0);
                            json->addString(root, "msg", "文件分享成功");
                            json->addString(root, "share_id", share_id);
                            json->addString(root, "share_url", share_url);
                            json->addString(root, "extract_code", strlen(extract_code) > 0 ? extract_code : "");
                            json->addString(root, "expire", expire_display);
                        } else {
                            json->addInt(root, "code", 1);
                            json->addString(root, "msg", "文件分享失败：插入记录出错");
                        }
                    }
                    // ====================== 下载量操作（已修改：自动补救） ======================
                    else if (strcmp(cmd, "pv") == 0) {
                        // 先尝试更新 user_file_list 中的下载量
                        snprintf(sql, sizeof(sql), "UPDATE user_file_list SET pv=pv+1 WHERE user_name='%s' AND file_md5='%s'", user_name, file_md5);
                        int affected = db->update(sql);
                        
                        if (affected > 0) {
                            json->addInt(root, "code", 0);
                            json->addString(root, "msg", "下载量更新成功");
                        } else {
                            // 没有找到记录，尝试从 share_file_list 中获取文件名并自动创建
                            snprintf(sql, sizeof(sql), "SELECT file_name FROM share_file_list WHERE user_name='%s' AND file_md5='%s' LIMIT 1", user_name, file_md5);
                            res = db->query(sql);
                            bool found_in_share = false;
                            char auto_file_name[256] = {0};
                            if (res != nullptr) {
                                row = mysql_fetch_row(res);
                                if (row != nullptr && row[0] != nullptr) {
                                    strncpy(auto_file_name, row[0], sizeof(auto_file_name)-1);
                                    found_in_share = true;
                                }
                                db->freeResult(res);
                            }
                            
                            if (found_in_share) {
                                // 自动插入 user_file_list 记录（分享状态为0，下载量初始0）
                                snprintf(sql, sizeof(sql),
                                    "INSERT INTO user_file_list(user_name, file_md5, file_name, share_status, pv, create_time) "
                                    "VALUES ('%s', '%s', '%s', 0, 0, NOW())",
                                    user_name, file_md5, auto_file_name);
                                int insert_ok = db->update(sql);
                                if (insert_ok > 0) {
                                    // 再次增加下载量
                                    snprintf(sql, sizeof(sql), "UPDATE user_file_list SET pv=pv+1 WHERE user_name='%s' AND file_md5='%s'", user_name, file_md5);
                                    db->update(sql);
                                    json->addInt(root, "code", 0);
                                    json->addString(root, "msg", "下载量更新成功（已自动创建文件记录）");
                                } else {
                                    json->addInt(root, "code", 1);
                                    json->addString(root, "msg", "下载量更新失败：无法创建文件记录");
                                }
                            } else {
                                json->addInt(root, "code", 1);
                                json->addString(root, "msg", "下载量更新失败：未找到对应文件记录，且无分享记录可追溯");
                            }
                        }
                    }
                    else {
                        json->addInt(root, "code", 1);
                        json->addString(root, "msg", "未知操作指令");
                    }

                    snprintf(resp, sizeof(resp), "%s", json->toString(root).c_str());
                    json->free(root);
                    db->disconnect();
                }
            }
        }
        FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
    }

    MySQLUtils::destroyInstance();
    RedisUtils::destroyInstance();
    CJsonUtils::destroyInstance();
    FastdfsUtils::destroyInstance();
    return 0;
}