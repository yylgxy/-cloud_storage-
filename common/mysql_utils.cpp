#include "mysql_utils.h"
using namespace std;
MySQLUtils* MySQLUtils::instance = nullptr; //初始化单例实例指针
MySQLUtils::MySQLUtils() : mysql(nullptr) {} //构造函数初始化mysql指针为nullptr
MySQLUtils::~MySQLUtils() {
    disconnect(); //析构函数关闭数据库连接
}
//获取单例实例的静态方法
MySQLUtils* MySQLUtils::getInstance() {
    if(instance == nullptr) {
        instance = new MySQLUtils();
    }
    return instance;
}
//初始化数据库信息
void MySQLUtils::initInfo(const string& host, const string& user, const string& password, const string& database, unsigned int port) {
    this->host = host;
    this->user = user;
    this->password = password;  
    this->database = database;
    this->port = port;
}
//连接数据库
bool MySQLUtils::connect() {
    if(mysql != nullptr) {
        mysql_close(mysql); //如果已经有连接，先关闭
    }
    mysql = mysql_init(nullptr); //初始化MySQL连接对象
    if(mysql == nullptr) {
        return false;
    }
    if(!mysql_real_connect(mysql, host.c_str(), user.c_str(), password.c_str(), database.c_str(), port, nullptr, 0)) {
        mysql_close(mysql);
        mysql = nullptr;
        return false;
    }
    mysql_set_character_set(mysql, "utf8mb4"); //设置字符集为utf8mb4
    return true;
}
//执行SQL查询，返回结果集指针
MYSQL_RES* MySQLUtils::query(const string& sql) {
    if(mysql == nullptr) {
        return nullptr; //未连接数据库
    }
    if(mysql_query(mysql, sql.c_str()) != 0) {
        return nullptr;
    }
    return mysql_store_result(mysql);
}
//执行SQL更新，返回受影响的行数
int MySQLUtils::update(const string& sql) {
    if(mysql == nullptr) {
        return -1; //未连接数据库
    }
    if(mysql_query(mysql, sql.c_str()) != 0) {
        return -1; //失败
    }
    return static_cast<int>(mysql_affected_rows(mysql)); //返回受影响的行数
}
//关闭数据库连接
void MySQLUtils::disconnect() {
    if(mysql != nullptr) {
        mysql_close(mysql);
        mysql = nullptr;
    }
}
//释放结果集内存
void MySQLUtils::freeResult(MYSQL_RES* result) {
    if(result != nullptr) {
        mysql_free_result(result);
    }
}
//获取错误信息
string MySQLUtils::getError() const {
    if(mysql == nullptr) {
        return "Not connected to database";
    }
    const char* error = mysql_error(mysql);
    return error ? string(error) : "Unknown error";
}
//销毁单例实例，防止内存泄漏
void MySQLUtils::destroyInstance() {
    if(instance != nullptr) {
        delete instance;
        instance = nullptr;
    }
}