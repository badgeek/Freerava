// cyclomp — JNI HTTPFileSource for MapLibre on Android.
// mbgl-core ships no HTTP transport for a pure-NDK app (the stock one lives
// in the Java SDK). This implements mln::HTTPFileSource with
// java.net.HttpURLConnection through the captured JavaVM — no Java sources.
#ifdef __ANDROID__
#include "android_env.h"

#include <mln/storage/http_file_source.hpp>
#include <mln/util/logging.hpp>
#include <mln/storage/resource.hpp>
#include <mln/storage/resource_options.hpp>
#include <mln/storage/response.hpp>
#include <mln/util/async_request.hpp>
#include <mln/util/async_task.hpp>
#include <mln/util/client_options.hpp>

#include <android/log.h>

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mln {

// The Java SDK normally provides the log sink; route to logcat directly.
void Log::platformRecord(EventSeverity severity, const std::string &msg) {
    int prio = severity == EventSeverity::Error   ? ANDROID_LOG_ERROR
             : severity == EventSeverity::Warning ? ANDROID_LOG_WARN
                                                  : ANDROID_LOG_INFO;
    __android_log_print(prio, "maplibre", "%s", msg.c_str());
}

namespace {

// Marshals the finished Response back to the mbgl thread that issued the
// request (same pattern as the darwin implementation).
struct RequestShared {
    std::mutex mutex;
    bool cancelled = false;
    Response &response;
    util::AsyncTask &async;
    RequestShared(Response &r, util::AsyncTask &a) : response(r), async(a) {}
    void notify(const Response &r) {
        std::scoped_lock lock(mutex);
        if (!cancelled) {
            response = r;
            async.send();
        }
    }
    void cancel() {
        std::scoped_lock lock(mutex);
        cancelled = true;
    }
};

class JniHTTPRequest : public AsyncRequest {
public:
    explicit JniHTTPRequest(FileSource::Callback cb)
        : shared(std::make_shared<RequestShared>(response, async)),
          callback(std::move(cb)) {}
    ~JniHTTPRequest() override { shared->cancel(); }

    std::shared_ptr<RequestShared> shared;

private:
    FileSource::Callback callback;
    Response response;
    util::AsyncTask async{[this] {
        // The callback may delete `this`; copy to temporaries first.
        auto cb = callback;
        auto res = response;
        cb(res);
    }};
};

// Blocking HTTP GET via JNI. Runs on a detached worker thread.
Response fetch(const Resource &resource) {
    __android_log_print(ANDROID_LOG_INFO, "cyclomp-http", "GET %s",
                        resource.url.c_str());
    Response out;
    auto fail = [&out](Response::Error::Reason reason, const char *msg) {
        out.error = std::make_unique<Response::Error>(reason, msg);
    };

    JavaVM *vm = cyclomp_java_vm();
    if (!vm) {
        fail(Response::Error::Reason::Connection, "JavaVM unavailable");
        return out;
    }
    JNIEnv *env = nullptr;
    bool attached = false;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            fail(Response::Error::Reason::Connection, "AttachCurrentThread failed");
            return out;
        }
        attached = true;
    }

    env->PushLocalFrame(64);
    bool pending = false; // java exception raised
    auto check = [&] {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            pending = true;
        }
        return pending;
    };

    do {
        jclass urlCls = env->FindClass("java/net/URL");
        jmethodID urlCtor = env->GetMethodID(urlCls, "<init>", "(Ljava/lang/String;)V");
        jobject url = env->NewObject(urlCls, urlCtor, env->NewStringUTF(resource.url.c_str()));
        if (check() || !url) { fail(Response::Error::Reason::Other, "bad URL"); break; }

        jmethodID open = env->GetMethodID(urlCls, "openConnection", "()Ljava/net/URLConnection;");
        jobject conn = env->CallObjectMethod(url, open);
        if (check() || !conn) { fail(Response::Error::Reason::Connection, "openConnection failed"); break; }

        jclass connCls = env->FindClass("java/net/HttpURLConnection");
        env->CallVoidMethod(conn, env->GetMethodID(connCls, "setConnectTimeout", "(I)V"), 10000);
        env->CallVoidMethod(conn, env->GetMethodID(connCls, "setReadTimeout", "(I)V"), 20000);
        jmethodID setProp = env->GetMethodID(
            connCls, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V");
        env->CallVoidMethod(conn, setProp, env->NewStringUTF("User-Agent"),
                            env->NewStringUTF("cyclomp/0.1 MapLibreNative"));
        if (resource.dataRange) {
            char range[64];
            std::snprintf(range, sizeof range, "bytes=%llu-%llu",
                          (unsigned long long)resource.dataRange->first,
                          (unsigned long long)resource.dataRange->second);
            env->CallVoidMethod(conn, setProp, env->NewStringUTF("Range"),
                                env->NewStringUTF(range));
        }

        jint code = env->CallIntMethod(conn, env->GetMethodID(connCls, "getResponseCode", "()I"));
        if (check()) { fail(Response::Error::Reason::Connection, "connect failed"); break; }

        jobject stream = env->CallObjectMethod(
            conn, env->GetMethodID(
                      connCls, code >= 400 ? "getErrorStream" : "getInputStream",
                      "()Ljava/io/InputStream;"));
        if (env->ExceptionCheck()) { env->ExceptionClear(); stream = nullptr; }

        std::string body;
        if (stream) {
            jclass inCls = env->FindClass("java/io/InputStream");
            jmethodID readM = env->GetMethodID(inCls, "read", "([B)I");
            jbyteArray buf = env->NewByteArray(16384);
            for (;;) {
                jint n = env->CallIntMethod(stream, readM, buf);
                if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
                if (n <= 0) break;
                size_t off = body.size();
                body.resize(off + (size_t)n);
                env->GetByteArrayRegion(buf, 0, n, reinterpret_cast<jbyte *>(&body[off]));
            }
            env->CallVoidMethod(stream, env->GetMethodID(inCls, "close", "()V"));
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        env->CallVoidMethod(conn, env->GetMethodID(connCls, "disconnect", "()V"));
        if (env->ExceptionCheck()) env->ExceptionClear();

        if (code == 200 || code == 206) {
            out.data = std::make_shared<const std::string>(std::move(body));
        } else if (code == 204) {
            out.noContent = true;
        } else if (code == 404) {
            // Missing tiles (e.g. ocean) are normal — treat as empty.
            if (resource.kind == Resource::Kind::Tile) out.noContent = true;
            else fail(Response::Error::Reason::NotFound, "HTTP 404");
        } else if (code == 429) {
            fail(Response::Error::Reason::RateLimit, "HTTP 429");
        } else if (code >= 500) {
            fail(Response::Error::Reason::Server, "HTTP 5xx");
        } else {
            fail(Response::Error::Reason::Other, "unexpected HTTP status");
        }
    } while (false);

    env->PopLocalFrame(nullptr);
    if (attached) vm->DetachCurrentThread();
    return out;
}

} // namespace

