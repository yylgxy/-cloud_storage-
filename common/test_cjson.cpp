#include "cjson_utils.h"
#include <iostream>
using namespace std;

int main() {
    CJsonUtils* json = CJsonUtils::getInstance();

    // 测试1：生成JSON
    cJSON* root = cJSON_CreateObject();
    json->addString(root, "file_id", "group1/M00/00/00/wKiWZWoPCLeAZV_yAAAADXkjG9s036.txt");
    json->addString(root, "file_name", "test.txt");
    json->addInt(root, "file_size", 1234);
    json->addBool(root, "is_public", true);

    string json_str = json->toString(root);
    cout << "生成的JSON：" << endl << json_str << endl;

    // 测试2：解析JSON
    cJSON* parsed = json->parse(json_str);
    if (parsed) {
        cout << endl << "解析结果：" << endl;
        cout << "file_id: " << json->getString(parsed, "file_id") << endl;
        cout << "file_size: " << json->getInt(parsed, "file_size") << endl;
        json->free(parsed);
    }

    // 测试3：保存到文件
    json->saveToFile(root, "file_info.json");
    cout << endl << "已保存到 file_info.json" << endl;

    // 释放资源
    json->free(root);
    CJsonUtils::destroyInstance();

    return 0;
}