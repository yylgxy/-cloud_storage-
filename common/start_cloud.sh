#!/bin/bash
# 快速启动云存储所有服务（在重启后执行）
# 用法: bash ~/start_cloud.sh
#
# 可选环境变量：
#   DB_PASSWORD        MySQL 密码（必填）
#   CLOUD_STORAGE_BIN  FastCGI 可执行文件所在目录，默认 $HOME/cloud_storage/common
#   SERVER_IP          仅用于打印访问地址，不影响服务监听
#   CLOUDFLARE_TUNNEL_CMD  可选。外网穿透启动命令，未设置则跳过穿透步骤

BIN="${CLOUD_STORAGE_BIN:-$HOME/cloud_storage/common}"

# 数据库密码从环境变量读取（不要在脚本里写死）
if [ -z "$DB_PASSWORD" ]; then
    echo "⚠️  请先设置 DB_PASSWORD 环境变量：export DB_PASSWORD='你的密码'"
    exit 1
fi

echo "=== 启动 FastDFS ==="
sudo fdfs_trackerd /etc/fdfs/tracker.conf
sudo fdfs_storaged /etc/fdfs/storage.conf

echo "=== 启动 nginx ==="
sudo /usr/local/nginx/sbin/nginx

echo "=== 启动 FastCGI ==="
spawn-fcgi -a 127.0.0.1 -p 8005 -f $BIN/ApiUpload
spawn-fcgi -a 127.0.0.1 -p 8006 -f $BIN/ApiRegister
spawn-fcgi -a 127.0.0.1 -p 8007 -f $BIN/ApiLogin
spawn-fcgi -a 127.0.0.1 -p 8008 -f $BIN/ApiMd5
spawn-fcgi -a 127.0.0.1 -p 8009 -f $BIN/ApiSharefiles
spawn-fcgi -a 127.0.0.1 -p 8010 -f $BIN/ApiDealsharefile
spawn-fcgi -a 127.0.0.1 -p 8011 -f $BIN/ApiSliceUpload
spawn-fcgi -a 127.0.0.1 -p 8012 -f $BIN/ApiDealfile
spawn-fcgi -a 127.0.0.1 -p 8013 -f $BIN/ApiShareAccess
spawn-fcgi -a 127.0.0.1 -p 8014 -f $BIN/ApiMyfiles

echo "=== 启动 Cloudflare Tunnel（外网访问，可选）==="
# 内网穿透的启动方式因环境而异（systemd 服务 / 直接跑 cloudflared 二进制等），
# 因此这里不写死具体命令：需要时通过 CLOUDFLARE_TUNNEL_CMD 传入，未设置则跳过。
if [ -n "$CLOUDFLARE_TUNNEL_CMD" ]; then
    eval "$CLOUDFLARE_TUNNEL_CMD"
else
    echo "（已跳过：未设置 CLOUDFLARE_TUNNEL_CMD，仅提供内网访问）"
fi

echo "=== 完成 ==="
echo "内网地址: http://${SERVER_IP:-<本机IP>}"
echo "外网地址: 取决于所用的穿透方式（Cloudflare Tunnel 等）"