class HTTPFileSource::Impl {
public:
    Impl(const ResourceOptions &ro, const ClientOptions &co)
        : resourceOptions(ro.clone()), clientOptions(co.clone()) {}
    std::mutex mutex;
    ResourceOptions resourceOptions;
    ClientOptions clientOptions;
};

HTTPFileSource::HTTPFileSource(const ResourceOptions &resourceOptions,
                               const ClientOptions &clientOptions)
    : impl(std::make_unique<Impl>(resourceOptions, clientOptions)) {}

HTTPFileSource::~HTTPFileSource() = default;

void HTTPFileSource::setResourceOptions(ResourceOptions options) {
    std::scoped_lock lock(impl->mutex);
    impl->resourceOptions = options.clone();
}

ResourceOptions HTTPFileSource::getResourceOptions() {
    std::scoped_lock lock(impl->mutex);
    return impl->resourceOptions.clone();
}

void HTTPFileSource::setClientOptions(ClientOptions options) {
    std::scoped_lock lock(impl->mutex);
    impl->clientOptions = options.clone();
}

ClientOptions HTTPFileSource::getClientOptions() {
    std::scoped_lock lock(impl->mutex);
    return impl->clientOptions.clone();
}

std::unique_ptr<AsyncRequest> HTTPFileSource::request(const Resource &resource,
                                                      Callback callback) {
    auto req = std::make_unique<JniHTTPRequest>(std::move(callback));
    auto shared = req->shared;
    Resource copy = resource;
    std::thread([shared, copy] { shared->notify(fetch(copy)); }).detach();
    return req;
}

} // namespace mln
#endif // __ANDROID__
