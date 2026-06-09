//防止头文件被重复包含
#ifndef MYSQL_UTILS_H
#define MYSQL_UTILS_H
#include <mysql/mysql.h>  //加入MySQL C API头文件
#include <string>
//MySQL数据库工具类
class MySQLUtils 
{
    private:
        static MySQLUtils* instance; //单例实例指针
        MYSQL* mysql; //MySQL连接对象指针
        std::string host; //数据库主机地址
        std::string user; //数据库用户名
        std::string password; //数据库密码
        std::string database; //数据库名称
        unsigned int port; //数据库端口号
    public:
        //构造函数和析构函数公开，允许线程局部实例
        MySQLUtils();
        ~MySQLUtils();
        //获取单例实例的静态方法
        static MySQLUtils* getInstance();
        //初始化数据库信息
        void initInfo(const std::string& host, const std::string& user, const std::string& password, const std::string& database, unsigned int port = 3306);
        //连接数据库
        bool connect();
        //执行SQL查询，返回结果集指针
        MYSQL_RES* query(const std::string& sql);
        //执行SQL更新，返回受影响的行数
        int update(const std::string& sql);
        //检查是否已连接
        bool isConnected() { return mysql != nullptr && mysql_ping(mysql) == 0; }
        //关闭数据库连接
        void disconnect();
        //释放结果集内存
        void freeResult(MYSQL_RES* result);
        //错误信息获得接口
        std::string getError() const;
        //需要加一个静态方法来销毁单例实例，防止内存泄漏
        static void destroyInstance();
        // 从环境变量 DB_PASSWORD 读取密码，避免硬编码
        static std::string getDbPassword();
};
#endif
    
