/**
 * 负载均衡策略对比基准测试
 *
 * 目的：量化"原生最大剩余空间策略"与"改进加权评分策略"在真实写入压力下的差异。
 *
 * 方法：离散事件模拟（discrete event simulation）。
 *   - 维护 CONCURRENCY 个并发写任务，每个任务有确定的字节数和剩余处理时间；
 *   - 节点处理时间 = 文件大小 / (基准带宽 × 磁盘性能系数)，因此机械盘的慢会真实
 *     体现在队列积压上，而不是靠人为编造的权重；
 *   - 两种策略消费**完全相同**的文件大小序列（固定随机种子），保证对比公平；
 *   - 每完成一个任务就立刻派发下一个，直到完成 TOTAL_TASKS 次上传。
 *
 * 决策粒度严格对齐 FastDFS 的三级瀑布：
 *   第一级在组之间选择（原生 store_lookup=2 按剩余空间绝对值，改进按两段式决策）；
 *   第二级在组内节点之间选择（原生 store_server=0 轮询，改进同样按两段式决策）。
 *
 * 本文件只依赖 lb_algorithm.h，不链接 FastDFS，可在任意平台编译运行：
 *   g++ -O2 -std=c++11 lb_benchmark.cpp lb_algorithm.cpp -o lb_benchmark
 */

#include "lb_algorithm.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

// ============================ 模拟参数 ============================
static const int    CONCURRENCY      = 64;      // 并发写客户端数
static const int    TOTAL_TASKS      = 20000;   // 总上传次数
static const double SSD_MB_PER_MS    = 0.20;    // 基准写带宽 200MB/s = 0.2MB/ms
static const double LOAD_HIGH_WATER  = 0.80;    // 统计"重载命中"的水位线
static const double LOW_SPACE_WATER  = 0.10;    // 统计"低容量命中"的水位线

// ============================ 模拟数据结构 ============================
struct SimNode {
    StorageNode node;      // 算法可见的快照
    int64_t used_mb;       // 已占用容量
    int64_t task_count;    // 累计承接任务数
    int64_t byte_count;    // 累计承接字节数
    int     peak_active;   // 在途任务峰值
    int     overload_hits; // 被选中时使用率已 >= LOAD_HIGH_WATER 的次数
    int     lowspace_hits; // 被选中时剩余比例已 < LOW_SPACE_WATER 的次数

    SimNode() : used_mb(0), task_count(0), byte_count(0),
                peak_active(0), overload_hits(0), lowspace_hits(0) {}
};

struct SimGroup {
    std::string name;
    std::vector<SimNode> nodes;
};

struct InFlight {
    int group_idx;
    int node_idx;
    double remain_ms;
};

// 固定种子的线性同余发生器：保证两种策略拿到同一串文件大小
struct Rng {
    unsigned long long s;
    explicit Rng(unsigned long long seed) : s(seed ? seed : 1ULL) {}
    unsigned int nextU32() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<unsigned int>(s >> 33);
    }
    // 返回 [0,1) 均匀分布
    double nextDouble() { return nextU32() / 4294967296.0; }
};

// ============================ 初始化拓扑 ============================
// 刻意构造"容量与性能倒挂"的拓扑：容量最大的组全是机械盘、容量最小的组是 SSD。
// 这正是原生策略的软肋——它只看剩余空间绝对值，会长期把新数据压到大机械盘组。
static void buildTopology(std::vector<SimGroup>& groups) {
    groups.clear();

    // group1：SSD 512GB × 2，性能最好但容量最小
    {
        SimGroup g;
        g.name = "group1";
        for (int i = 0; i < 2; ++i) {
            SimNode sn;
            sn.node.group_name = g.name;
            sn.node.ip_addr    = "10.0.0.1" + std::string(1, char('1' + i));
            sn.node.port       = 23000;
            sn.node.total_mb   = 524288;   // 512 GB
            sn.node.free_mb    = 524288;
            sn.node.disk_perf  = 1.00;     // SSD
            sn.node.task_capacity = 48;
            g.nodes.push_back(sn);
        }
        groups.push_back(g);
    }

    // group2：HDD 4TB × 2，容量巨大但性能一般
    {
        SimGroup g;
        g.name = "group2";
        for (int i = 0; i < 2; ++i) {
            SimNode sn;
            sn.node.group_name = g.name;
            sn.node.ip_addr    = "10.0.0.2" + std::string(1, char('1' + i));
            sn.node.port       = 23000;
            sn.node.total_mb   = 4194304;  // 4 TB
            sn.node.free_mb    = 4194304;
            sn.node.disk_perf  = 0.55;     // HDD
            sn.node.task_capacity = 48;
            g.nodes.push_back(sn);
        }
        groups.push_back(g);
    }

    // group3：HDD 2TB × 1，单点，容量与性能都不占优
    {
        SimGroup g;
        g.name = "group3";
        SimNode sn;
        sn.node.group_name = g.name;
        sn.node.ip_addr    = "10.0.0.31";
        sn.node.port       = 23000;
        sn.node.total_mb   = 2097152;  // 2 TB
        sn.node.free_mb    = 2097152;
        sn.node.disk_perf  = 0.55;
        sn.node.task_capacity = 48;
        g.nodes.push_back(sn);
        groups.push_back(g);
    }
}

