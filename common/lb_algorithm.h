#ifndef LB_ALGORITHM_H
#define LB_ALGORITHM_H

#include <string>
#include <vector>
#include <map>
#include <cstdint>

/**
 * 存储节点度量快照
 *
 * 说明：本结构体刻意不依赖任何 FastDFS 头文件，只承载"选盘决策需要的最小信息集"。
 * 这样做的收益是：
 *   1) 算法可以被单元测试 / 基准测试直接驱动，不需要真的搭一套 FastDFS 集群；
 *   2) 算法与上游 SDK 解耦，FastDFS 升级换字段只需改 lb_tracker 的采集层。
 * 字段来源见 lb_tracker.cpp：total_mb/free_mb 来自 tracker 上报的磁盘容量，
 * active_tasks 来自进程内统计 + storage 上报的连接数。
 */
struct StorageNode {
    std::string group_name;   // 所属组，例如 group1
    std::string ip_addr;      // 存储服务器 IP
    int         port;         // 存储服务器端口
    int64_t     total_mb;     // 磁盘总容量（MB）
    int64_t     free_mb;      // 磁盘剩余容量（MB）
    double      disk_perf;    // 磁盘性能系数：SSD=1.0 / SAS=0.75 / HDD=0.55
    int         active_tasks; // 当前在途写任务数
    int         task_capacity;// 该节点允许的并发写任务上限
    int         store_path;   // 该组内选定的 store_path 下标

    StorageNode()
        : port(23000), total_mb(0), free_mb(0),
          disk_perf(1.0), active_tasks(0), task_capacity(64), store_path(0) {}
};

/** 评分权重，三者归一化后加权求和 */
struct LbWeights {
    double space;  // 剩余空间比例权重
    double disk;   // 磁盘性能权重
    double load;   // 空闲度（1 - 任务使用率）权重

    LbWeights() : space(0.25), disk(0.45), load(0.30) {}
};

/** 选盘策略 */
enum LbStrategy {
    LB_MAX_FREE_SPACE = 0,  // 原生语义：等价 FastDFS store_lookup=2，只比剩余空间绝对值
    LB_WEIGHTED_SCORE = 1   // 改进：归一化加权评分 + 高低水位保护
};

/**
 * FastDFS 负载均衡决策器
 *
 * 改进动机（对应简历"难点 4"）：
 *   FastDFS 原生的"最大剩余空间"策略只比较 free_mb 的绝对值，存在两个局限：
 *     1) 未区分磁盘介质性能——容量大的机械盘永远压过 SSD，快盘被闲置；
 *     2) 未感知节点当前负载——同一个空闲节点会被反复派活，而繁忙节点继续被选中。
 *
 * 但单纯把"剩余空间绝对值"换成"剩余空间比例"并与其他维度加权求和是不够的：
 * 在容量悬殊的异构集群里（例如 1TB SSD 对 8TB HDD），只要空间权重不是绝对主导，
 * 快盘就会被持续榨取、先行写满，反而缩短了扩容窗口——这是第一版实现暴露出来的问题。
 *
 * 因此最终采用**两段式决策**（容量水位过滤 + 加权评分），这也是 HDFS
 * AvailableSpaceVolumeChoosingPolicy 的思路：
 *   第一段 硬性过滤：写满的节点、使用率超过高水位的节点直接出局；
 *   第二段 水位过滤：计算存活节点的剩余比例，低于 (最优水位 - space_delta) 的节点出局，
 *                    这一步保证容量均衡不会被性能因素破坏；
 *   第三段 加权评分：在剩余候选里按 score = w_space*剩余比例 + w_disk*磁盘性能
 *                    + w_load*空闲度 取最高分。
 * 效果是：各节点水位接近时，由性能与负载主导，快盘承担更多 IO；
 *        一旦某个节点水位明显落后，容量维度立即接管，强制分流。
 */
class LbAlgorithm {
public:
    struct Config {
        LbWeights weights;
        double space_delta;      // 水位过滤带宽：剩余比例低于(最优水位-该值)的节点出局（默认 0.15）
        double load_high_water;  // 高水位：使用率高于该值则直接淘汰（默认 0.90）

        Config() : space_delta(0.15), load_high_water(0.90) {}
    };

    LbAlgorithm();
    ~LbAlgorithm();

    static LbAlgorithm* getInstance();
    static void destroyInstance();

    void setConfig(const Config& cfg) { cfg_ = cfg; }
    const Config& config() const { return cfg_; }

    // ---------- 策略实现 ----------
    // 原生策略：取剩余空间绝对值最大的节点（= FastDFS store_lookup=2）
    int selectMaxFreeSpace(const std::vector<StorageNode>& nodes) const;
    // 改进策略：加权评分取最高分
    int selectWeighted(const std::vector<StorageNode>& nodes) const;
    // 统一入口，strategy 非法时回退到原生策略
    int select(const std::vector<StorageNode>& nodes, LbStrategy strategy) const;

    // ---------- 评分拆解（压测与调试用）----------
    double score(const StorageNode& n) const;
    double spaceScore(const StorageNode& n) const;  // free / total
    double diskScore(const StorageNode& n) const;   // disk_perf
    double idleScore(const StorageNode& n) const;   // 1 - active / capacity

    // ---------- 任务使用率统计 ----------
    // FastCGI 进程常驻，因此进程内计数即可近似反映"本机发往该组的在途任务数"。
    // key 建议用 "group/ip"，不使用 std::mutex 是为了不在热路径上引入锁开销，
    // 单进程内的自增自减在 x86 上足够，真实生产环境应换成共享内存 + 原子量。
    void onTaskBegin(const std::string& key);
    void onTaskEnd(const std::string& key);
    int  activeTasks(const std::string& key) const;

private:
    Config cfg_;
    std::map<std::string, int> inflight_;  // key -> 在途任务数

    static LbAlgorithm* instance_;
};

#endif // LB_ALGORITHM_H
