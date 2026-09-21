#pragma once

#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <cstdint>
#include <memory>

#include "stats.h"

// Forward declaration
namespace fec_protocol { class FrameReceiver; }
namespace p2p { class P2PManager; }

namespace pull {

// 订阅者（拉流端）：以流名订阅一条已存在的流（接收 1 视频 + 1 音频 + 1 消息）
// 线程模型：
//   - 内部网络线程：负责 TCP 连接、订阅握手、接收帧
//   - 调用方线程：通过回调接收帧数据
class Subscriber {
public:
    using OnConnectedCb   = std::function<void(uint32_t sessionId)>;
    using OnDisconnectedCb = std::function<void()>;
    using OnSubscribeFailedCb = std::function<void(const std::string& reason)>;
    using OnFrameCb       = std::function<void(uint32_t sourceSessionId, std::vector<uint8_t>&& data)>;
    // 遥控信令回调：sourceSessionId=发送者，data/len=opaque payload
    using OnControlDataCb = std::function<void(uint32_t sourceSessionId, const uint8_t* data, size_t len)>;

    Subscriber();
    ~Subscriber();

    // 禁止拷贝
    Subscriber(const Subscriber&) = delete;
    Subscriber& operator=(const Subscriber&) = delete;

    // 设置事件回调（需在 Start 前调用）
    void SetOnConnected(OnConnectedCb cb)         { m_onConnected    = std::move(cb); }
    void SetOnDisconnected(OnDisconnectedCb cb)   { m_onDisconnected = std::move(cb); }
    // 订阅被服务器拒绝（如流不存在）时回调，参数为服务器返回的原因
    void SetOnSubscribeFailed(OnSubscribeFailedCb cb) { m_onSubscribeFailed = std::move(cb); }
    void SetOnFrame(OnFrameCb cb)                 { m_onFrame        = std::move(cb); }

    // P2P 集成
    void SetP2PManager(p2p::P2PManager* mgr) { m_p2pManager = mgr; }
    bool SendP2PSignaling(const std::vector<uint8_t>& data);
    void ProcessExternalFec(const uint8_t* fecData, size_t fecLen);

    // 遥控信令（Sub → Pub 上游）
    void SetOnControlData(OnControlDataCb cb) { m_onControlData = std::move(cb); }
    void SendCtrlUpstream(const uint8_t* data, size_t len, bool reliable);

    // 手动请求关键帧：向推流端发送 PLI（线程安全，由网络线程实际发送）
    void RequestKeyFrame() { m_pliRequested.store(true); }

    // 启动：连接服务器 → 订阅流 → 进入接收循环
    // serverIp : SFU 服务器 IP
    // port     : TCP 端口
    // streamId : 要订阅的流名（与推流端 uid 一致）
    // userId   : 本端用户 ID
    // useUdp   : true → 使用 UDP 模式（无长度前缀）
    // udpPort  : UDP 端口（0 → port+1）
    bool Start(const std::string& serverIp, int port,
               const std::string& streamId, const std::string& userId,
               bool useUdp = false, int udpPort = 0);

    // 停止拉流（阻塞直到网络线程退出）
    void Stop();

    bool IsRunning() const { return m_running.load(); }

    // FEC/NACK/帧级统计快照（观测用；网络线程更新，拷贝可能有轻微竞态）
    fec_protocol::ProtocolStats GetStats() const;

private:
    void NetThreadFunc();

    // TCP 工具
    bool TcpConnect(const std::string& ip, int port);
    void TcpClose();
    bool TcpSendAll(const uint8_t* data, size_t len);
    bool TcpRecvAll(uint8_t* buf, size_t len);
    bool TcpRecvFrame(std::vector<uint8_t>& out);

    // UDP 工具
    bool UdpConnect(const std::string& ip, int port);
    bool UdpSendAll(const uint8_t* data, size_t len);
    bool UdpRecvPacket(std::vector<uint8_t>& out);
    bool UdpDoSubscribe();

    // FEC 接收端初始化（UDP/TCP 共用；NACK/STATS 回传按模式走各自通道）
    void InitFrameReceiver();

    // FEC 接收端
    std::unique_ptr<fec_protocol::FrameReceiver> m_frameReceiver;

    // 订阅协议
    bool DoSubscribe();

    // 解析控制信令消息并触发回调
    void DispatchControlMessage(const uint8_t* data, size_t len);

    // 可靠信令 TCP 接收线程（UDP 模式下读取 m_ctrlTcpFd）
    void CtrlRecvThreadFunc();

    // 状态
    std::string m_serverIp;
    int         m_port     = 0;
    std::string m_streamId;
    std::string m_userId;
    uint32_t    m_sessionId = 0;
    uint32_t    m_sourceSessionId = 0;  // 最近收到的推流端 sessionId（NACK/STATS 回传路由用）
    bool        m_useUdp   = false;
    int         m_udpPort  = 0;

    int         m_sockFd   = -1;
    int         m_ctrlTcpFd = -1;  // UDP 模式下可靠信令 TCP 连接
    std::atomic<bool> m_ctrlRunning{false};
    std::thread     m_ctrlRecvThread;
    std::atomic<bool> m_running{false};
    std::thread     m_netThread;

    // 回调
    OnConnectedCb    m_onConnected;
    OnDisconnectedCb m_onDisconnected;
    OnSubscribeFailedCb m_onSubscribeFailed;
    OnFrameCb        m_onFrame;
    p2p::P2PManager* m_p2pManager = nullptr;
    OnControlDataCb  m_onControlData;
    std::string      m_failReason;  // 最近一次订阅失败的服务器原因
    std::atomic<bool> m_pliRequested{false};  // RequestKeyFrame 标志，网络线程消费
};

} // namespace pull
