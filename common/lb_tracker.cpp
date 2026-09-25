#include "lb_tracker.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

using namespace std;

// 一次最多统计多少个组。FastDFS 单集群的组数远小于该值，
// 这里刻意用自定义常量而不是上游宏，避免不同版本的宏名差异导致编译失败。
static const int MAX_GROUPS = 64;

LbTracker* LbTracker::instance_ = nullptr;

LbTracker::LbTracker()
    : default_disk_perf_(1.0),    // 未配置时按同构集群处理，不影响空间与负载维度
      default_capacity_(64) {}

LbTracker::~LbTracker() {}

LbTracker* LbTracker::getInstance() {
    if (instance_ == nullptr) {
        instance_ = new LbTracker();
    }
    return instance_;
}

void LbTracker::destroyInstance() {
    delete instance_;
    instance_ = nullptr;
}

double LbTracker::diskPerfOf(const string& key) const {
    map<string, double>::const_iterator it = perf_map_.find(key);
    if (it == perf_map_.end()) return default_disk_perf_;
    return it->second;
}

int LbTracker::capacityOf(const string& key) const {
    map<string, int>::const_iterator it = cap_map_.find(key);
    if (it == cap_map_.end()) return default_capacity_;
    return it->second;
}

// 配置文件格式：  <key> = <perf>:<capacity>
// key 可以是组名（参与组级评分）或节点 IP（参与节点级评分）
bool LbTracker::loadNodeProfile(const char* conf_path) {
    if (conf_path == nullptr) return false;

    ifstream in(conf_path);
    if (!in.is_open()) {
        return false;  // 配置缺失不是错误，调用方继续用默认画像
    }

    string line;
    int loaded = 0;
    while (getline(in, line)) {
        // 去掉注释
        size_t hash = line.find('#');
        if (hash != string::npos) line = line.substr(0, hash);
        if (line.empty()) continue;

        size_t eq = line.find('=');
        if (eq == string::npos) continue;

        string key = line.substr(0, eq);
        string val = line.substr(eq + 1);

        // trim
        while (!key.empty() && isspace((unsigned char)key[0])) key.erase(0, 1);
        while (!key.empty() && isspace((unsigned char)key[key.size() - 1])) key.erase(key.size() - 1);
        while (!val.empty() && isspace((unsigned char)val[0])) val.erase(0, 1);
        while (!val.empty() && isspace((unsigned char)val[val.size() - 1])) val.erase(val.size() - 1);
        if (key.empty() || val.empty()) continue;

        size_t colon = val.find(':');
        string perf_str = (colon == string::npos) ? val : val.substr(0, colon);
        string cap_str  = (colon == string::npos) ? "" : val.substr(colon + 1);

        perf_map_[key] = atof(perf_str.c_str());
        if (!cap_str.empty()) {
            cap_map_[key] = atoi(cap_str.c_str());
        }
        ++loaded;
    }

    return loaded > 0;
}

int LbTracker::selectStorage(ConnectionInfo* tracker,
                             ConnectionInfo* outStorage,
                             char* group_name,
                             int* store_path,
                             LbStrategy strategy) {
    if (tracker == nullptr || outStorage == nullptr ||
        group_name == nullptr || store_path == nullptr) {
        return -1;
    }

    LbAlgorithm* lb = LbAlgorithm::getInstance();

    // ---------- 原生策略：完全走 FastDFS 原有链路，不做任何干预 ----------
    if (strategy == LB_MAX_FREE_SPACE) {
        memset(outStorage, 0, sizeof(ConnectionInfo));
        int ret = tracker_query_storage_store_without_group(
            tracker, outStorage, group_name, store_path);
        return (ret == 0) ? 1 : -1;
    }

    // ---------- 改进策略：第一级（组）由客户端接管 ----------
    FDFSGroupStat group_stats[MAX_GROUPS];
    memset(group_stats, 0, sizeof(group_stats));
    int group_count = 0;

    if (tracker_list_groups(tracker, group_stats, MAX_GROUPS, &group_count) != 0 ||
        group_count <= 0) {
        // 拉不到组信息（tracker 版本过旧或网络异常）→ 安全回退
        memset(outStorage, 0, sizeof(ConnectionInfo));
        int ret = tracker_query_storage_store_without_group(
            tracker, outStorage, group_name, store_path);
        return (ret == 0) ? 1 : -1;
    }

    vector<StorageNode> group_cands;
    group_cands.reserve(group_count);

    for (int i = 0; i < group_count; ++i) {
        // 只保留可写节点数 > 0 的组
        if (group_stats[i].writable_server_count <= 0) continue;

        StorageNode n;
        n.group_name    = group_stats[i].group_name;
        n.total_mb      = group_stats[i].total_mb;
        n.free_mb       = group_stats[i].free_mb;
        n.disk_perf     = diskPerfOf(n.group_name);
        n.task_capacity = capacityOf(n.group_name);
        n.active_tasks  = lb->activeTasks(n.group_name);
        // 组容量按节点数与单节点容量近似，用于把组级在途任务换算成使用率
        if (n.task_capacity <= 0) n.task_capacity = default_capacity_;
        group_cands.push_back(n);
    }

    if (group_cands.empty()) {
        memset(outStorage, 0, sizeof(ConnectionInfo));
        int ret = tracker_query_storage_store_without_group(
            tracker, outStorage, group_name, store_path);
        return (ret == 0) ? 1 : -1;
    }

    int pick = lb->select(group_cands, LB_WEIGHTED_SCORE);
    if (pick < 0) pick = 0;

    // 第二级（组内节点）与第三级（路径）交回 tracker 原生逻辑处理：
    // 组内节点同构，原生轮询已足够均衡，也避免了与 tracker 的 store_path 选择冲突。
    char chosen_group[FDFS_GROUP_NAME_MAX_LEN + 1];
    memset(chosen_group, 0, sizeof(chosen_group));
    strncpy(chosen_group, group_cands[pick].group_name.c_str(), FDFS_GROUP_NAME_MAX_LEN);

    memset(outStorage, 0, sizeof(ConnectionInfo));
    int ret = tracker_query_storage_store_with_group(
        tracker, chosen_group, outStorage, store_path);

    if (ret != 0) {
        // 指定组失败时再回退一次，保证请求不会因为选组逻辑而丢失
        memset(outStorage, 0, sizeof(ConnectionInfo));
        int ret2 = tracker_query_storage_store_without_group(
            tracker, outStorage, group_name, store_path);
        return (ret2 == 0) ? 1 : -1;
    }

    strncpy(group_name, chosen_group, FDFS_GROUP_NAME_MAX_LEN);
    return 0;
}
