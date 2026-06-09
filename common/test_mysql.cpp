#include "mysql_utils.h"
#include <iostream>
#include <string>

using namespace std;

int main() {
    MySQLUtils* db = MySQLUtils::getInstance();

    // 数据库信息（已适配你的ai_cloud_storage）
    // 从环境变量读取密码，见最新 commit\ndb->initInfo("127.0.0.1", "root", "CHANGE_ME", "ai_cloud_storage", 3306);

    if (!db->connect()) {
        cerr << "连接失败: " << db->getError() << endl;
        MySQLUtils::destroyInstance();
        return -1;
    }
    cout << "MySQL 连接成功！" << endl << endl;

    // ========== 测试1：插入测试用户（完全匹配你的user_info表字段） ==========
    // 密码是123456的MD5值，和你的MD5工具类结果完全一致
    int insert_rows = db->update(
        "INSERT INTO user_info (user_name, password_md5, nickname) "
        "VALUES ('test_user', 'e10adc3949ba59abbe56e057f20f883e', '测试用户')"
    );
    if (insert_rows < 0) {
        cerr << " 插入用户失败: " << db->getError() << endl;
    } else {
        cout << "插入用户成功，受影响行数: " << insert_rows << endl;
    }

    // ========== 测试2：查询所有用户（完全匹配你的字段） ==========
    cout << endl << " 查询所有用户：" << endl;
    MYSQL_RES* res = db->query("SELECT id, user_name, nickname, create_time FROM user_info");
    if (res == nullptr) {
        cerr << " 查询失败: " << db->getError() << endl;
    } else {
        // 打印表头
        cout << "ID\t用户名\t昵称\t创建时间" << endl;
        cout << "----------------------------------------" << endl;
        
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
            cout << (row[0] ? row[0] : "NULL") << "\t"
                 << (row[1] ? row[1] : "NULL") << "\t"
                 << (row[2] ? row[2] : "NULL") << "\t"
                 << (row[3] ? row[3] : "NULL") << endl;
        }
        db->freeResult(res);
    }

    // ========== 测试3：更新用户昵称 ==========
    int update_rows = db->update(
        "UPDATE user_info SET nickname = '修改后的昵称' WHERE user_name = 'test_user'"
    );
    if (update_rows < 0) {
        cerr << endl << " 更新用户失败: " << db->getError() << endl;
    } else {
        cout << endl << " 更新用户成功，受影响行数: " << update_rows << endl;
    }

    // ========== 测试4：删除测试用户 ==========
    int delete_rows = db->update("DELETE FROM user_info WHERE user_name = 'test_user'");
    if (delete_rows < 0) {
        cerr << endl << " 删除用户失败: " << db->getError() << endl;
    } else {
        cout << endl << " 删除用户成功，受影响行数: " << delete_rows << endl;
    }

    // ========== 测试5：验证错误处理 ==========
    cout << endl << " 测试错误处理（故意写错表名）：" << endl;
    if (db->update("INSERT INTO not_exist_table VALUES (1)") < 0) {
        cout << "预期错误: " << db->getError() << endl;
    }

    // 清理资源
    db->disconnect();
    MySQLUtils::destroyInstance();
    cout << endl << " 所有测试完成!MySQL工具类可以正常使用了" << endl;

    return 0;
}