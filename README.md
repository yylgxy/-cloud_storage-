# ☁️ Cloud Storage

> 基于 C++ FastCGI + FastDFS 的分布式云存储系统

一个轻量级云存储后端服务，支持文件上传（含秒传与分片断点续传）、下载、分享与管理。
后端通过 FastCGI 与 Nginx 通信，使用 FastDFS 作为分布式文件存储引擎，
并对 FastDFS 的选盘策略做了负载均衡改造。

---

## 📋 功能特性

| 功能 | 说明 |
|------|------|
| ✅ **用户系统** | 注册、登录（密码 MD5 哈希）、Token 鉴权 |
| ⚡ **文件秒传** | 以文件 MD5 为指纹，命中已有文件时零字节上传 |
| 📤 **文件上传** | 单文件上传；大于 95MB 自动切分片，支持断点续传 |
| 📥 **文件下载** | 通过 FastDFS 直连下载，`sendfile` 零拷贝 |
| 🔗 **文件分享** | 生成分享链接，支持提取码与有效期 |
| 🏆 **热度排行榜** | Redis ZSET 维护下载热度排行，`ZREVRANGE` 取 TopN |
| ⚖️ **存储负载均衡** | 改造 FastDFS 选盘：容量水位过滤 + 磁盘性能 + 任务使用率加权评分 |
| ☁️ **外网穿透** | 集成 Cloudflare Tunnel，无需公网 IP |

---

## 🏗 系统架构

```
用户浏览器
     │
     ▼
   Nginx（静态资源 + FastCGI 网关 + FastDFS 下载模块）
     │
     ├── /api/* ─────────► FastCGI (C++)  10 个独立进程，各监听一个端口
     │                          │
     │                          ├── MySQL   用户信息、文件元数据、分享记录
     │                          ├── Redis   Token 会话、MD5 指纹、分片进度、热度排行榜
     │                          └── FastDFS 物理文件存储（组级选盘由本地算法接管）
     │
     └── /group*/M00 ────► ngx_fastdfs_module（下载不经过业务进程）
```

**动静分离**：`/api/*` 动态请求转发给 FastCGI 进程；`/group*/M00` 的文件下载由 Nginx 的
`ngx_fastdfs_module` 直接处理，配合 `sendfile` 走内核零拷贝，业务进程完全不参与下载链路。

---

## ⚖️ 存储负载均衡（本项目的主要改造点）

### 背景：FastDFS 原生策略的两个局限

FastDFS 的选盘是一条三级瀑布：

| 级别 | 配置项 | 默认行为 |
|------|--------|----------|
| 选组 | `store_lookup` | `2` = 剩余空间最大的组 |
| 选节点 | `store_server` | `0` = 轮询 |
| 选路径 | `store_path` | `0` = 轮询 |

默认策略只看**剩余空间的绝对值**，存在两个问题：

1. **忽略磁盘介质差异**——容量大的机械盘永远压过 SSD，快盘被闲置；
2. **忽略节点当前负载**——已在高水位排队的节点仍会被反复选中。

### 实现方案

代码位置：`common/lb_algorithm.h/.cpp`（决策算法，零外部依赖）、`common/lb_tracker.h/.cpp`（对接 FastDFS 采集与选组）。

决策采用**两段式**（思路来自 HDFS `AvailableSpaceVolumeChoosingPolicy`）：

```
第一段 硬性过滤   写满的节点、使用率 ≥ 高水位(90%)的节点直接出局
第二段 容量水位过滤   剩余比例低于 (最优水位 − space_delta) 的节点出局
第三段 加权评分   score = w_space × (free/total) + w_disk × disk_perf
                        + w_load × (1 − active_tasks / capacity)
```

**为什么必须要有第二段**：第一版实现只做了加权求和（`space=0.5, disk=0.3, load=0.2`），
基准测试显示它在容量悬殊的异构集群里会让 SSD 组涨到 56% 占用、HDD 组仍只有 12%——
**小容量盘先行写满，扩容窗口反而被缩短了**。这不是改进，只是换了个偏向。
引入水位过滤后，容量均衡由过滤阶段强制保证，加权评分只负责在"水位相近的候选"里
挑性能和负载更优的那个，两个目标不再互相干扰。

**只接管组级决策的原因**：容量差异与介质差异都发生在组这一级（一个组通常对应一批
同构机器），组内节点同构，原生轮询已足够均衡。因此客户端只接管第一级，
再调用 `tracker_query_storage_store_with_group()` 让 tracker 在组内正常选节点。
这样**不需要修改 FastDFS 源码**，升级上游时不会产生冲突，也可以灰度：
`LB_ENABLE=0` 即刻回退原生逻辑。

### 实测数据

