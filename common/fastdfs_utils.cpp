#include "fastdfs_utils.h"
#include "lb_tracker.h"
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <iostream>

using namespace std;

FastdfsUtils* FastdfsUtils::instance = nullptr;
// 默认开启：改进算法已实现并通过基准测试（见 docs/ 与 README），
// 缺省即接管组级选盘；需要回退原生逻辑时设 LB_ENABLE=0。
bool FastdfsUtils::lb_enabled = true;

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

void FastdfsUtils::setLoadBalanceEnabled(bool enabled) {
    lb_enabled = enabled;
}

bool FastdfsUtils::isLoadBalanceEnabled() {
    return lb_enabled;
}

bool FastdfsUtils::init(const char* config_file) {
    if (initialized) return true;

    if (fdfs_client_init(config_file) != 0) {
        return false;
    }

    // 初始化时只加载配置，不保存任何连接
    g_log_context.log_level = 0;
    initialized = true;

    // 负载均衡：默认开启，客户端接管组级选盘（水位过滤 + 加权评分）。
    // 需要回退 FastDFS 原生选盘时，设置环境变量 LB_ENABLE=0 即可。
    // 节点画像文件默认取 /etc/fdfs/lb_nodes.conf，可用 LB_NODE_PROFILE 覆盖；
    // 画像缺失不影响正确性——LbTracker 会退回默认同构画像，
    // 且拉取组信息失败等异常情况均会自动回退到原生接口。
    const char* lb_env = getenv("LB_ENABLE");
    setLoadBalanceEnabled(!(lb_env != nullptr && strcmp(lb_env, "0") == 0));

    const char* profile = getenv("LB_NODE_PROFILE");
    if (profile == nullptr) profile = "/etc/fdfs/lb_nodes.conf";
    LbTracker::getInstance()->loadNodeProfile(profile);

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

    // ------------------------------------------------------------------
    // 选盘：由 LbTracker 统一决定。
    //   开关关闭 → 等价于原来的 tracker_query_storage_store_without_group
    //   开关打开 → 客户端先按"水位过滤 + 加权评分"选组，再让 tracker 在组内选节点
    // ------------------------------------------------------------------
    LbTracker* lb_tracker = LbTracker::getInstance();
    LbStrategy strategy = isLoadBalanceEnabled() ? LB_WEIGHTED_SCORE : LB_MAX_FREE_SPACE;

    int pick = lb_tracker->selectStorage(tracker, &storage, group_name, &store_path, strategy);
    if (pick < 0) {
        cerr << "selectStorage failed" << endl;
        tracker_close_connection_ex(tracker, false);
        return "";
    }

    // 调试输出：看看选中的组与路径
    cout << "DEBUG: group_name=" << group_name
         << ", store_path=" << store_path
         << ", strategy=" << (strategy == LB_WEIGHTED_SCORE ? "weighted" : "native") << endl;

    // 在途任务计数：供下一次决策的空闲度维度使用
    LbAlgorithm* lb = LbAlgorithm::getInstance();
    string inflight_key(group_name);
    lb->onTaskBegin(inflight_key);

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

    lb->onTaskEnd(inflight_key);

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
