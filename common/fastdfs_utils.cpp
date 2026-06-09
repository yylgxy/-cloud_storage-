#include "fastdfs_utils.h"
#include <cstring>
#include <cerrno>
#include <iostream>

using namespace std;

FastdfsUtils* FastdfsUtils::instance = nullptr;

FastdfsUtils::FastdfsUtils() : initialized(false) {}

FastdfsUtils::~FastdfsUtils() {
    if (initialized) {
        fdfs_client_destroy();
    }
}

FastdfsUtils* FastdfsUtils::getInstance() {
    if (!instance) {
        instance = new FastdfsUtils();
    }
    return instance;
}

bool FastdfsUtils::init(const char* config_file) {
    if (initialized) return true;

    if (fdfs_client_init(config_file) != 0) {
        return false;
    }

    // 初始化时只加载配置，不保存任何连接
    g_log_context.log_level = 0;
    initialized = true;
    return true;
}

// 上传文件：每次从 tracker 获取临时连接
string FastdfsUtils::uploadFile(const string& local_file) {
    if (!initialized) return "";

    ConnectionInfo* tracker = tracker_get_connection();
    if (!tracker) {
        return "";
    }

    ConnectionInfo storage;
    memset(&storage, 0, sizeof(storage));
    char group_name[FDFS_GROUP_NAME_MAX_LEN + 1] = {0};
    char file_id[256] = {0};
    int store_path = 0;

    // 查询存储节点
    if (tracker_query_storage_store_without_group(tracker, &storage, group_name, &store_path) != 0) {
        cerr << "tracker_query_storage_store_without_group failed" << endl;
        tracker_close_connection_ex(tracker, false);
        return "";
    }

    // 调试输出：看看 store_path 和 group_name 是否正确
    cout << "DEBUG: group_name=" << group_name << ", store_path=" << store_path << endl;

    // ✅ 使用不带 cmd 参数的上传函数，避免 cmd 值错乱
    int ret = storage_upload_by_filename1(
        tracker,
        &storage,
        store_path,              // store_path_index
        local_file.c_str(),      // local_filename
        NULL,                    // file_ext_name （NULL 会自动提取扩展名）
        NULL,                    // meta_list
        0,                       // meta_count
        group_name,
        file_id
    );

    // 关闭连接
    tracker_close_connection_ex(&storage, false);
    tracker_close_connection_ex(tracker, false);

    if (ret == 0) {
        return file_id;
    }

    cerr << "storage_upload_by_filename1 failed, ret=" << ret 
         << ", errno=" << errno << " (" << strerror(errno) << ")" << endl;
    return "";
}

// 下载文件
bool FastdfsUtils::downloadFile(const string& file_id, const string& save_file) {
    if (!initialized) return false;

    ConnectionInfo* tracker = tracker_get_connection();
    if (!tracker) return false;

    ConnectionInfo storage;
    memset(&storage, 0, sizeof(storage));
    int64_t file_size = 0;

    int ret = tracker_query_storage_fetch1(tracker, &storage, file_id.c_str());
    if (ret != 0) {
        tracker_close_connection_ex(tracker, false);
        return false;
    }

    ret = storage_download_file_to_file1(
        tracker, &storage,
        file_id.c_str(), save_file.c_str(), &file_size
    );

    // 关闭连接
    tracker_close_connection_ex(&storage, false);
    tracker_close_connection_ex(tracker, false);

    return ret == 0;
}

// 删除文件
bool FastdfsUtils::deleteFile(const string& file_id) {
    if (!initialized) return false;

    ConnectionInfo* tracker = tracker_get_connection();
    if (!tracker) return false;

    ConnectionInfo storage;
    memset(&storage, 0, sizeof(storage));

    if (tracker_query_storage_fetch1(tracker, &storage, file_id.c_str()) != 0) {
        tracker_close_connection_ex(tracker, false);
        return false;
    }

    int ret = storage_delete_file(tracker, &storage, file_id.c_str(), NULL);

    tracker_close_connection_ex(&storage, false);
    tracker_close_connection_ex(tracker, false);

    return ret == 0;
}

string FastdfsUtils::getError() const {
    return strerror(errno);
}

void FastdfsUtils::destroyInstance() {
    delete instance;
    instance = nullptr;
}