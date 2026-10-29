#include "net.h"

#include <curl/curl.h>

#define USER_AGENT "TarkovHideout-NX/" APP_VERSION " (+https://github.com/axewoo/TarkovHideout-NX)"

static size_t writeCb(char *ptr, size_t size, size_t nmemb, void *user) {
    static_cast<std::string *>(user)->append(ptr, size * nmemb);
    return size * nmemb;
}

void netInit() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void netExit() {
    curl_global_cleanup();
}

HttpResult httpGet(const std::string &url, const std::vector<std::string> &headers) {
    HttpResult res;
    CURL *curl = curl_easy_init();
    if (!curl) {
        res.error = "curl init failed";
        return res;
    }
    struct curl_slist *list = nullptr;
    for (const auto &h : headers)
        list = curl_slist_append(list, h.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);
#ifdef _WIN32
    // Trust the certificates Windows trusts; there is no CA bundle file to point at.
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
    if (list)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        res.ok = true;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.status);
    } else {
        res.error = curl_easy_strerror(rc);
    }
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    return res;
}
