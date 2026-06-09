#!/bin/bash
# 快速启动云存储所有服务（在重启后执行）
# 用法: bash ~/start_cloud.sh

BIN=/home/yyl/cloud_storage/common

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

echo "=== 启动 Cloudflare Tunnel（外网访问）==="
systemctl --user start cloudflared-tunnel

echo "=== 完成 ==="
echo "内网地址: http://192.168.150.101"
echo "隧道地址: cat /tmp/cloudflared-tunnel.log | grep 'trycloudflare.com'"
