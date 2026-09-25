#include "fcgi_stdio.h"
#include "mysql_utils.h"
#include "redis_utils.h"
#include "cjson_utils.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <time.h>
#include <stdlib.h>

using namespace std;

// 共享文件热度排行榜的 Redis ZSET 键名，与 ApiSharefiles 的 rank 命令共用。
// member = share_id，score = 下载量；写入端只做 ZINCRBY / ZREM，读取端做 ZREVRANGE。
static const char* RANK_KEY = "rank:share:hot";

int main() {
    MySQLUtils* db = MySQLUtils::getInstance();
    RedisUtils* redis = RedisUtils::getInstance();
    CJsonUtils* json = CJsonUtils::getInstance();

    db->initInfo("127.0.0.1", "root", MySQLUtils::getDbPassword(), "ai_cloud_storage", 3306);
    redis->initInfo("127.0.0.1", 6379);
    redis->connect();
    srand((unsigned int)time(nullptr));

    while (FCGI_Accept() >= 0) {
        char resp[2048] = {0};
        char token[128] = {0};
        char share_id[64] = {0};
        char cmd[32] = {0};
        char user_id_str[32] = {0};
        char sql[1024] = {0};
        char* query_str = getenv("QUERY_STRING");
        cJSON* root = nullptr;
        redisReply* reply = nullptr;
        MYSQL_RES* res = nullptr;
        MYSQL_ROW row;

        if (query_str == nullptr) {
            root = cJSON_CreateObject();
            json->addInt(root, "code", 1);
            json->addString(root, "msg", "请求参数为空");
            snprintf(resp, sizeof(resp), "%s", json->toString(root).c_str());
            json->free(root);
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            continue;
        }

        sscanf(query_str, "token=%127[^&]&share_id=%63[^&]&cmd=%31[^&]", token, share_id, cmd);

        if (strlen(token) == 0 || strlen(share_id) == 0 || strlen(cmd) == 0) {
            root = cJSON_CreateObject();
            json->addInt(root, "code", 1);
            json->addString(root, "msg", "缺少必要参数");
            snprintf(resp, sizeof(resp), "%s", json->toString(root).c_str());
            json->free(root);
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            continue;
        }

        // 验证Token
        string redis_cmd_str = "GET ";
        redis_cmd_str += token;
        reply = redis->command(redis_cmd_str);

        if (!reply || reply->type != REDIS_REPLY_STRING) {
            root = cJSON_CreateObject();
            json->addInt(root, "code", 4);
            json->addString(root, "msg", "Token无效或已过期");
            snprintf(resp, sizeof(resp), "%s", json->toString(root).c_str());
            json->free(root);
            redis->freeReply(reply);
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            continue;
        }

        strncpy(user_id_str, reply->str, sizeof(user_id_str) - 1);
        redis->freeReply(reply);

        if (!db->connect()) {
            root = cJSON_CreateObject();
            json->addInt(root, "code", 1);
            json->addString(root, "msg", "数据库连接失败");
            snprintf(resp, sizeof(resp), "%s", json->toString(root).c_str());
            json->free(root);
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            continue;
        }

        root = cJSON_CreateObject();

        // ====================== cancel：取消自己的文件分享 ======================
        if (strcmp(cmd, "cancel") == 0) {
            // 从user_info查询当前用户的实际用户名
            char user_name[32] = {0};
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
                json->addInt(root, "code", 1);
                json->addString(root, "msg", "取消分享失败：用户不存在");
            }
            else {
                // 先查询分享记录，确认所有权并获取 file_md5
                snprintf(sql, sizeof(sql),
                    "SELECT file_md5, file_name FROM share_file_list "
                    "WHERE share_id='%s' AND user_name='%s'",
                    share_id, user_name);
                res = db->query(sql);

                if (res == nullptr || mysql_num_rows(res) == 0) {
                    json->addInt(root, "code", 1);
                    json->addString(root, "msg", "取消分享失败：分享记录不存在或无权限");
                    db->freeResult(res);
                }
                else {
                    row = mysql_fetch_row(res);
                    char md5[128] = {0};
                    strncpy(md5, row[0], sizeof(md5) - 1);

                    // 删除分享记录
                    snprintf(sql, sizeof(sql),
                        "DELETE FROM share_file_list WHERE share_id='%s' AND user_name='%s'",
                        share_id, user_name);
                    int del_share = db->update(sql);

                    if (del_share > 0) {
                        // 更新user_file_list的分享状态为0
                        snprintf(sql, sizeof(sql),
                            "UPDATE user_file_list SET share_status=0 "
                            "WHERE file_md5='%s' AND share_status=1",
                            md5);
                        db->update(sql);

                        // 同步从热度排行榜摘除，否则已取消的分享还会挂在榜上
                        string zcmd = "ZREM ";
                        zcmd += RANK_KEY;
                        zcmd += " ";
                        zcmd += share_id;
                        redisReply* zr = redis->command(zcmd);
                        redis->freeReply(zr);

                        json->addInt(root, "code", 0);
                        json->addString(root, "msg", "取消分享成功");
                    } else {
                        json->addInt(root, "code", 1);
                        json->addString(root, "msg", "取消分享失败");
                    }
                    db->freeResult(res);
                }
            }
        }
        // ====================== save：转存文件到自己的网盘 ======================
        else if (strcmp(cmd, "save") == 0) {
            // 从user_info查询当前用户的实际用户名
            char user_name[32] = {0};
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
                json->addInt(root, "code", 1);
                json->addString(root, "msg", "转存失败：用户不存在");
            }
            else {
                // 查询分享的文件信息
                snprintf(sql, sizeof(sql),
                    "SELECT file_md5, file_name FROM share_file_list WHERE share_id='%s'",
                    share_id);
                res = db->query(sql);

                if (res == nullptr || mysql_num_rows(res) == 0) {
                    json->addInt(root, "code", 1);
                    json->addString(root, "msg", "转存失败：分享记录不存在");
                    db->freeResult(res);
                }
                else {
                    row = mysql_fetch_row(res);
                    char file_md5[128] = {0}, file_name[256] = {0};
                    strncpy(file_md5, row[0], sizeof(file_md5) - 1);
                    strncpy(file_name, row[1], sizeof(file_name) - 1);
                    db->freeResult(res);

                    // 检查是否已经转存过
                    snprintf(sql, sizeof(sql),
                        "SELECT id FROM user_file_list WHERE user_name='%s' AND file_md5='%s' LIMIT 1",
                        user_name, file_md5);
                    res = db->query(sql);

                    bool already_exists = (res && mysql_num_rows(res) > 0);
                    db->freeResult(res);

                    if (already_exists) {
                        json->addInt(root, "code", 1);
                        json->addString(root, "msg", "转存失败：文件已存在");
                    }
                    else {
                        // 插入到user_file_list
                        snprintf(sql, sizeof(sql),
                            "INSERT INTO user_file_list(user_name, file_md5, file_name, share_status, pv, create_time) "
                            "VALUES ('%s', '%s', '%s', 0, 0, NOW())",
                            user_name, file_md5, file_name);
                        int insert_ok = db->update(sql);

                        if (insert_ok > 0) {
                            // 增加file_info引用计数
                            snprintf(sql, sizeof(sql),
                                "UPDATE file_info SET ref_count=ref_count+1 WHERE file_md5='%s'",
                                file_md5);
                            db->update(sql);

                            // 更新user_file_count
                            snprintf(sql, sizeof(sql),
                                "INSERT INTO user_file_count(user_name, file_count) VALUES ('%s', 1) "
                                "ON DUPLICATE KEY UPDATE file_count=file_count+1",
                                user_name);
                            db->update(sql);

                            json->addInt(root, "code", 0);
                            json->addString(root, "msg", "转存成功");
                        } else {
                            json->addInt(root, "code", 1);
                            json->addString(root, "msg", "转存失败：插入记录出错");
                        }
                    }
                }
            }
        }
        // ====================== pv：增加下载量 ======================
        else if (strcmp(cmd, "pv") == 0) {
            // 更新share_file_list的下载量
            snprintf(sql, sizeof(sql),
                "UPDATE share_file_list SET pv=pv+1 WHERE share_id='%s'",
                share_id);
            int affected = db->update(sql);

            if (affected > 0) {
                // 同步维护 Redis 热度排行榜。
                // ZINCRBY 是单命令原子自增，多个用户同时下载同一个分享时
                // 不会像 UPDATE ... SET pv=pv+1 那样产生行锁排队。
                string zcmd = "ZINCRBY ";
                zcmd += RANK_KEY;
                zcmd += " 1 ";
                zcmd += share_id;

                redisReply* zr = redis->command(zcmd);
                double new_score = 0.0;
                if (zr != nullptr && zr->type == REDIS_REPLY_STRING) {
                    new_score = atof(zr->str);
                } else if (zr != nullptr && zr->type == REDIS_REPLY_INTEGER) {
                    new_score = (double)zr->integer;
                }
                redis->freeReply(zr);

                // 自愈：分数等于 1 说明这个 member 是本次请求刚创建的，即 Redis 里原本没有它
                // （Redis 重启丢数据，或该分享创建于排行榜功能上线之前）。
                // 此时用 MySQL 的真实 pv 覆盖，避免榜上分数从 1 开始失真。
                if (new_score <= 1.0) {
                    char pv_sql[512];
                    snprintf(pv_sql, sizeof(pv_sql),
                        "SELECT pv FROM share_file_list WHERE share_id='%s' LIMIT 1",
                        share_id);
                    MYSQL_RES* pv_res = db->query(pv_sql);
                    if (pv_res != nullptr && mysql_num_rows(pv_res) > 0) {
                        MYSQL_ROW pv_row = mysql_fetch_row(pv_res);
                        char zadd[256];
                        snprintf(zadd, sizeof(zadd), "ZADD %s %s %s",
                                 RANK_KEY, (pv_row[0] != nullptr ? pv_row[0] : "0"), share_id);
                        redisReply* zr2 = redis->command(zadd);
                        redis->freeReply(zr2);
                    }
                    db->freeResult(pv_res);
                }

                json->addInt(root, "code", 0);
                json->addString(root, "msg", "下载量更新成功");
            } else {
                json->addInt(root, "code", 1);
                json->addString(root, "msg", "下载量更新失败：分享记录不存在");
            }
        }
        else {
            json->addInt(root, "code", 1);
            json->addString(root, "msg", "未知操作指令");
        }

        snprintf(resp, sizeof(resp), "%s", json->toString(root).c_str());
        json->free(root);
        db->disconnect();
        FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
    }

    MySQLUtils::destroyInstance();
    RedisUtils::destroyInstance();
    CJsonUtils::destroyInstance();
    return 0;
}
