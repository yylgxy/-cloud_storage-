#include "cjson_utils.h"
#include <fstream>
#include <cstring>

using namespace std;

CJsonUtils* CJsonUtils::instance = nullptr;

CJsonUtils* CJsonUtils::getInstance() {
    if (!instance) {
        instance = new CJsonUtils();
    }
    return instance;
}

void CJsonUtils::destroyInstance() {
    if (instance) {
        delete instance;
        instance = nullptr;
    }
}

cJSON* CJsonUtils::parse(const string& json_str) {
    return cJSON_Parse(json_str.c_str());
}

string CJsonUtils::toString(cJSON* root, bool formatted) {
    if (!root) return "";
    char* buf = formatted ? cJSON_Print(root) : cJSON_PrintUnformatted(root);
    string res = buf;
    cJSON_free(buf);
    return res;
}

cJSON* CJsonUtils::loadFromFile(const string& file_path) {
    ifstream ifs(file_path);
    if (!ifs.is_open()) return nullptr;
    
    string content((istreambuf_iterator<char>(ifs)), istreambuf_iterator<char>());
    ifs.close();
    return parse(content);
}

bool CJsonUtils::saveToFile(cJSON* root, const string& file_path, bool formatted) {
    if (!root) return false;
    ofstream ofs(file_path);
    if (!ofs.is_open()) return false;
    
    ofs << toString(root, formatted);
    ofs.close();
    return true;
}

void CJsonUtils::free(cJSON* root) {
    if (root) {
        cJSON_Delete(root);
    }
}

int CJsonUtils::getInt(cJSON* root, const string& key, int default_val) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key.c_str());
    return (item && cJSON_IsNumber(item)) ? item->valuedouble : default_val;
}

string CJsonUtils::getString(cJSON* root, const string& key, const string& default_val) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key.c_str());
    return (item && cJSON_IsString(item)) ? item->valuestring : default_val;
}

bool CJsonUtils::getBool(cJSON* root, const string& key, bool default_val) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key.c_str());
    return (item && cJSON_IsBool(item)) ? cJSON_IsTrue(item) : default_val;
}

double CJsonUtils::getDouble(cJSON* root, const string& key, double default_val) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key.c_str());
    return (item && cJSON_IsNumber(item)) ? item->valuedouble : default_val;
}

void CJsonUtils::addInt(cJSON* root, const string& key, int value) {
    cJSON_AddNumberToObject(root, key.c_str(), value);
}

void CJsonUtils::addString(cJSON* root, const string& key, const string& value) {
    cJSON_AddStringToObject(root, key.c_str(), value.c_str());
}

void CJsonUtils::addBool(cJSON* root, const string& key, bool value) {
    cJSON_AddBoolToObject(root, key.c_str(), value);
}

void CJsonUtils::addDouble(cJSON* root, const string& key, double value) {
    cJSON_AddNumberToObject(root, key.c_str(), value);
}