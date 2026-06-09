# Cloud Storage

基于 FastDFS + FastCGI 的云存储项目。

## 功能

- 用户注册/登录
- 文件上传（支持断点续传）
- 文件分享
- 文件管理（浏览/下载/删除）

## 技术栈

- **后端**: C++ (FastCGI)
- **存储**: FastDFS（分布式文件系统）
- **数据库**: MySQL + Redis
- **Web 服务器**: Nginx
- **JSON 处理**: cJSON
