#include "net.h"

#define USER_AGENT "TarkovHideout-NX/" APP_VERSION " (+https://github.com/axewoo/TarkovHideout-NX)"

#ifdef __ANDROID__
// Android has no system libcurl; the activity's Java HttpURLConnection does the request.
#include <SDL2/SDL.h>
#include <jni.h>

void netInit() {}
void netExit() {}
void netThreadExit() {}

HttpResult httpGet(const std::string &url, const std::vector<std::string> &headers, const std::atomic<bool> *) {
    return httpPost(url, headers, std::string());
}

// An empty body makes it a GET (the Java side picks the method).
HttpResult httpPost(const std::string &url, const std::vector<std::string> &headers, const std::string &body) {
    HttpResult res;
    JNIEnv *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) {
        res.error = "no Java environment";
        return res;
    }
    jclass cls = env->GetObjectClass(activity);
    // static byte[] httpGet(String url, String[] headers): 4 bytes of status (big endian, -1 when
    // the request failed) followed by the body, or the error message.
    // static byte[] http(String url, String[] headers, byte[] body): body null for a GET.
    jmethodID mid = env->GetStaticMethodID(cls, "http", "(Ljava/lang/String;[Ljava/lang/String;[B)[B");
    if (!mid) {
        env->ExceptionClear();
        res.error = "http missing";
        env->DeleteLocalRef(cls);
        env->DeleteLocalRef(activity);
        return res;
    }
    jclass strCls = env->FindClass("java/lang/String");
    std::vector<std::string> all = headers;
    all.push_back("User-Agent: " USER_AGENT);
    jobjectArray jh = env->NewObjectArray(all.size(), strCls, nullptr);
    for (size_t i = 0; i < all.size(); i++) {
        jstring h = env->NewStringUTF(all[i].c_str());
        env->SetObjectArrayElement(jh, i, h);
        env->DeleteLocalRef(h);
    }
    jstring ju = env->NewStringUTF(url.c_str());
    jbyteArray jb = nullptr;
    if (!body.empty()) {
        jb = env->NewByteArray(body.size());
        env->SetByteArrayRegion(jb, 0, body.size(), reinterpret_cast<const jbyte *>(body.data()));
    }
    auto out = static_cast<jbyteArray>(env->CallStaticObjectMethod(cls, mid, ju, jh, jb));
    if (jb)
        env->DeleteLocalRef(jb);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        out = nullptr;
    }
    if (out) {
        jsize n = env->GetArrayLength(out);
        std::string data(size_t(n), '\0');
        env->GetByteArrayRegion(out, 0, n, reinterpret_cast<jbyte *>(data.data()));
        if (n >= 4) {
            const auto *b = reinterpret_cast<const unsigned char *>(data.data());
            int32_t status = int32_t(uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3]);
            if (status >= 0) {
                res.ok = true;
                res.status = status;
                res.body = data.substr(4);
            } else {
                res.error = data.substr(4);
            }
        }
        env->DeleteLocalRef(out);
    } else {
        res.error = "request failed";
    }
    env->DeleteLocalRef(ju);
    env->DeleteLocalRef(jh);
    env->DeleteLocalRef(strCls);
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return res;
}
#else
#include <curl/curl.h>

static size_t writeCb(char *ptr, size_t size, size_t nmemb, void *user) {
    static_cast<std::string *>(user)->append(ptr, size * nmemb);
    return size * nmemb;
}

// One handle per thread keeps its connection, and the TLS session with it, open between
// requests, which saves a handshake for every map piece fetched from the same server.
static thread_local CURL *threadCurl = nullptr;

static int progressCb(void *cancel, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<const std::atomic<bool> *>(cancel)->load() ? 1 : 0;
}

void netInit() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void netThreadExit() {
    if (threadCurl)
        curl_easy_cleanup(threadCurl);
    threadCurl = nullptr;
}

void netExit() {
    netThreadExit();
    curl_global_cleanup();
}

static HttpResult request(const std::string &url, const std::vector<std::string> &headers,
                          const std::atomic<bool> *cancel, const std::string *postBody) {
    HttpResult res;
    if (!threadCurl)
        threadCurl = curl_easy_init();
    CURL *curl = threadCurl;
    if (!curl) {
        res.error = "curl init failed";
        return res;
    }
    curl_easy_reset(curl);
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
    if (postBody) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postBody->c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)postBody->size());
    }
    if (cancel) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool> *>(cancel));
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        res.ok = true;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.status);
    } else {
        res.error = curl_easy_strerror(rc);
    }
    curl_slist_free_all(list);
    return res;
}

HttpResult httpGet(const std::string &url, const std::vector<std::string> &headers, const std::atomic<bool> *cancel) {
    return request(url, headers, cancel, nullptr);
}

HttpResult httpPost(const std::string &url, const std::vector<std::string> &headers, const std::string &body) {
    return request(url, headers, nullptr, &body);
}
#endif
