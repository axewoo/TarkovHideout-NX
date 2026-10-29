#pragma once

#include <atomic>
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
// Frees the calling thread's connection; call it before a download thread ends.
void netThreadExit();

// cancel, when given and set from another thread, stops the transfer early.
HttpResult httpGet(const std::string &url, const std::vector<std::string> &headers = {},
                   const std::atomic<bool> *cancel = nullptr);
// POSTs body (JSON) and returns the response like httpGet.
HttpResult httpPost(const std::string &url, const std::vector<std::string> &headers, const std::string &body);
