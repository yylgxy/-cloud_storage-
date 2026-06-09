# ☁️ Cloud Storage

> 基于 C++ FastCGI + FastDFS 的分布式云存储系统

一个轻量级、高性能的云存储后端服务，支持文件上传、下载、分享和管理。前端提供简洁的 Web 界面，后端通过 FastCGI 与 Nginx 通信，使用 FastDFS 作为分布式文件存储引擎。

---

## 📋 功能特性

| 功能 | 说明 |
|------|------|
| ✅ **用户系统** | 注册、登录（MD5 加密）、Token 鉴权 |
| 📤 **文件上传** | 单文件上传，支持断点续传（分片上传） |
| 📥 **文件下载** | 通过 FastDFS 直连下载 |
| 🔗 **文件分享** | 生成分享链接，支持提取码和过期时间 |
| 📂 **文件管理** | 文件列表浏览、删除、公开/取消分享 |
| 🏆 **热门排行** | 按访问量展示热门分享文件 |
| ☁️ **外网穿透** | 集成 Cloudflare Tunnel，无需公网 IP |

---

## 🏗 系统架构

```
用户浏览器
     │
     ▼
   Nginx（反向代理 + 静态文件）
     │
     ├── /api/* ──────► FastCGI (C++)
     │                        │
     │                        ├── MySQL（用户数据）
     │                        ├── Redis（Token/缓存）
     │                        └── FastDFS（文件存储）
     │
     └── /group*/M00 ──► FastDFS（文件下载）

```

---

## 🛠 技术栈

| 层面 | 技术 |
|------|------|
| **后端语言** | C++11 |
| **Web 服务器** | Nginx + FastCGI |
| **文件存储** | FastDFS（分布式文件系统） |
| **数据库** | MySQL（用户信息）、Redis（Token 会话） |
| **前端** | 原生 HTML + CSS + JavaScript |
| **JSON 解析** | cJSON |
| **外网访问** | Cloudflare Tunnel |

---

## 🔐 环境变量

启动前必须设置数据库密码：

```bash
export DB_PASSWORD="你的数据库密码"
```

也可复制 `.env.example` 参考配置。

---

## 🚀 快速启动

```bash
# 1. 启动 FastDFS
sudo fdfs_trackerd /etc/fdfs/tracker.conf
sudo fdfs_storaged /etc/fdfs/storage.conf

# 2. 启动 Nginx
sudo /usr/local/nginx/sbin/nginx

# 3. 启动 FastCGI 服务（分别监听不同端口）
spawn-fcgi -a 127.0.0.1 -p 8005 -f ./common/ApiUpload
spawn-fcgi -a 127.0.0.1 -p 8006 -f ./common/ApiRegister
spawn-fcgi -a 127.0.0.1 -p 8007 -f ./common/ApiLogin
spawn-fcgi -a 127.0.0.1 -p 8008 -f ./common/ApiMd5
spawn-fcgi -a 127.0.0.1 -p 8009 -f ./common/ApiSharefiles
# ...（详见 start_cloud.sh）
```

也可直接运行一键启动脚本：

```bash
bash common/start_cloud.sh
```

---

## 📁 项目结构

```
cloud_storage/
├── .gitignore                 # Git 忽略规则
├── README.md                  # 本文件
├── common/
│   ├── ApiLogin.cpp           # 登录接口
│   ├── ApiRegister.cpp        # 注册接口
│   ├── ApiUpload.cpp          # 文件上传接口
│   ├── ApiSliceUpload.cpp     # 断点续传接口
│   ├── ApiMd5.cpp             # 文件 MD5 校验接口
│   ├── ApiMyfiles.cpp         # 文件列表管理接口
│   ├── ApiDealfile.cpp        # 文件操作（删除/分享）接口
│   ├── ApiSharefiles.cpp      # 分享文件列表接口
│   ├── ApiDealsharefile.cpp   # 分享操作（取消/保存）接口
│   ├── ApiShareAccess.cpp     # 分享访问接口
│   ├── cjson_utils.cpp/.h     # JSON 工具封装
│   ├── mysql_utils.cpp/.h     # MySQL 数据库操作封装
│   ├── redis_utils.cpp/.h     # Redis 缓存操作封装
│   ├── fastdfs_utils.cpp/.h   # FastDFS 文件操作封装
│   ├── md5_utils.cpp/.h       # MD5 工具函数
│   ├── test_*.cpp             # 单元测试
│   ├── *.html                 # 前端页面
│   ├── *.conf.example         # Nginx/FastCGI 配置模板
│   ├── mime.types             # MIME 类型表
│   └── start_cloud.sh         # 一键启动脚本
```

---

## 🔐 接口说明

所有 API 接口通过 HTTP GET/POST 请求，返回 JSON 格式数据：

| 接口路径 | 方法 | 说明 |
|----------|------|------|
| `/api/register` | POST | 用户注册 |
| `/api/login` | POST | 用户登录，返回 Token |
| `/api/upload` | POST | 文件上传 |
| `/api/sliceupload` | POST | 分片上传（断点续传） |
| `/api/md5` | GET/POST | 文件秒传校验 |
| `/api/myfiles` | POST | 文件列表/计数 |
| `/api/dealfile` | GET | 文件删除/分享/取消分享 |
| `/api/sharefiles` | GET/POST | 分享文件列表/排行 |
| `/api/dealsharefile` | GET | 取消分享/保存到我的文件 |
| `/api/share` | GET | 通过分享链接访问文件 |

---

## 📄 开源协议

MIT License

---

**如果这个项目对你有帮助，欢迎 Star ⭐**
