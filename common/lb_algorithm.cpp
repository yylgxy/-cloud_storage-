#include "lb_algorithm.h"
#include <cfloat>

LbAlgorithm* LbAlgorithm::instance_ = nullptr;

LbAlgorithm::LbAlgorithm() {}
LbAlgorithm::~LbAlgorithm() {}

LbAlgorithm* LbAlgorithm::getInstance() {
    if (instance_ == nullptr) {
        instance_ = new LbAlgorithm();
    }
    return instance_;
}

void LbAlgorithm::destroyInstance() {
    delete instance_;
    instance_ = nullptr;
}

// ============================ 评分拆解 ============================

// 剩余空间比例：把绝对值归一化到 [0,1]，消除"大盘永远赢"的容量偏向
double LbAlgorithm::spaceScore(const StorageNode& n) const {
    if (n.total_mb <= 0) return 0.0;
    double ratio = static_cast<double>(n.free_mb) / static_cast<double>(n.total_mb);
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    return ratio;
}

// 磁盘性能系数：由节点画像配置注入（SSD 1.0 / HDD 0.55）
double LbAlgorithm::diskScore(const StorageNode& n) const {
    if (n.disk_perf < 0.0) return 0.0;
    if (n.disk_perf > 1.0) return 1.0;
    return n.disk_perf;
}

// 空闲度：1 - 任务使用率
double LbAlgorithm::idleScore(const StorageNode& n) const {
    if (n.task_capacity <= 0) return 0.0;
    double used = static_cast<double>(n.active_tasks) / static_cast<double>(n.task_capacity);
    if (used < 0.0) used = 0.0;
    if (used > 1.0) used = 1.0;
    return 1.0 - used;
}

double LbAlgorithm::score(const StorageNode& n) const {
    // 三个维度都已归一化到 [0,1]，加权后总分同样落在 [0,1]，
    // 因此权重可以直接按业务偏好调整，不需要再考虑量纲对齐。
    return cfg_.weights.space * spaceScore(n)
         + cfg_.weights.disk  * diskScore(n)
         + cfg_.weights.load  * idleScore(n);
}

// ============================ 策略实现 ============================

int LbAlgorithm::selectMaxFreeSpace(const std::vector<StorageNode>& nodes) const {
    int best = -1;
    int64_t best_free = -1;
    for (size_t i = 0; i < nodes.size(); ++i) {
        // 原生语义完全忽略性能与负载，只看剩余空间绝对值
        if (nodes[i].free_mb > best_free) {
            best_free = nodes[i].free_mb;
            best = static_cast<int>(i);
        }
    }
    return best;
}

int LbAlgorithm::selectWeighted(const std::vector<StorageNode>& nodes) const {
    // ---------- 第一段：硬性过滤 ----------
    // 写满的节点没有落点，满负荷的节点再派活只会加剧排队，两类都直接出局。
    std::vector<int> alive;
    alive.reserve(nodes.size());
    double max_ratio = -1.0;

    for (size_t i = 0; i < nodes.size(); ++i) {
        const StorageNode& n = nodes[i];

        if (n.free_mb <= 0) continue;                       // 磁盘已写满
        if (n.task_capacity > 0 &&
            static_cast<double>(n.active_tasks) / n.task_capacity >= cfg_.load_high_water) {
            continue;                                        // 已到高水位
        }
        alive.push_back(static_cast<int>(i));
        double r = spaceScore(n);
        if (r > max_ratio) max_ratio = r;
    }

    // 全部节点都不可写时退化为原生策略，保证请求一定有落点
    if (alive.empty()) {
        return selectMaxFreeSpace(nodes);
    }

    // ---------- 第二段：容量水位过滤 ----------
    // 只保留水位与当前最优水位相差不超过 space_delta 的节点。
    // 这一步是"容量均衡"的守门人：即使某个快盘性能评分再高，只要它已经明显比
    // 同伴更满，也会被挡在候选之外，从而避免小容量盘先行写满、缩短扩容窗口。
    std::vector<int> cand;
    cand.reserve(alive.size());
    double floor_ratio = max_ratio - cfg_.space_delta;

    for (size_t k = 0; k < alive.size(); ++k) {
        int idx = alive[k];
        if (spaceScore(nodes[idx]) >= floor_ratio) {
            cand.push_back(idx);
        }
    }
    if (cand.empty()) cand = alive;  // 理论上不会发生，防御性兜底

    // ---------- 第三段：加权评分 ----------
    int best = cand[0];
    double best_score = score(nodes[best]);
    for (size_t k = 1; k < cand.size(); ++k) {
        double s = score(nodes[cand[k]]);
        if (s > best_score) {
            best_score = s;
            best = cand[k];
        }
    }
    return best;
}

int LbAlgorithm::select(const std::vector<StorageNode>& nodes, LbStrategy strategy) const {
    if (nodes.empty()) return -1;

    switch (strategy) {
        case LB_MAX_FREE_SPACE:
            return selectMaxFreeSpace(nodes);
        case LB_WEIGHTED_SCORE:
            return selectWeighted(nodes);
        default:
            return selectMaxFreeSpace(nodes);
    }
}

// ============================ 在途任务统计 ============================

void LbAlgorithm::onTaskBegin(const std::string& key) {
    inflight_[key] += 1;
}

void LbAlgorithm::onTaskEnd(const std::string& key) {
    std::map<std::string, int>::iterator it = inflight_.find(key);
    if (it == inflight_.end()) return;
    it->second -= 1;
    if (it->second <= 0) {
        inflight_.erase(it);  // 归零即回收，避免 map 无界增长
    }
}

int LbAlgorithm::activeTasks(const std::string& key) const {
    std::map<std::string, int>::const_iterator it = inflight_.find(key);
    if (it == inflight_.end()) return 0;
    return it->second;
}
