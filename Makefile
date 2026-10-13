# ============================================================
#  云存储文件系统 · 构建脚本
#
#  用法：
#    make              编译全部 10 个 FastCGI 接口到 common/
#    make benchmark    单独编译负载均衡基准测试
#    make clean        清理中间文件与可执行文件
#
#  依赖库（若路径 / 名称与默认不同，改下面的 LDLIBS）：
#    libfcgi          FastCGI 开发库（编译时需 fcgi_stdio.h / fcgiapp.h）
#    libmysqlclient   MySQL 客户端库
#    libhiredis       Redis 客户端库
#    libfdfsclient    FastDFS 客户端库
#    libcrypto        OpenSSL（md5_utils 使用 EVP 接口）
#    libcjson         cJSON
# ============================================================

CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++11 -Wall
CPPFLAGS += -Icommon
LDLIBS   ?= -lfcgi -lmysqlclient -lhiredis -lfdfsclient -lcrypto -lcjson

COMMON := common

# ---- 公共依赖：每个 FastCGI 接口都会链接 ----
UTILS_SRC := $(COMMON)/mysql_utils.cpp   \
             $(COMMON)/redis_utils.cpp   \
             $(COMMON)/fastdfs_utils.cpp \
             $(COMMON)/cjson_utils.cpp   \
             $(COMMON)/md5_utils.cpp     \
             $(COMMON)/lb_algorithm.cpp  \
             $(COMMON)/lb_tracker.cpp

UTILS_OBJ := $(UTILS_SRC:.cpp=.o)

# ---- 10 个 FastCGI 接口（端口分配见 common/start_cloud.sh）----
APPS := ApiUpload        \
        ApiRegister      \
        ApiLogin         \
        ApiMd5           \
        ApiSharefiles    \
        ApiDealsharefile \
        ApiSliceUpload   \
        ApiDealfile      \
        ApiShareAccess   \
        ApiMyfiles

BINS := $(addprefix $(COMMON)/,$(APPS))

.PHONY: all benchmark clean

all: $(BINS)

# 链接：接口自身目标文件 + 公共依赖目标文件
$(COMMON)/%: $(COMMON)/%.o $(UTILS_OBJ)
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

# 编译
$(COMMON)/%.o: $(COMMON)/%.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

# ---- 基准测试：纯算法，不依赖 FastCGI / 数据库 ----
benchmark: $(COMMON)/lb_benchmark.cpp $(COMMON)/lb_algorithm.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ -o $(COMMON)/lb_benchmark

clean:
	rm -f $(UTILS_OBJ) $(BINS) $(COMMON)/lb_benchmark
