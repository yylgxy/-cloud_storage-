#include <iostream>
#include "md5_utils.h"

int main() {
    std::cout << MD5Utils::md5("123456") << std::endl;
    return 0;
}