// SdlRenderer.cpp — SDL2 NV12 渲染实现

#include "SdlRenderer.h"

#include <iostream>

// ─────────────────────────────────────────────────────────────────────────────
SdlRenderer::SdlRenderer()  = default;

SdlRenderer::~SdlRenderer()
{
    // 释放队列中还未渲染的帧
    std::lock_guard<std::mutex> lk(m_mutex);
    while (!m_queue.empty()) {
        if (m_queue.front().buf) CFRelease(m_queue.front().buf);
        m_queue.pop();
    }

    if (m_texture)  { SDL_DestroyTexture(m_texture);   m_texture  = nullptr; }
    if (m_renderer) { SDL_DestroyRenderer(m_renderer); m_renderer = nullptr; }
    if (m_window)   { SDL_DestroyWindow(m_window);     m_window   = nullptr; }
    if (m_sdlInited){ SDL_Quit();                       m_sdlInited = false;  }
}

// ─────────────────────────────────────────────────────────────────────────────
bool SdlRenderer::Init(const std::string& title)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << "[SDL] SDL_Init 失败: " << SDL_GetError() << std::endl;
        return false;
    }
    m_sdlInited = true;

    // 初始以 1280x720 创建窗口，首帧到达后若尺寸不同会自动调整
    m_window = SDL_CreateWindow(
        title.c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!m_window) {
        std::cerr << "[SDL] SDL_CreateWindow 失败: " << SDL_GetError() << std::endl;
        return false;
    }

    // 优先硬件加速渲染器
    m_renderer = SDL_CreateRenderer(m_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!m_renderer) {
        std::cerr << "[SDL] 硬件渲染器不可用，回退到软件渲染器" << std::endl;
        m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!m_renderer) {
        std::cerr << "[SDL] SDL_CreateRenderer 失败: " << SDL_GetError() << std::endl;
        return false;
    }

    SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
    SDL_RenderClear(m_renderer);
    SDL_RenderPresent(m_renderer);

    std::cout << "[SDL] 窗口已创建: " << title << std::endl;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 入队（任意线程安全），保留最新 4 帧，丢弃过旧的帧以保持低延迟
// ─────────────────────────────────────────────────────────────────────────────
void SdlRenderer::EnqueueFrame(CVImageBufferRef buf, int width, int height)
{
    if (!buf || !m_sdlInited) return;
    CFRetain(buf);

    std::lock_guard<std::mutex> lk(m_mutex);
    // 丢弃过旧的帧：队列超过 4 帧时丢头部
    while (m_queue.size() >= 4) {
        if (m_queue.front().buf) CFRelease(m_queue.front().buf);
        m_queue.pop();
    }
    m_queue.push({buf, width, height});
}

// ─────────────────────────────────────────────────────────────────────────────
// 主线程调用：处理 SDL 事件，渲染一帧
// ─────────────────────────────────────────────────────────────────────────────
bool SdlRenderer::PollAndRender()
{
    // ── SDL 事件处理 ──────────────────────────────────────────────────────────
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) return false;
        if (event.type == SDL_KEYDOWN) {
            if (event.key.keysym.sym == SDLK_ESCAPE ||
                event.key.keysym.sym == SDLK_q) {
                return false;
            }
        }
    }

    // ── 取出一帧渲染 ──────────────────────────────────────────────────────────
    FrameEntry entry;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_queue.empty()) {
            entry = m_queue.front();
            m_queue.pop();
        }
    }

    if (entry.buf) {
        EnsureTexture(entry.width, entry.height);
        if (m_texture) {
            RenderNV12(entry.buf, entry.width, entry.height);
        }
        CFRelease(entry.buf);
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 当尺寸变化时重建纹理，并同步调整窗口/渲染逻辑尺寸
// ─────────────────────────────────────────────────────────────────────────────
void SdlRenderer::EnsureTexture(int width, int height)
{
    if (m_texture && m_texW == width && m_texH == height) return;

    if (m_texture) {
        SDL_DestroyTexture(m_texture);
        m_texture = nullptr;
    }

    // SDL_PIXELFORMAT_NV12 → 双平面 YCbCr (Y + UV 交错)，与 VideoToolbox 输出匹配
    m_texture = SDL_CreateTexture(
        m_renderer,
        SDL_PIXELFORMAT_NV12,
        SDL_TEXTUREACCESS_STREAMING,
        width, height);

    if (!m_texture) {
        std::cerr << "[SDL] SDL_CreateTexture(NV12) 失败: " << SDL_GetError() << std::endl;
        return;
    }

    m_texW = width;
    m_texH = height;

    // 保持纵横比缩放到当前窗口
    SDL_RenderSetLogicalSize(m_renderer, width, height);
    std::cout << "[SDL] 纹理已创建: " << width << "x" << height << " NV12" << std::endl;
}

// ─────────────────────────────────────────────────────────────────────────────
// 将 CVImageBuffer（NV12 双平面）上传到 SDL 纹理并渲染
// ─────────────────────────────────────────────────────────────────────────────
void SdlRenderer::RenderNV12(CVImageBufferRef buf, int /*width*/, int /*height*/)
{
    if (!buf || !m_texture || !m_renderer) return;

    CVPixelBufferLockBaseAddress(buf, kCVPixelBufferLock_ReadOnly);

    // 平面 0：Y
    const uint8_t* yPlane  = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(buf, 0);
    int            yPitch  = (int)CVPixelBufferGetBytesPerRowOfPlane(buf, 0);
    // 平面 1：UV（交错）
    const uint8_t* uvPlane = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(buf, 1);
    int            uvPitch = (int)CVPixelBufferGetBytesPerRowOfPlane(buf, 1);

    SDL_UpdateNVTexture(m_texture, nullptr, yPlane, yPitch, uvPlane, uvPitch);

    CVPixelBufferUnlockBaseAddress(buf, kCVPixelBufferLock_ReadOnly);

    SDL_RenderClear(m_renderer);
    SDL_RenderCopy(m_renderer, m_texture, nullptr, nullptr);
    SDL_RenderPresent(m_renderer);
}
