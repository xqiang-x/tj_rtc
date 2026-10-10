#pragma once
// SdlRenderer.h — SDL2 视频渲染器
// 线程模型：
//   - Init() / PollAndRender() 必须在主线程调用
//   - EnqueueFrame() 可在任意线程调用（内部加锁）

#include <SDL2/SDL.h>
#include <CoreVideo/CoreVideo.h>

#include <queue>
#include <mutex>
#include <string>

class SdlRenderer {
public:
    SdlRenderer();
    ~SdlRenderer();

    SdlRenderer(const SdlRenderer&) = delete;
    SdlRenderer& operator=(const SdlRenderer&) = delete;

    // 初始化 SDL 窗口和渲染器（主线程调用）
    bool Init(const std::string& title = "SFU Pull — H.264 Live");

    // 将解码后的帧加入渲染队列（任意线程安全）
    // 内部会 CFRetain(buf)，调用者无需保留
    void EnqueueFrame(CVImageBufferRef buf, int width, int height);

    // 处理 SDL 事件并渲染一帧（主线程调用）
    // 返回 false 表示窗口被关闭（应退出主循环）
    bool PollAndRender();

    bool IsInitialized() const { return m_sdlInited; }

private:
    // 创建或重建 NV12 纹理（尺寸变化时自动触发）
    void EnsureTexture(int width, int height);

    // 将 CVImageBuffer（NV12）上传到 SDL 纹理并渲染
    void RenderNV12(CVImageBufferRef buf, int width, int height);

    SDL_Window*   m_window   = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture*  m_texture  = nullptr;
    int  m_texW = 0, m_texH = 0;
    bool m_sdlInited = false;

    struct FrameEntry {
        CVImageBufferRef buf    = nullptr;
        int              width  = 0;
        int              height = 0;
    };

    std::queue<FrameEntry> m_queue;
    std::mutex             m_mutex;
};
