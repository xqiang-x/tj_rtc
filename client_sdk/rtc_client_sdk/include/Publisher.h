#pragma once

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <cstdint>
#include <memory>
#include "MemPool.h"
#include "bitrate_controller.h"

// Forward declarations
namespace fec_protocol { class FrameSender; struct ProtocolStats; }
class Pacer;
namespace p2p { class P2PManager; }

namespace push {

// 发布者（推流端）：以流名注册并发布一路流（1 视频 + 1 音频 + 1 消息）
// 线程模型：
//   - 调用方线程：调用 PushVideoFrame() 将帧入队（线程安全）
//   - 内部网络线程：负责 TCP 连接、订阅握手、发帧循环
class Publisher {
public:
    using OnConnectedCb   = std::function<void(uint32_t sessionId)>;
    using OnDisconnectedCb = std::function<void()>;
    // 码率建议回调：收到接收端反馈并经状态机决策后触发（在网络接收线程中调用）
    using OnBitrateAdviceCb = std::function<void(const fec_protocol::BitrateAdvice&)>;
    // 遥控信令回调：sourceSessionId=发送者，data/len=opaque payload
    using OnControlDataCb = std::function<void(uint32_t sourceSessionId, const uint8_t* data, size_t len)>;
    // 关键帧请求回调（PLI）：订阅端丢帧后经服务器节流到达。
    // 注意：在 UDP 接收线程中触发且持有发送锁，实现必须非阻塞——
    // 只置标志，由编码线程在下一帧强制 IDR（PushVideoFrame 时 isParamSet/isKeyFrame=true）
    using OnKeyFrameRequestCb = std::function<void()>;

    Publisher();
    ~Publisher();

    // 禁止拷贝
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;

    // 设置事件回调（需在 Start 前调用）
    void SetOnConnected(OnConnectedCb cb)      { m_onConnected    = std::move(cb); }
    void SetOnDisconnected(OnDisconnectedCb cb){ m_onDisconnected = std::move(cb); }
    void SetOnBitrateAdvice(OnBitrateAdviceCb cb){ m_onBitrateAdvice = std::move(cb); }

    // P2P 集成
    void SetP2PManager(p2p::P2PManager* mgr) { m_p2pManager = mgr; }
    bool SendP2PSignaling(const std::vector<uint8_t>& data);

    // 遥控信令（Pub → Subs 广播）
    void SetOnControlData(OnControlDataCb cb) { m_onControlData = std::move(cb); }
    void SendCtrlDownstream(const uint8_t* data, size_t len, bool reliable);

    // 关键帧请求（PLI）回调
    void SetOnKeyFrameRequest(OnKeyFrameRequestCb cb) { m_onKeyFrameRequest = std::move(cb); }

    // 启动：连接服务器 → 注册并发布流 → 进入推流循环
    // serverIp : SFU 服务器 IP
    // port     : TCP 端口
    // streamId : 流名（推流 uid，全局唯一）
    // userId   : 本端用户 ID
    // useUdp   : true → 使用 UDP 模式（无长度前缀）
    // udpPort  : UDP 端口（0 → port+1）
    bool Start(const std::string& serverIp, int port,
               const std::string& streamId, const std::string& userId,
               bool useUdp = false, int udpPort = 0);

    // 停止推流（阻塞直到网络线程退出）
    void Stop();

    bool IsRunning() const { return m_running.load(); }

    // 线程安全：将编码好的 H.264 帧加入发送队列
    // isParamSet  : true → SPS+PPS 参数集帧（VIDEO_PARAMS）
    // isKeyFrame  : true → IDR 帧（VIDEO_IDR），false → 普通帧（VIDEO）
    void PushVideoFrame(const uint8_t* data, size_t len,
                        bool isParamSet, bool isKeyFrame);
    
    // 线程安全：将 PCM 音频帧加入发送队列
    // 注意：目前只支持 PCM 16-bit，未经编码
    void PushAudioFrame(const uint8_t* data, size_t len);

private:
    // 网络线程主函数
    void NetThreadFunc();

    // TCP 工具函数（在网络线程中调用）
    bool TcpConnect(const std::string& ip, int port);
    void TcpClose();
    bool TcpSendAll(const uint8_t* data, size_t len);  // 带 4字节长度前缀
    bool TcpRecvAll(uint8_t* buf, size_t len);          // 精确读取 len 字节
    bool TcpRecvFrame(std::vector<uint8_t>& out);       // 读取一个完整协议帧

    // UDP 工具函数
    bool UdpConnect(const std::string& ip, int port);
    bool UdpSendAll(const uint8_t* data, size_t len);
    bool UdpRecvPacket(std::vector<uint8_t>& out);
    bool UdpDoSubscribe();

