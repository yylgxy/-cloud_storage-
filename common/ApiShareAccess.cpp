#include "fcgi_stdio.h"
#include "mysql_utils.h"
#include "cjson_utils.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace std;

int main() {
    MySQLUtils* db = MySQLUtils::getInstance();
    CJsonUtils* json = CJsonUtils::getInstance();

    db->initInfo("127.0.0.1", "root", "CHANGE_ME", "ai_cloud_storage", 3306);

    while (FCGI_Accept() >= 0) {
        char resp[2048] = {0};
        char share_id[64] = {0};
        char input_code[32] = {0};
        char sql[1024] = {0};
        char* query_str = getenv("QUERY_STRING");

        if (query_str == nullptr) {
            cJSON* root = cJSON_CreateObject();
            cJSON_AddNumberToObject(root, "code", 1);
            cJSON_AddStringToObject(root, "msg", "缺少参数");
            snprintf(resp, sizeof(resp), "%s", json->toString(root, false).c_str());
            json->free(root);
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            continue;
        }

        sscanf(query_str, "shareid=%63[^&]&code=%31[^&]", share_id, input_code);

        if (strlen(share_id) == 0) {
            cJSON* root = cJSON_CreateObject();
            cJSON_AddNumberToObject(root, "code", 1);
            cJSON_AddStringToObject(root, "msg", "缺少分享ID");
            snprintf(resp, sizeof(resp), "%s", json->toString(root, false).c_str());
            json->free(root);
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            continue;
        }

        if (!db->connect()) {
            cJSON* root = cJSON_CreateObject();
            cJSON_AddNumberToObject(root, "code", 50);
            cJSON_AddStringToObject(root, "msg", "数据库连接失败");
            snprintf(resp, sizeof(resp), "%s", json->toString(root, false).c_str());
            json->free(root);
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            continue;
        }

        // 查询分享记录
        snprintf(sql, sizeof(sql),
            "SELECT s.file_md5, s.file_name, s.extract_code, s.expire_time, s.share_url, s.pv, "
            "f.file_url, f.file_size "
            "FROM share_file_list s "
            "LEFT JOIN file_info f ON s.file_md5 = f.file_md5 "
            "WHERE s.share_id='%s' LIMIT 1",
            share_id);

        MYSQL_RES* res = db->query(sql);
        cJSON* root = cJSON_CreateObject();

        if (res == nullptr || mysql_num_rows(res) == 0) {
            cJSON_AddNumberToObject(root, "code", 2);
            cJSON_AddStringToObject(root, "msg", "分享链接不存在");
            db->freeResult(res);
            db->disconnect();
            snprintf(resp, sizeof(resp), "%s", json->toString(root, false).c_str());
            json->free(root);
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            continue;
        }

        MYSQL_ROW row = mysql_fetch_row(res);
        const char* file_md5      = row[0] ? row[0] : "";
        const char* file_name     = row[1] ? row[1] : "";
        const char* extract_code  = row[2] ? row[2] : "";
        const char* expire_time   = row[3] ? row[3] : "";
        const char* share_url     = row[4] ? row[4] : "";
        int pv                    = row[5] ? atoi(row[5]) : 0;
        const char* file_url      = row[6] ? row[6] : "";
        long long file_size       = row[7] ? atoll(row[7]) : 0;
        db->freeResult(res);

        // 检查是否过期
        if (strlen(expire_time) > 0) {
            snprintf(sql, sizeof(sql),
                "SELECT expire_time < NOW() FROM share_file_list WHERE share_id='%s'",
                share_id);
            MYSQL_RES* expire_res = db->query(sql);
            if (expire_res && mysql_num_rows(expire_res) > 0) {
                MYSQL_ROW expire_row = mysql_fetch_row(expire_res);
                if (expire_row[0] && strcmp(expire_row[0], "1") == 0) {
                    cJSON_AddNumberToObject(root, "code", 3);
                    cJSON_AddStringToObject(root, "msg", "分享链接已过期");
                    cJSON_AddStringToObject(root, "expire_time", expire_time);
                    db->freeResult(expire_res);
                    db->disconnect();
                    snprintf(resp, sizeof(resp), "%s", json->toString(root, false).c_str());
                    json->free(root);
                    FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
                    continue;
                }
            }
            db->freeResult(expire_res);
        }

        // 检查提取码
        bool need_code = (strlen(extract_code) > 0);
        if (need_code && strlen(input_code) == 0) {
            // 需要提取码但未提供
            cJSON_AddNumberToObject(root, "code", 4);
            cJSON_AddStringToObject(root, "msg", "需要提取码");
            cJSON_AddStringToObject(root, "share_id", share_id);
            cJSON_AddStringToObject(root, "file_name", file_name);
            cJSON_AddNumberToObject(root, "file_size", file_size);
            db->disconnect();
            snprintf(resp, sizeof(resp), "%s", json->toString(root, false).c_str());
            json->free(root);
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            continue;
        }

        if (need_code && strcmp(input_code, extract_code) != 0) {
            // 提取码错误
            cJSON_AddNumberToObject(root, "code", 5);
            cJSON_AddStringToObject(root, "msg", "提取码错误");
            db->disconnect();
            snprintf(resp, sizeof(resp), "%s", json->toString(root, false).c_str());
            json->free(root);
            FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
            continue;
        }

        // 校验通过（pv由前端操作时主动调用cmd=pv更新，此处不自动累加）
        db->disconnect();

        // 返回文件信息
        cJSON_AddNumberToObject(root, "code", 0);
        cJSON_AddStringToObject(root, "msg", "success");
        cJSON_AddStringToObject(root, "share_id", share_id);
        cJSON_AddStringToObject(root, "file_name", file_name);
        cJSON_AddNumberToObject(root, "file_size", file_size);
        cJSON_AddStringToObject(root, "file_url", file_url);
        cJSON_AddStringToObject(root, "file_md5", file_md5);

        snprintf(resp, sizeof(resp), "%s", json->toString(root, false).c_str());
        json->free(root);
        FCGI_printf("Content-Type: application/json\r\n\r\n%s", resp);
    }

    MySQLUtils::destroyInstance();
    CJsonUtils::destroyInstance();
    return 0;
}
