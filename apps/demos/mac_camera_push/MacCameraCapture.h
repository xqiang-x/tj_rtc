#pragma once

#include <functional>
#include <string>
#include <vector>
#include <cstdint>

#ifdef __APPLE__
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#endif

namespace camera {

// 每帧回调：CVPixelBufferRef 是 kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
// 调用者不需要 retain/release pixelBuffer（已在回调期间持有）
using FrameCallback = std::function<void(CVPixelBufferRef pixelBuffer, CMTime pts)>;

class MacCameraCapture {
public:
    MacCameraCapture();
    ~MacCameraCapture();

    // 禁止拷贝
    MacCameraCapture(const MacCameraCapture&) = delete;
    MacCameraCapture& operator=(const MacCameraCapture&) = delete;

    // 列出所有可用摄像头（返回设备名称列表）
    static std::vector<std::string> ListDevices();

    // 注册帧回调（必须在 Start 之前调用）
    void SetFrameCallback(FrameCallback cb);

    // 启动采集
    // width/height: 请求分辨率（实际分辨率由设备决定）
    // fps:           请求帧率
    // deviceIndex:   摄像头索引（0 = 默认/第一个）
    // 返回 true 表示成功
    bool Start(int width = 1280, int height = 720, int fps = 30, int deviceIndex = 0);

    // 停止采集
    void Stop();

    bool IsRunning() const { return m_running; }

    // 获取当前实际分辨率
    int GetWidth()  const { return m_actualWidth; }
    int GetHeight() const { return m_actualHeight; }

private:
    // 使用 void* 存储 Objective-C 对象，避免在纯 C++ 头文件中暴露 ObjC 类型
    void* m_session   = nullptr;   // AVCaptureSession*
    void* m_delegate  = nullptr;   // CaptureDelegate* (内部 ObjC 类)
    void* m_output    = nullptr;   // AVCaptureVideoDataOutput*
    void* m_queue     = nullptr;   // dispatch_queue_t

    FrameCallback m_callback;
    bool m_running      = false;
    int  m_actualWidth  = 0;
    int  m_actualHeight = 0;
};

} // namespace camera