    // UDP 回包接收线程：NACK/PONG/STATS → FrameSender 处理（重传/RTT/缓存回收）
    void UdpRecvThreadFunc();

    // 可靠信令 TCP 接收线程（UDP 模式下用于接收 kCtrlReliable）
    void CtrlRecvThreadFunc();

    // 解析控制信令消息并触发回调
    void DispatchControlMessage(const uint8_t* data, size_t len);

    // 处理接收端反馈（STATS）：驱动码率状态机并应用建议
    void HandlePeerStats(const fec_protocol::ProtocolStats& stats);

    // FEC 发送端（网络线程与接收线程共享，访问需持 m_senderMutex）
    std::unique_ptr<fec_protocol::FrameSender> m_frameSender;
    std::mutex m_senderMutex;
    
    // Pacer 带宽控制（仅 UDP 模式使用，默认 50 Mbps）
    std::unique_ptr<Pacer> m_pacer;

    // 自适应码率状态机（仅 UDP 模式使用）
    std::unique_ptr<fec_protocol::BitrateController> m_bitrateController;
    
    // UDP 发送包内存池（由 m_senderMutex 保护）
    mempool::FixedMemoryPool m_udpPool{256};

    // SFU 协议操作（在网络线程中调用）
    bool DoSubscribe();    // 发送订阅请求并等待响应

    // 帧队列（生产者 = 编码线程，消费者 = 网络线程）
    struct FrameItem {
        std::vector<uint8_t> data;
        uint8_t frameType;   // SFU FrameType 字节（0x01/0x02/0x03/0x10）
        bool isKeyFrame;
        bool isAudio;  // true = 音频帧，false = 视频帧
    };

    std::deque<FrameItem>   m_queue;
    std::mutex              m_queueMutex;
    std::condition_variable m_queueCv;

    std::thread        m_netThread;
    std::thread        m_recvThread;
    std::atomic_bool   m_running{false};

    // 配置（Start 调用时写入，之后只读）
    std::string m_serverIp;
    int         m_port   = 0;
    std::string m_streamId;
    std::string m_userId;
    bool        m_useUdp = false;
    int         m_udpPort = 0;

    // 会话状态
    uint32_t     m_sessionId = 0;
    int          m_sockFd    = -1;   // TCP socket fd

    OnConnectedCb    m_onConnected;
    OnDisconnectedCb m_onDisconnected;
    OnBitrateAdviceCb m_onBitrateAdvice;
    p2p::P2PManager* m_p2pManager = nullptr;
    OnControlDataCb  m_onControlData;
    OnKeyFrameRequestCb m_onKeyFrameRequest;

    // 可靠信令 TCP 接收（UDP 模式下独立 TCP 连接接收 kCtrlReliable）
    std::thread      m_ctrlRecvThread;
    std::atomic_bool m_ctrlRunning{false};
    int              m_ctrlTcpFd = -1;

    // ── 断线检测与自动重连 ──────────────────────────────────
    // UDP sendto 连续失败计数（成功时清零），超过阈值判定断线
    std::atomic<int> m_udpSendFailCount{0};
    // 可靠信令 TCP 连接断开（CtrlRecvThreadFunc 退出）时置位，触发重连
    std::atomic_bool m_ctrlLinkDown{false};
    // 首次发帧时刻（FrameSender 创建时记录）：从未收到 PONG 的兜底超时基准
    uint64_t m_firstSendTimeUs = 0;

    // UDP 模式下：连续 sendto 失败超过该次数视为断线（服务器端口无监听时
    // ICMP unreachable 会让 sendto 立即失败，几十毫秒内即可触发）
    static constexpr int kUdpSendFailThreshold = 20;
    // UDP 模式下：超过该时长未收到 PONG 视为断线（服务器重启后旧 session
    // 失效、PING 无回应的场景）；PING 间隔 1s，10 倍阈值容错抖动
    static constexpr uint64_t kPongTimeoutUs = 10'000'000;
    // 从未收到过 PONG 的兜底超时：服务端若为不回 PONG 的旧版本/中间链路失效，
    // 推流开始该时长后仍无任何回包也判定断线，避免永久盲发（曾实测 3.5 小时）
    static constexpr uint64_t kFirstPongTimeoutUs = 15'000'000;
    // 断线后重连间隔
    static constexpr int kReconnectIntervalMs = 1000;
    
    // Pacer 配置（仅 UDP 模式）
    uint64_t m_bandwidth_bps = 50'000'000;  // 默认 50 Mbps
public:
    // 设置推流带宽（bits per second，仅 UDP 模式有效）
    void SetBandwidth(uint64_t bps) { m_bandwidth_bps = bps; }
};

} // namespace push
