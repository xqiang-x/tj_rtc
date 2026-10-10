#include <jni.h>
#include <string>
#include <vector>
#include <android/log.h>
#include "Subscriber.h"
#include "SfuFrameType.h"

#define LOG_TAG "RtcJniBridge"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// 全局 Java VM 指针（用于线程附加）
static JavaVM* g_java_vm = nullptr;
static jobject g_java_thiz = nullptr;

// JNI_OnLoad: 库加载时保存 JavaVM
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_java_vm = vm;
    return JNI_VERSION_1_6;
}

// 附加当前线程到 JVM
static JNIEnv* AttachCurrentThread() {
    JNIEnv* env = nullptr;
    if (g_java_vm) {
        g_java_vm->AttachCurrentThread(&env, nullptr);
    }
    return env;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_example_rtcapp_RtcClient_nativeInit(JNIEnv* env, jobject thiz) {
    LOGD("nativeInit: 开始创建 Subscriber 实例");

    // 保存全局引用
    if (thiz) {
        g_java_thiz = env->NewGlobalRef(thiz);
        LOGD("nativeInit: Java 引用已保存");
    } else {
        LOGE("nativeInit: thiz 为 null!");
        return 0;
    }

    try {
        LOGD("nativeInit: 正在 new pull::Subscriber()...");
        auto* sub = new pull::Subscriber();
        if (sub) {
            LOGD("nativeInit: Subscriber 创建成功, ptr=%p", sub);
            return reinterpret_cast<jlong>(sub);
        } else {
            LOGE("nativeInit: Subscriber 创建失败，返回 null");
            return 0;
        }
    } catch (const std::exception& e) {
        LOGE("nativeInit: 异常: %s", e.what());
        return 0;
    } catch (...) {
        LOGE("nativeInit: 未知异常");
        return 0;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_rtcapp_RtcClient_nativeStart(
    JNIEnv* env, jobject thiz, jlong nativePtr,
    jstring serverIp, jint serverPort, jstring streamId,
    jstring userId, jboolean enableAudio, jboolean useUdp) {

    auto* sub = reinterpret_cast<pull::Subscriber*>(nativePtr);

    // 安全检查
    if (!sub) {
        LOGE("nativeStart: sub 指针为 null! nativePtr=%ld", static_cast<long>(nativePtr));
        return;
    }

    if (!serverIp || !streamId || !userId) {
        LOGE("nativeStart: null parameter detected");
        return;
    }

    // 解析参数
    const char* ip_str = env->GetStringUTFChars(serverIp, nullptr);
    std::string ip = ip_str ? ip_str : "";
    env->ReleaseStringUTFChars(serverIp, ip_str);

    const char* stream_str = env->GetStringUTFChars(streamId, nullptr);
    std::string stream = stream_str ? stream_str : "";
    env->ReleaseStringUTFChars(streamId, stream_str);

    const char* user_str = env->GetStringUTFChars(userId, nullptr);
    std::string user = user_str ? user_str : "";
    env->ReleaseStringUTFChars(userId, user_str);

    LOGD("nativeStart: server=%s:%d stream=%s user=%s audio=%d udp=%d",
         ip.c_str(), serverPort, stream.c_str(), user.c_str(), enableAudio, useUdp);

    // 创建 Java 对象的全局引用（用于回调）
    jobject global_thiz = env->NewGlobalRef(thiz);

    // 帧回调：按 FrameType 首字节分流视频/音频，转发到 Java 同名回调
    static int video_frame_count = 0;
    static int audio_frame_count = 0;
    sub->SetOnFrame([global_thiz](uint32_t sourceSessionId, std::vector<uint8_t>&& data) {
        if (data.empty()) return;

        bool isAudio = (data[0] == static_cast<uint8_t>(sfu::FrameType::AUDIO_PCM));

        JNIEnv* env = AttachCurrentThread();
        if (!env) {
            LOGE("[Frame] AttachCurrentThread 失败!");
            return;
        }

        jclass cls = env->GetObjectClass(global_thiz);
        if (cls == nullptr) {
            LOGE("[Frame] GetObjectClass 失败!");
            return;
        }

        jmethodID mid = nullptr;
        if (isAudio) {
            audio_frame_count++;
            if (audio_frame_count <= 5) {
                LOGD("[Audio] 收到帧 #%d, size=%zu", audio_frame_count, data.size());
            }
            mid = env->GetMethodID(cls, "onAudioFrame", "(I[B)V");
        } else {
            video_frame_count++;
            if (video_frame_count <= 10) {
                LOGD("[Video] 收到帧 #%d, size=%zu, sessionId=%u",
                     video_frame_count, data.size(), sourceSessionId);
            }
            mid = env->GetMethodID(cls, "onVideoFrame", "(I[B)V");
        }
        if (mid == nullptr) {
            LOGE("[Frame] Failed to find method: %s", isAudio ? "onAudioFrame" : "onVideoFrame");
            env->DeleteLocalRef(cls);
            return;
        }

        jbyteArray arr = env->NewByteArray(data.size());
        env->SetByteArrayRegion(arr, 0, data.size(),
                                reinterpret_cast<jbyte*>(data.data()));

        env->CallVoidMethod(global_thiz, mid, sourceSessionId, arr);

        // 检查 JNI 异常
        if (env->ExceptionCheck()) {
            LOGE("[Frame] Java 回调抛出异常!");
            env->ExceptionDescribe();
            env->ExceptionClear();
        }

        env->DeleteLocalRef(arr);
        env->DeleteLocalRef(cls);
    });

    // 设置连接回调
    sub->SetOnConnected([global_thiz](uint32_t sessionId) {
        JNIEnv* env = AttachCurrentThread();
        if (!env) return;

        jclass cls = env->GetObjectClass(global_thiz);
        jmethodID mid = env->GetMethodID(cls, "onConnected", "(I)V");
        if (mid) {
            env->CallVoidMethod(global_thiz, mid, sessionId);
        }
    });

    sub->SetOnDisconnected([global_thiz]() {
        JNIEnv* env = AttachCurrentThread();
        if (!env) return;

        jclass cls = env->GetObjectClass(global_thiz);
        jmethodID mid = env->GetMethodID(cls, "onDisconnected", "()V");
        if (mid) {
            env->CallVoidMethod(global_thiz, mid);
        }
    });

    sub->SetOnSubscribeFailed([global_thiz](const std::string& reason) {
        JNIEnv* env = AttachCurrentThread();
        if (!env) return;

        jclass cls = env->GetObjectClass(global_thiz);
        jmethodID mid = env->GetMethodID(cls, "onSubscribeFailed", "(Ljava/lang/String;)V");
        if (mid) {
            jstring jReason = env->NewStringUTF(reason.c_str());
            env->CallVoidMethod(global_thiz, mid, jReason);
            env->DeleteLocalRef(jReason);
        }
    });

    // 启动订阅（拉流）
    bool success = sub->Start(ip, serverPort, stream, user, useUdp, 0);
    LOGD("nativeStart: Subscriber Start result=%d", success);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_rtcapp_RtcClient_nativeStop(JNIEnv* env, jobject thiz, jlong nativePtr) {
    auto* sub = reinterpret_cast<pull::Subscriber*>(nativePtr);
    sub->Stop();
    LOGD("nativeStop: Subscriber stopped");
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_rtcapp_RtcClient_nativeRelease(JNIEnv* env, jobject thiz, jlong nativePtr) {
    auto* sub = reinterpret_cast<pull::Subscriber*>(nativePtr);
    delete sub;

    // 释放全局引用
    if (g_java_thiz) {
        env->DeleteGlobalRef(g_java_thiz);
        g_java_thiz = nullptr;
    }

    LOGD("nativeRelease: Subscriber released");
}