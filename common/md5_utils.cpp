#include "md5_utils.h"
#include <openssl/evp.h> // 新版OpenSSL通用摘要接口
#include <fstream>
#include <sstream>
#include <iomanip>
#include <stdexcept>

using namespace std;

string MD5Utils::md5(const std::string& str) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
    EVP_DigestUpdate(ctx, str.data(), str.size());
    EVP_DigestFinal_ex(ctx, digest, &digest_len);
    EVP_MD_CTX_free(ctx);

    stringstream ss;
    ss << hex << setfill('0');
    for(unsigned int i = 0; i < digest_len; i++) {
        ss << setw(2) << static_cast<int>(digest[i]);
    }
    return ss.str();
}

string MD5Utils::md5_file(const std::string& file_path) {
    ifstream file(file_path, ios::binary);
    if(!file.is_open()) {
        throw runtime_error("无法打开文件: " + file_path);
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    char buffer[4096];

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);

    while(file.read(buffer, sizeof(buffer))) {
        EVP_DigestUpdate(ctx, buffer, file.gcount());
    }
    // 读取最后剩余不足4KB的尾部内容
    EVP_DigestUpdate(ctx, buffer, file.gcount());

    EVP_DigestFinal_ex(ctx, digest, &digest_len);
    EVP_MD_CTX_free(ctx);

    stringstream ss;
    ss << hex << setfill('0');
    for(unsigned int i = 0; i < digest_len; i++) {
        ss << setw(2) << static_cast<int>(digest[i]);
    }
    return ss.str();
}