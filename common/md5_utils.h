#ifndef MD5_UTILS_H
#define MD5_UTILS_H
#include <string>
//MD5算法工具类
class MD5Utils {
public:
    static std::string md5(const std::string& str); //MD5加密函数，输入字符串，返回加密后的字符串
    static std::string md5_file(const std::string& file_path);//MD5加密函数，输入文件路径，返回加密后的字符串
};
#endif