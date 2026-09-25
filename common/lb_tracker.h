#ifndef LB_TRACKER_H
#define LB_TRACKER_H

#include <string>
#include <map>
#include "lb_algorithm.h"

extern "C" {
#include <fastdfs/fdfs_client.h>
}

/**
 * 把改进的负载均衡算法接到 FastDFS tracker 的选盘链路上。
 *
 * 为什么只接管"组级"决策
 * ----------------------
 * FastDFS 的选盘是一条三级瀑布：
 *     store_lookup   选组    （默认 2 = 剩余空间最大的组）
 *     store_server   选节点  （默认 0 = 轮询）
 *     store_path     选路径  （默认 0 = 轮询）
 * 容量差异与介质差异都发生在**组**这一级——一个组通常对应一批同构机器，
 * 而组内节点是同构的，用原生轮询就足够均衡。所以客户端只需要接管第一级：
 *   1) tracker_list_groups() 拉取所有组的 total_mb / free_mb；
 *   2) 用改进算法（水位过滤 + 加权评分）算出目标组；
 *   3) tracker_query_storage_store_with_group() 让 tracker 在该组内正常选节点。
 *
 * 这样做**不需要修改 FastDFS 源码**，并且可以灰度：开关打开走新逻辑，
 * 关闭时回退到 tracker_query_storage_store_without_group() 的原生行为。
 * 相比直接改 tracker 源码的方案，升级 FastDFS 时不会产生 merge 冲突。
 *
 * 依赖：
 *   - libfastdfs 客户端库（fastdfs/fdfs_client.h）
 *   - lb_algorithm.h
 * 编译：见 README「负载均衡」一节。
 */
class LbTracker {
public:
    static LbTracker* getInstance();
    static void destroyInstance();

    /**
     * 加载节点画像配置（磁盘性能系数、并发上限）。
     * 文件格式（每行一条，'#' 开头为注释）：
     *     group1   = 1.00:96      # 组级：SSD 组，并发上限 96
     *     10.0.0.21 = 0.55:48     # 节点级：HDD 节点
     * 未配置的条目使用默认值（perf=1.0，capacity=默认并发上限），因此本文件可选。
     */
    bool loadNodeProfile(const char* conf_path);

    /**
     * 选盘主入口。
     * 返回 0  使用改进算法成功
     * 返回 1  改进算法不可用，已回退到 FastDFS 原生选盘
     * 返回 -1 完全失败
     */
    int selectStorage(ConnectionInfo* tracker,
                      ConnectionInfo* outStorage,
                      char* group_name,
                      int* store_path,
                      LbStrategy strategy);

    /** 查询某个 key（组名或 IP）配置的磁盘性能系数 */
    double diskPerfOf(const std::string& key) const;
    /** 查询某个 key 配置的并发上限 */
    int capacityOf(const std::string& key) const;

    void setDefaultDiskPerf(double perf) { default_disk_perf_ = perf; }
    void setDefaultCapacity(int cap) { default_capacity_ = cap; }

private:
    LbTracker();
    ~LbTracker();

    std::map<std::string, double> perf_map_;
    std::map<std::string, int>    cap_map_;
    double default_disk_perf_;
    int    default_capacity_;

    static LbTracker* instance_;
};

#endif // LB_TRACKER_H
