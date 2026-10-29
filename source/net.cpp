#include "net.h"

#define USER_AGENT "TarkovHideout-NX/" APP_VERSION " (+https://github.com/axewoo/TarkovHideout-NX)"

#ifdef __ANDROID__
// Android has no system libcurl; the activity's Java HttpURLConnection does the request.
#include <SDL2/SDL.h>
#include <jni.h>

void netInit() {}
void netExit() {}

HttpResult httpGet(const std::string &url, const std::vector<std::string> &headers) {
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
    jmethodID mid = env->GetStaticMethodID(cls, "httpGet", "(Ljava/lang/String;[Ljava/lang/String;)[B");
    if (!mid) {
        env->ExceptionClear();
        res.error = "httpGet missing";
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
    auto out = static_cast<jbyteArray>(env->CallStaticObjectMethod(cls, mid, ju, jh));
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
#endif