// 组级快照：容量求和、性能按容量加权平均、负载求和
static StorageNode aggregateGroup(const SimGroup& g) {
    StorageNode out;
    out.group_name = g.name;
    int64_t total = 0, free_mb = 0, cap = 0;
    int active = 0;
    double perf_weighted = 0.0;

    for (size_t i = 0; i < g.nodes.size(); ++i) {
        const SimNode& sn = g.nodes[i];
        total    += sn.node.total_mb;
        free_mb  += sn.node.free_mb;
        active   += sn.node.active_tasks;
        cap      += sn.node.task_capacity;
        perf_weighted += sn.node.disk_perf * static_cast<double>(sn.node.total_mb);
    }
    out.total_mb      = total;
    out.free_mb       = free_mb;
    out.active_tasks  = active;
    out.task_capacity = static_cast<int>(cap);
    out.disk_perf     = (total > 0) ? (perf_weighted / total) : 1.0;
    return out;
}

// ============================ 一次完整模拟 ============================
struct SimResult {
    double makespan_ms;
    double throughput_mb_s;
    int64_t total_bytes;
    std::vector<std::vector<SimNode> > final_nodes;  // [group][node]
    int overload_hits;
    int lowspace_hits;
    int peak_inflight;
    double task_cv;       // 任务数变异系数
    double usage_std;     // 容量占用比例标准差
    double usage_spread;  // 容量占用比例极差
    int used_groups;      // 实际承接了任务的组数
};

