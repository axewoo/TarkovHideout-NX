#pragma once

#include <string>
#include <vector>

struct HttpResult {
    bool ok = false;      // transport succeeded
    long status = 0;
    std::string body;
    std::string error;
};

void netInit();
void netExit();

HttpResult httpGet(const std::string &url, const std::vector<std::string> &headers = {});
