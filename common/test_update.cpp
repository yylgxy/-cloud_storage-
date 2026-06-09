#include "mysql_utils.h"
#include <cstdio>

int main() {
    MySQLUtils* db = MySQLUtils::getInstance();
    db->initInfo("127.0.0.1", "root", MySQLUtils::getDbPassword(), "ai_cloud_storage", 3306);

    if (!db->connect()) {
        printf("数据库连接失败: %s\n", db->getError().c_str());
        return 1;
    }

    // 测试插入一条数据
    char sql[1024];
    snprintf(sql, sizeof(sql), 
        "INSERT INTO user_file_list(user_name, file_md5, file_name) VALUES('test008', 'test_md5', 'test.txt')");
    
    int affected = db->update(sql);
    printf("执行SQL: %s\n", sql);
    printf("受影响行数: %d\n", affected);
    printf("错误信息: %s\n", db->getError().c_str());

    db->disconnect();
    MySQLUtils::destroyInstance();
    return 0;
}