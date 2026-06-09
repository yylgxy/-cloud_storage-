#ifndef CJSON_UTILS_H
#define CJSON_UTILS_H

#include <string>
extern "C" {
#include <cjson/cJSON.h>
}

class CJsonUtils {
private:
    static CJsonUtils* instance;
    CJsonUtils() = default;
    ~CJsonUtils() = default;

public:
    // 单例获取
    static CJsonUtils* getInstance();
    static void destroyInstance();

    // 核心功能
    // 1. 解析JSON字符串
    cJSON* parse(const std::string& json_str);
    // 2. 生成JSON字符串（自动格式化）
    std::string toString(cJSON* root, bool formatted = true);
    // 3. 从文件加载JSON
    cJSON* loadFromFile(const std::string& file_path);
    // 4. 保存JSON到文件
    bool saveToFile(cJSON* root, const std::string& file_path, bool formatted = true);
    // 5. 安全释放JSON对象
    void free(cJSON* root);

    // 便捷获取值（带默认值，防止崩溃）
    int getInt(cJSON* root, const std::string& key, int default_val = 0);
    std::string getString(cJSON* root, const std::string& key, const std::string& default_val = "");
    bool getBool(cJSON* root, const std::string& key, bool default_val = false);
    double getDouble(cJSON* root, const std::string& key, double default_val = 0.0);

    // 便捷添加键值对
    void addInt(cJSON* root, const std::string& key, int value);
    void addString(cJSON* root, const std::string& key, const std::string& value);
    void addBool(cJSON* root, const std::string& key, bool value);
    void addDouble(cJSON* root, const std::string& key, double value);
};

#endif