基准测试：`common/lb_benchmark.cpp`，离散事件模拟 20000 次上传、64 并发，
两种策略消费完全一致的文件大小序列。拓扑刻意构造"容量与性能倒挂"：

| 组 | 介质 | 单节点容量 | 性能系数 |
|----|------|-----------|---------|
| group1 | SSD ×2 | 512 GB | 1.00 |
| group2 | HDD ×2 | 4 TB | 0.55 |
| group3 | HDD ×1 | 2 TB | 0.55 |

**场景一：常规负载**（文件 1~20MB，总量约 200GB，远小于总容量）

| 指标 | 原生策略 | 改进策略 | 变化 |
|------|---------|---------|------|
| 总完成时间 | 16338 ms | 9112 ms | **−44.2%** |
| 吞吐 | 6418.91 MB/s | 11508.81 MB/s | **+79.3%** |
| 选中节点的重载次数(≥80%) | 5 | 0 | −100% |
| 实际承接任务的组数 | 1 | 3 | — |

容量充裕时算法把数据导向快盘，吞吐提升 79%——此时容量不是瓶颈，
水位差偏大是**性能优先的自觉取舍**。

**场景二：高水位容量压力**（文件 50~250MB，总量约 2.9TB，足以把 1TB 的 SSD 组推向写满）

| 指标 | 原生策略 | 改进策略 | 变化 |
|------|---------|---------|------|
| 总完成时间 | 284318 ms | 264880 ms | −6.8% |
| 吞吐 | 6985.06 MB/s | 7497.64 MB/s | +7.3% |
| 任务数变异系数 | 1.2247 | 0.5073 | **−58.6%** |
| 容量占用标准差 | 0.1160 | 0.0686 | **−40.9%** |
| 容量占用极差 | 23.76% | **15.01%** | −36.8% |
| 实际承接任务的组数 | 1 | 3 | — |

水位极差收敛到 **15.01%**，与设计的 `space_delta = 0.15` 精确吻合——
这正是第二段水位过滤在起作用的直接证据。

复现方式：

```bash
cd common
g++ -O2 -std=c++11 lb_benchmark.cpp lb_algorithm.cpp -o lb_benchmark
./lb_benchmark                     # 常规负载
./lb_benchmark 20260609 --pressure # 高水位容量压力
```

原始输出留存在 `docs/raw_benchmark_normal.txt` 与 `docs/raw_benchmark_pressure.txt`。

---

## 🗄 Redis 数据设计

| 键 | 类型 | 用途 | TTL |
|----|------|------|-----|
| `<token>` | String | 会话，值为 user_id | 登录时设定 |
| `md5:<file_md5>` | String | 秒传指纹缓存，值为 FastDFS file_id | 3600s |
| `lock:md5:<file_md5>` | String | 秒传并发控制锁（`SETNX` + `EXPIRE`） | 5s |
| `upload:<file_md5>:slices` | Set | 分片上传进度，`SADD` 记录已到达的分片序号 | — |
| `rank:share:hot` | **ZSet** | 分享文件下载热度排行榜，member=share_id，score=pv | 持久 |

**排行榜为什么用 ZSet**：`ZREVRANGE key 0 N-1 WITHSCORES` 取 TopN 的复杂度是
`O(log N + M)`，不需要像 `ORDER BY pv DESC` 那样全表扫描再排序。
写入端用 `ZINCRBY` 原子自增，多个用户同时下载同一分享时不会产生行锁竞争。
元数据仍以 MySQL 为准，排行榜查询时用 `ZREVRANGE` 拿到 share_id 列表后
**一次 `IN` 查询补齐详情**（而非循环逐个查，避免 N+1）。

一致性处理：
- 分享创建 → `ZADD` 入榜（score=0），保证新分享立即可见；
- 取消分享 → `ZREM` 摘除，避免已取消的分享滞留在榜上；
- `ZINCRBY` 返回分数为 1 说明该 member 是本次新建（Redis 曾丢数据）→ 用 MySQL 真实 pv 校准；
- 排行榜查询发现 `ZCARD = 0` → 从 MySQL 一次性回填，避免 Redis 重启后排行榜凭空清空。

---

## 🔐 环境变量

| 变量 | 必填 | 说明 |
|------|------|------|
| `DB_PASSWORD` | ✅ | MySQL 密码，代码不硬编码任何口令 |
| `LB_ENABLE` | — | 默认开启改进的存储负载均衡；设为 `0` 可回退到 FastDFS 原生选盘 |
| `LB_NODE_PROFILE` | — | 节点画像配置路径，默认 `/etc/fdfs/lb_nodes.conf` |
| `SERVER_IP` | — | 仅用于启动脚本打印访问地址 |

参考模板见 `.env.example` 与 `common/lb_nodes.conf.example`。

---

## 🚀 快速启动