static SimResult runSimulation(LbStrategy strategy, const std::vector<double>& sizes) {
    std::vector<SimGroup> groups;
    buildTopology(groups);

    LbAlgorithm lb;
    LbAlgorithm::Config cfg;
    cfg.space_delta     = 0.15;
    cfg.load_high_water = 0.90;
    lb.setConfig(cfg);

    SimResult r;
    r.makespan_ms = 0.0;
    r.total_bytes = 0;
    r.overload_hits = 0;
    r.lowspace_hits = 0;
    r.peak_inflight = 0;
    r.used_groups = 0;

    std::vector<InFlight> inflight;
    int dispatched = 0;
    int completed  = 0;
    double clock   = 0.0;

    // 组内轮询游标（模拟 FastDFS store_server=0 的轮询语义）
    std::vector<size_t> round_robin(groups.size(), 0);

    while (completed < TOTAL_TASKS) {
        // ---- 1. 刷新全部节点快照（容量与在途任务）----
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            for (size_t ni = 0; ni < groups[gi].nodes.size(); ++ni) {
                SimNode& sn = groups[gi].nodes[ni];
                sn.node.free_mb = sn.node.total_mb - sn.used_mb;
            }
        }
        // 按在途任务反推每个节点的 active_tasks
        for (size_t gi = 0; gi < groups.size(); ++gi)
            for (size_t ni = 0; ni < groups[gi].nodes.size(); ++ni)
                groups[gi].nodes[ni].node.active_tasks = 0;
        for (size_t i = 0; i < inflight.size(); ++i) {
            groups[inflight[i].group_idx].nodes[inflight[i].node_idx]
                .node.active_tasks += 1;
        }

        // ---- 2. 派发新任务，填满并发槽位 ----
        while (dispatched < TOTAL_TASKS &&
               static_cast<int>(inflight.size()) < CONCURRENCY) {

            // 2.1 第一级：组选择
            std::vector<StorageNode> group_cands;
            for (size_t gi = 0; gi < groups.size(); ++gi) {
                group_cands.push_back(aggregateGroup(groups[gi]));
            }
            int gi = lb.select(group_cands, strategy);
            if (gi < 0) break;

            // 2.2 第二级：组内节点选择
            std::vector<StorageNode> node_cands;
            for (size_t ni = 0; ni < groups[gi].nodes.size(); ++ni) {
                node_cands.push_back(groups[gi].nodes[ni].node);
            }
            int ni;
            if (strategy == LB_MAX_FREE_SPACE) {
                // 原生：组内轮询
                ni = static_cast<int>(round_robin[gi] % groups[gi].nodes.size());
                round_robin[gi] += 1;
            } else {
                ni = lb.select(node_cands, LB_WEIGHTED_SCORE);
            }
            if (ni < 0) ni = 0;

            SimNode& target = groups[gi].nodes[ni];

            // ---- 2.3 记录决策质量指标 ----
            double used_ratio = target.node.task_capacity > 0
                ? static_cast<double>(target.node.active_tasks) / target.node.task_capacity
                : 0.0;
            double free_ratio = target.node.total_mb > 0
                ? static_cast<double>(target.node.free_mb) / target.node.total_mb
                : 0.0;
            if (used_ratio >= LOAD_HIGH_WATER) r.overload_hits += 1;
            if (free_ratio < LOW_SPACE_WATER)  r.lowspace_hits += 1;

            // ---- 2.4 取本次写入大小（两种策略共用预先算好的同一序列）----
            double size_mb = sizes[dispatched];  // 1~20MB
            double speed   = SSD_MB_PER_MS * target.node.disk_perf;  // MB/ms
            double dur_ms  = size_mb / speed;

            InFlight f;
            f.group_idx = gi;
            f.node_idx  = ni;
            f.remain_ms = dur_ms;
            inflight.push_back(f);

            target.task_count += 1;
            target.byte_count += static_cast<int64_t>(size_mb);
            target.used_mb    += static_cast<int64_t>(size_mb);
            r.total_bytes     += static_cast<int64_t>(size_mb);
            if (target.node.active_tasks + 1 > target.peak_active)
                target.peak_active = target.node.active_tasks + 1;

            dispatched += 1;
        }

        if (inflight.empty()) break;

        // ---- 3. 推进时钟到最早完成的任务 ----
        size_t min_idx = 0;
        for (size_t i = 1; i < inflight.size(); ++i) {
            if (inflight[i].remain_ms < inflight[min_idx].remain_ms) min_idx = i;
        }
        double delta = inflight[min_idx].remain_ms;
        clock += delta;
        for (size_t i = 0; i < inflight.size(); ++i) inflight[i].remain_ms -= delta;

        inflight.erase(inflight.begin() + min_idx);
        completed += 1;

        if (static_cast<int>(inflight.size()) > r.peak_inflight)
            r.peak_inflight = static_cast<int>(inflight.size());
    }

    r.makespan_ms     = clock;
    r.throughput_mb_s = (clock > 0.0)
        ? (static_cast<double>(r.total_bytes) / clock * 1000.0 / 1024.0 * 1024.0 / 1024.0 * 1024.0 / 1000.0)
        : 0.0;
    // 上式化简为 MB/ms -> MB/s
    r.throughput_mb_s = (clock > 0.0) ? (static_cast<double>(r.total_bytes) / clock * 1000.0) : 0.0;

    // ---- 4. 汇总分布指标 ----
    std::vector<double> usage_ratios;
    std::vector<double> task_counts;
    int group_used = 0;
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        bool this_group_used = false;
        for (size_t ni = 0; ni < groups[gi].nodes.size(); ++ni) {
            const SimNode& sn = groups[gi].nodes[ni];
            double ratio = (sn.node.total_mb > 0)
                ? static_cast<double>(sn.used_mb) / sn.node.total_mb : 0.0;
            usage_ratios.push_back(ratio);
            task_counts.push_back(static_cast<double>(sn.task_count));
            if (sn.task_count > 0) this_group_used = true;
        }
        if (this_group_used) group_used += 1;
    }
    r.used_groups = group_used;
    r.final_nodes.clear();
    for (size_t gi = 0; gi < groups.size(); ++gi) r.final_nodes.push_back(groups[gi].nodes);

    double mean_usage = 0.0;
    for (size_t i = 0; i < usage_ratios.size(); ++i) mean_usage += usage_ratios[i];
    mean_usage /= static_cast<double>(usage_ratios.size());
    double var_usage = 0.0;
    for (size_t i = 0; i < usage_ratios.size(); ++i) {
        var_usage += (usage_ratios[i] - mean_usage) * (usage_ratios[i] - mean_usage);
    }
    r.usage_std = std::sqrt(var_usage / static_cast<double>(usage_ratios.size()));
    double max_u = 0.0, min_u = 1e18;
    for (size_t i = 0; i < usage_ratios.size(); ++i) {
        if (usage_ratios[i] > max_u) max_u = usage_ratios[i];
        if (usage_ratios[i] < min_u) min_u = usage_ratios[i];
    }
    r.usage_spread = max_u - min_u;

    double mean_t = 0.0;
    for (size_t i = 0; i < task_counts.size(); ++i) mean_t += task_counts[i];
    mean_t /= static_cast<double>(task_counts.size());
    double var_t = 0.0;
    for (size_t i = 0; i < task_counts.size(); ++i) {
        var_t += (task_counts[i] - mean_t) * (task_counts[i] - mean_t);
    }
    var_t /= static_cast<double>(task_counts.size());
    r.task_cv = (mean_t > 0.0) ? (std::sqrt(var_t) / mean_t) : 0.0;

    return r;
}

