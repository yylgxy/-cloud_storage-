#ifndef FASTDFS_UTILS_H
#define FASTDFS_UTILS_H

#include <string>
extern "C" {
#include <fastdfs/fdfs_client.h>
}

class FastdfsUtils {
public:
    static FastdfsUtils* getInstance();
    static void destroyInstance();

    bool init(const char* config_file);
    std::string uploadFile(const std::string& local_file);
    bool downloadFile(const std::string& file_id, const std::string& save_file);
    bool deleteFile(const std::string& file_id);
    std::string getError() const;

private:
    FastdfsUtils();
    ~FastdfsUtils();

    static FastdfsUtils* instance;
    bool initialized;   // 标记全局库是否已初始化成功
};

#endif