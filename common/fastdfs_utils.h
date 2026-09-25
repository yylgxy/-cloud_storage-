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

    /**
     * 负载均衡开关（可灰度）。
     *   true  —— 上传时由本地改进算法选组（见 lb_tracker.h）
     *   false —— 完全走 FastDFS 原生选盘链路（tracker_query_storage_store_without_group）
     * 默认关闭，部署时通过 setLoadBalanceEnabled(true) 或环境变量 LB_ENABLE=1 打开，
     * 出问题可以立刻改配置回退，不需要重新编译。
     */
    static void setLoadBalanceEnabled(bool enabled);
    static bool isLoadBalanceEnabled();

    bool init(const char* config_file);
    std::string uploadFile(const std::string& local_file);
    bool downloadFile(const std::string& file_id, const std::string& save_file);
    bool deleteFile(const std::string& file_id);
    std::string getError() const;

private:
    FastdfsUtils();
    ~FastdfsUtils();

    static FastdfsUtils* instance;
    static bool lb_enabled;   // 负载均衡开关
    bool initialized;         // 标记全局库是否已初始化成功
};

#endif