// ============================ 输出 ============================
static void printNodeTable(const char* title, const SimResult& r,
                           const std::vector<SimGroup>& ref) {
    printf("\n%s\n", title);
    printf("%-9s %-13s %-10s %14s %14s %12s\n",
           "组", "节点IP", "介质", "承接任务数", "承接数据(MB)", "容量占用");
    printf("--------------------------------------------------------------------------\n");
    for (size_t gi = 0; gi < r.final_nodes.size(); ++gi) {
        for (size_t ni = 0; ni < r.final_nodes[gi].size(); ++ni) {
            const SimNode& sn = r.final_nodes[gi][ni];
            const char* media = (sn.node.disk_perf > 0.9) ? "SSD" : "HDD";
            double ratio = (sn.node.total_mb > 0)
                ? static_cast<double>(sn.used_mb) / sn.node.total_mb : 0.0;
            printf("%-9s %-13s %-10s %14lld %14lld %11.2f%%\n",
                   ref[gi].name.c_str(), sn.node.ip_addr.c_str(), media,
                   (long long)sn.task_count, (long long)sn.byte_count, ratio * 100.0);
        }
    }
}

int main(int argc, char** argv) {
    unsigned long long seed = (argc > 1) ? strtoull(argv[1], NULL, 10) : 20260609ULL;
    bool csv = false;
    bool pressure = false;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--csv") == 0) csv = true;
        if (strcmp(argv[i], "--pressure") == 0) pressure = true;
    }

    // 常规场景：数据量远小于总容量，考察吞吐与快盘利用率；
    // 高水位场景：单文件放大到 50~250MB，总量约 2.9TB，足以把 1TB 的 SSD 组推向写满，
    //             用于验证"空间权重是否真的能在容量收紧时发挥作用"。
    double size_lo = pressure ? 50.0  : 1.0;
    double size_hi = pressure ? 250.0 : 20.0;

    printf("============================================================\n");
    printf(" FastDFS 选盘策略对比基准测试\n");
    printf("============================================================\n");
    printf(" 场景            : %s\n", pressure ? "高水位（容量压力）" : "常规负载");
    printf(" 并发客户端      : %d\n", CONCURRENCY);
    printf(" 总上传次数      : %d\n", TOTAL_TASKS);
    printf(" SSD 基准带宽    : %.0f MB/s\n", SSD_MB_PER_MS * 1000.0);
    printf(" 文件大小分布    : %.0f ~ %.0f MB 均匀随机\n", size_lo, size_hi);
    printf(" 随机种子        : %llu  (两种策略使用同一序列)\n", seed);

    std::vector<SimGroup> topo;
    buildTopology(topo);

    // 预先固定文件大小序列，让两种策略面对完全一致的工作负载
    Rng size_rng(seed);
    std::vector<double> sizes(TOTAL_TASKS);
    for (int i = 0; i < TOTAL_TASKS; ++i) {
        sizes[i] = size_lo + size_rng.nextDouble() * (size_hi - size_lo);
    }

    SimResult base = runSimulation(LB_MAX_FREE_SPACE, sizes);
    SimResult opt  = runSimulation(LB_WEIGHTED_SCORE, sizes);

    printNodeTable("【策略 A】原生：组级最大剩余空间 + 组内轮询", base, topo);
    printNodeTable("【策略 B】改进：组级加权评分 + 组内加权评分", opt, topo);

    printf("\n============================================================\n");
    printf(" 汇总对比\n");
    printf("============================================================\n");
    printf("%-26s %16s %16s %12s\n", "指标", "策略A(原生)", "策略B(改进)", "变化");
    printf("--------------------------------------------------------------------------\n");

    printf("%-26s %16.1f %16.1f %11.1f%%\n", "总完成时间(ms)",
           base.makespan_ms, opt.makespan_ms,
           (base.makespan_ms - opt.makespan_ms) / base.makespan_ms * 100.0);
    printf("%-26s %16.2f %16.2f %11.1f%%\n", "吞吐(MB/s)",
           base.throughput_mb_s, opt.throughput_mb_s,
           (opt.throughput_mb_s - base.throughput_mb_s) / base.throughput_mb_s * 100.0);
    printf("%-26s %16.4f %16.4f %11.1f%%\n", "任务数变异系数(越小越均衡)",
           base.task_cv, opt.task_cv,
           (base.task_cv - opt.task_cv) / base.task_cv * 100.0);
    printf("%-26s %16.4f %16.4f %11.1f%%\n", "容量占用标准差(越小越均衡)",
           base.usage_std, opt.usage_std,
           (base.usage_std - opt.usage_std) / base.usage_std * 100.0);
    printf("%-26s %16.2f%% %15.2f%% %11.1f%%\n", "容量占用极差",
           base.usage_spread * 100.0, opt.usage_spread * 100.0,
           (base.usage_spread - opt.usage_spread) / base.usage_spread * 100.0);
    printf("%-26s %16d %16d %11.1f%%\n", "选中节点的重载次数(>=80%)",
           base.overload_hits, opt.overload_hits,
           base.overload_hits > 0
             ? (double)(base.overload_hits - opt.overload_hits) / base.overload_hits * 100.0 : 0.0);
    printf("%-26s %16d %16d\n", "选中节点的低容量次数(<10%)",
           base.lowspace_hits, opt.lowspace_hits);
    printf("%-26s %16d %16d\n", "在途任务峰值",
           base.peak_inflight, opt.peak_inflight);
    printf("%-26s %16d %16d\n", "实际承接任务的组数",
           base.used_groups, opt.used_groups);

    if (csv) {
        printf("\n----CSV----\n");
        printf("strategy,group,node_ip,media,task_count,bytes_mb,usage_ratio\n");
        const char* names[2] = {"max_free_space", "weighted_score"};
        const SimResult* rs[2] = {&base, &opt};
        for (int k = 0; k < 2; ++k) {
            for (size_t gi = 0; gi < rs[k]->final_nodes.size(); ++gi) {
                for (size_t ni = 0; ni < rs[k]->final_nodes[gi].size(); ++ni) {
                    const SimNode& sn = rs[k]->final_nodes[gi][ni];
                    double ratio = (sn.node.total_mb > 0)
                        ? (double)sn.used_mb / sn.node.total_mb : 0.0;
                    printf("%s,%s,%s,%s,%lld,%lld,%.6f\n",
                           names[k], topo[gi].name.c_str(), sn.node.ip_addr.c_str(),
                           (sn.node.disk_perf > 0.9 ? "SSD" : "HDD"),
                           (long long)sn.task_count, (long long)sn.byte_count, ratio);
                }
            }
        }
    }
    return 0;
}