```bash
# 0. 初始化数据库（建表，只需执行一次）
mysql -u root -p < schema.sql

# 1. 编译：生成 common/ 下的 10 个 FastCGI 可执行文件
#    （依赖库路径若与默认不同，改 Makefile 顶部的 LIBS 变量）
make

# 2. 启动 FastDFS
sudo fdfs_trackerd /etc/fdfs/tracker.conf
sudo fdfs_storaged /etc/fdfs/storage.conf

# 3. 启动 Nginx
sudo /usr/local/nginx/sbin/nginx

# 4. 启动 FastCGI 服务（10 个进程各监听一个端口）
spawn-fcgi -a 127.0.0.1 -p 8005 -f ./common/ApiUpload
spawn-fcgi -a 127.0.0.1 -p 8006 -f ./common/ApiRegister
spawn-fcgi -a 127.0.0.1 -p 8007 -f ./common/ApiLogin
spawn-fcgi -a 127.0.0.1 -p 8008 -f ./common/ApiMd5
spawn-fcgi -a 127.0.0.1 -p 8009 -f ./common/ApiSharefiles
# ...（完整端口分配见 common/start_cloud.sh）
```

一键启动：

```bash
export DB_PASSWORD="你的数据库密码"
bash common/start_cloud.sh
```

端口分配与 Nginx 配置的对应关系见 `common/nginx.conf.example` 中的注释。

---

## 📁 项目结构

```
cloud_storage/
├── .env.example                 # 环境变量模板
├── Makefile                     # 一键构建：编译 common/ 下全部 FastCGI 可执行文件
├── schema.sql                   # 数据库建表脚本（5 张表）
├── README.md
├── docs/
│   ├── raw_benchmark_normal.txt    # 负载均衡基准测试原始输出（常规负载）
│   └── raw_benchmark_pressure.txt  # 负载均衡基准测试原始输出（高水位）
└── common/
    ├── ApiUpload.cpp            # 上传接口（multipart 解析）
    ├── ApiSliceUpload.cpp       # 分片上传接口（断点续传）
    ├── ApiMd5.cpp               # 秒传校验接口（多线程 FCGI，8 worker）
    ├── ApiRegister.cpp          # 注册接口
    ├── ApiLogin.cpp             # 登录接口
    ├── ApiMyfiles.cpp           # 文件列表接口
    ├── ApiDealfile.cpp          # 文件删除 / 分享 / 取消分享
    ├── ApiSharefiles.cpp        # 分享列表 / 计数 / 热度排行榜
    ├── ApiDealsharefile.cpp     # 分享转存 / 取消 / 下载量自增
    ├── ApiShareAccess.cpp       # 分享链接访问（提取码与有效期校验）
    ├── lb_algorithm.h/.cpp      # ★ 改进的负载均衡决策算法（无外部依赖）
    ├── lb_tracker.h/.cpp        # ★ 对接 FastDFS tracker 采集与选组
    ├── lb_benchmark.cpp         # ★ 负载均衡策略对比基准测试
    ├── lb_nodes.conf.example    # 节点画像配置模板
    ├── mysql_utils.cpp/.h       # MySQL 封装（口令从环境变量读取）
    ├── redis_utils.cpp/.h       # Redis 封装
    ├── fastdfs_utils.cpp/.h     # FastDFS 封装（上传链路接入改进算法）
    ├── cjson_utils.cpp/.h       # JSON 封装
    ├── md5_utils.cpp/.h         # MD5 工具
    ├── test_*.cpp               # 各组件单元测试
    ├── *.html                   # 前端页面
    ├── *.conf.example           # Nginx / FastCGI 配置模板
    └── start_cloud.sh           # 一键启动脚本
```

---

## 🔌 接口说明

| 接口路径 | 方法 | 说明 |
|----------|------|------|
| `/api/register` | POST | 用户注册 |
| `/api/login` | POST | 用户登录，返回 Token |
| `/api/md5` | GET/POST | 秒传校验，命中返回 `code=0`，未命中返回 `code=6`（需要上传） |
| `/api/upload` | POST | 单文件上传 |
| `/api/sliceupload` | POST | 分片上传（断点续传） |
| `/api/myfiles` | POST | 文件列表 / 计数 |
| `/api/dealfile` | GET | 文件删除 / 分享 / 取消分享 |
| `/api/sharefiles` | GET/POST | 分享列表（`cmd=list`）、计数（`cmd=count`）、排行榜（`cmd=rank&top=N`） |
| `/api/dealsharefile` | GET | 转存（`cmd=save`）、取消（`cmd=cancel`）、下载量自增（`cmd=pv`） |
| `/share` | GET | 通过分享链接访问文件（校验提取码与有效期）。注意：该路由**无 `/api` 前缀**，与其余接口不同 |

---

## 📄 开源协议

MIT License
