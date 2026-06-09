#include "fastdfs_utils.h"
#include <iostream>
#include <cstdio>
using namespace std;

int main() {
    FILE* f = fopen("test.txt", "r");
    if (!f) {
        cout << "❌ 找不到 test.txt" << endl;
        return 1;
    }
    fclose(f);

    FastdfsUtils* fdfs = FastdfsUtils::getInstance();
    if (!fdfs->init("/etc/fdfs/client.conf")) {
        cerr << "❌ 初始化失败" << endl;
        return 1;
    }

    string file_id = fdfs->uploadFile("test.txt");
    if (!file_id.empty()) {
        cout << "🎉 上传成功，file_id: " << file_id << endl;
    } else {
        cout << "❌ 上传失败" << endl;
    }

    FastdfsUtils::destroyInstance();
    return 0;
}