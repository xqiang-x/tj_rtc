// Publisher.cpp
// 纯 C++ TCP 推流客户端：connect → subscribe → send stream data

#include "Publisher.h"

// SFU 协议头（仅使用 inline 函数，无需链接）
#include "SfuSubscribeProtocol.h"
#include "SfuProtocol.h"
#include "SfuFrameType.h"
#include "SfuP2PProtocol.h"
#include "SfuSockOpt.h"
#include "P2PManager.h"

// FEC 分片
#include "sender.h"

// Pacer 带宽控制
#include "Pacer.h"
#include "types.h"
#include "config.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <iostream>
#include <chrono>
#include <algorithm>

namespace push {

using namespace sfu;  // 使用 FrameType 枚举

// ─────────────────────────────────────────────────────────────
Publisher::Publisher()  = default;
Publisher::~Publisher() { Stop(); }

// ─────────────────────────────────────────────────────────────
bool Publisher::Start(const std::string& serverIp, int port,
                       const std::string& streamId, const std::string& userId,
                       bool useUdp, int udpPort)
{
    if (m_running.load()) return true;

    m_serverIp = serverIp;
    m_port     = port;
    m_streamId = streamId;
    m_userId   = userId;
    m_useUdp   = useUdp;
    m_udpPort  = (udpPort > 0) ? udpPort : port + 1;
    m_running.store(true);

    m_netThread = std::thread(&Publisher::NetThreadFunc, this);
    return true;
}

void Publisher::Stop() {
    // 先设 running=false，再关 socket、再 join
    // 注意：不能用 if(!exchange) return 做早退 —— 网络线程会自己把 m_running 置 false，
    // 若此时再调 Stop()，exchange 返回 false 导致跳过 join，析构 joinable thread → terminate
    m_running.store(false);

    // 停止信令接收线程
    m_ctrlRunning.store(false);
    if (m_ctrlTcpFd >= 0) {
        ::shutdown(m_ctrlTcpFd, SHUT_RDWR);
        ::close(m_ctrlTcpFd);
        m_ctrlTcpFd = -1;
    }

    // 唤醒阻塞在 condition_variable 上的网络线程
    m_queueCv.notify_all();

    // 关闭 socket，使阻塞的 recv/send 立即返回
    TcpClose();

    // 无论 m_running 之前是什么值，只要线程可 join 就必须 join
    if (m_netThread.joinable()) {
        m_netThread.join();
    }
    if (m_recvThread.joinable()) {
        m_recvThread.join();
    }
    if (m_ctrlRecvThread.joinable()) {
        m_ctrlRecvThread.join();
    }
}

// ─────────────────────────────────────────────────────────────
// 线程安全入队
// ─────────────────────────────────────────────────────────────
static constexpr size_t kMaxVideoQueueSize  = 60;
static constexpr size_t kMaxAudioQueueSize  = 100;  // 约 2 秒

void Publisher::PushVideoFrame(const uint8_t* data, size_t len,
                                    bool isParamSet, bool isKeyFrame)
{
    if (!m_running.load() || m_sessionId == 0) return;

    FrameItem item;
    item.data.assign(data, data + len);
    item.frameType = isParamSet ? static_cast<uint8_t>(FrameType::VIDEO_PARAMS)
                   : isKeyFrame ? static_cast<uint8_t>(FrameType::VIDEO_IDR)
                                : static_cast<uint8_t>(FrameType::VIDEO_P);
    item.isKeyFrame = isKeyFrame || isParamSet;
    item.isAudio = false;

    {
        std::lock_guard<std::mutex> lk(m_queueMutex);
        if (m_queue.size() >= kMaxVideoQueueSize) {
            // 积压：丢弃最近关键帧之前的所有帧，保证剩余流可从关键帧解码
            ssize_t kfPos = -1;
            for (ssize_t i = static_cast<ssize_t>(m_queue.size()) - 1; i >= 0; --i) {
                if (!m_queue[i].isAudio && m_queue[i].isKeyFrame) { kfPos = i; break; }
            }
            if (kfPos > 0) {
                m_queue.erase(m_queue.begin(), m_queue.begin() + kfPos);
            } else {
                m_queue.pop_front();
            }
        }
        m_queue.push_back(std::move(item));
    }
    m_queueCv.notify_one();
}

void Publisher::PushAudioFrame(const uint8_t* data, size_t len)
{
    if (!m_running.load() || m_sessionId == 0) return;

    FrameItem item;
    item.data.assign(data, data + len);
    item.frameType = static_cast<uint8_t>(FrameType::AUDIO_PCM);
    item.isKeyFrame = false;
    item.isAudio = true;

    {
        std::lock_guard<std::mutex> lk(m_queueMutex);
        while (m_queue.size() >= kMaxAudioQueueSize) m_queue.pop_front();
        m_queue.push_back(std::move(item));
    }
    m_queueCv.notify_one();
}

// ─────────────────────────────────────────────────────────────
// 网络线程：connect → subscribe → send loop
// ─────────────────────────────────────────────────────────────
void Publisher::NetThreadFunc() {
    std::cout << "[Push] 网络线程启动 (" << (m_useUdp ? "UDP" : "TCP") << ")" << std::endl;

    auto nowUs = []() -> uint64_t {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };

    auto sendFrame = [this, &nowUs](uint8_t frameType,
                                    const std::vector<uint8_t>& data) -> bool {
        if (m_useUdp) {
            // UDP 模式：FEC 分片（sender 与接收线程共享，全程持锁）
            std::lock_guard<std::mutex> lk(m_senderMutex);
            if (!m_frameSender) {
                fec_protocol::SenderConfig cfg;
                cfg.mtu = 1400;  // 默认 MTU
                cfg.fec_ratio = 0.1f;  // 10% FEC 冗余
                cfg.cache_timeout_ms = 300 * 1000;  // 无损模式：已发块缓存 300s，覆盖分钟级停滞后到达的 NACK
                m_frameSender = std::make_unique<fec_protocol::FrameSender>(cfg);
                m_firstSendTimeUs = nowUs();

                if (!m_pacer) {
                    m_pacer = std::make_unique<Pacer>(m_bandwidth_bps);
                    std::cout << "[Push] Pacer initialized with "
                              << m_bandwidth_bps / 1'000'000 << " Mbps" << std::endl;
                }

                // 自适应码率状态机：以配置带宽为上限，媒体码率从中档起步
                if (!m_bitrateController) {
                    fec_protocol::BitrateControllerConfig bcCfg;
                    bcCfg.max_bitrate_bps = static_cast<uint32_t>(m_bandwidth_bps);
                    bcCfg.initial_bitrate_bps =
                        std::min<uint32_t>(bcCfg.max_bitrate_bps, 3'000'000);
                    bcCfg.fec_initial = cfg.fec_ratio;
                    // FEC 自适应冗余的约束与公式（fec = clamp(loss×factor, min, max)）
                    bcCfg.fec_min = cfg.fec_ratio_min;
                    bcCfg.fec_max = cfg.fec_ratio_max;
                    bcCfg.fec_loss_factor = cfg.fec_loss_factor;
                    m_bitrateController =
                        std::make_unique<fec_protocol::BitrateController>(bcCfg);
                }

                // 接收端反馈（STATS）→ 码率状态机
                m_frameSender->setStatsCallback(
                    [this](const fec_protocol::ProtocolStats& s) { HandlePeerStats(s); });

                // 关键帧请求（PLI）→ 对外回调（调用方应置标志，下一帧强制 IDR）
                m_frameSender->setKeyFrameRequestCallback([this]() {
                    std::cout << "[Push] PLI received - key frame requested" << std::endl;
                    if (m_onKeyFrameRequest) m_onKeyFrameRequest();
                });

                m_frameSender->setSendCallback([this](const uint8_t* pkt, size_t len) {
                    // SFU 外层封装 FEC 分片包：[SFU Header][sessionId][FEC包]
                    // 使用内存池避免每包 new/delete
                    uint8_t* buf = static_cast<uint8_t*>(m_udpPool.allocate());
                    size_t msgLen = sfu::SerializeStreamDataTo(buf, mempool::FixedMemoryPool::kBlockSize,
                                                               m_sessionId, pkt, len);
                    if (msgLen == 0) {
                        // payload 太大（超过 1494），回退 vector 方式
                        m_udpPool.deallocate(buf);
                        auto sfuMsg = sfu::SerializeStreamData(m_sessionId, pkt, len);
                        int64_t wait_us = m_pacer->beforeSend(sfuMsg.size());
                        if (wait_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(wait_us));
                        UdpSendAll(sfuMsg.data(), sfuMsg.size());
                        m_pacer->afterSend(sfuMsg.size());

                        // P2P 双发（Phase 1 探测阶段：所有包走双路）
                        if (m_p2pManager && m_p2pManager->GetPhase() >= p2p::P2PPhase::kConnected) {
                            auto p2pPkt = sfu::SerializeP2PDataPacket(pkt, len);
                            m_p2pManager->SendP2PPacket(p2pPkt.data(), p2pPkt.size());
                        }
                        return;
                    }

                    // Apply Pacer rate limiting
                    int64_t wait_us = m_pacer->beforeSend(msgLen);
                    if (wait_us > 0) {
                        std::this_thread::sleep_for(std::chrono::microseconds(wait_us));
                    }

                    UdpSendAll(buf, msgLen);
                    m_pacer->afterSend(msgLen);
                    m_udpPool.deallocate(buf);

                    // P2P 双发（Phase 1 探测阶段：所有包走双路）
                    if (m_p2pManager && m_p2pManager->GetPhase() >= p2p::P2PPhase::kConnected) {
                        auto p2pPkt = sfu::SerializeP2PDataPacket(pkt, len);
                        m_p2pManager->SendP2PPacket(p2pPkt.data(), p2pPkt.size());
                    }
                });
            }

            // 根据帧类型选择 FEC FrameType
            fec_protocol::FrameType fecType;
            switch (frameType) {
                case 0x01: fecType = fec_protocol::frame_type::VIDEO_PARAMS; break;
                case 0x02: fecType = fec_protocol::frame_type::VIDEO_IDR;    break;
                case 0x03: fecType = fec_protocol::frame_type::VIDEO;        break;
                case 0x10: fecType = fec_protocol::frame_type::AUDIO;        break;
                default:   fecType = fec_protocol::frame_type::DATA;         break;
            }

            bool ret = m_frameSender->sendFrame(data.data(), data.size(), fecType, nowUs());
            // tick 驱动 PING/缓存清理/FEC 调整
            m_frameSender->tick(nowUs());
            return ret;
        } else {
            // TCP 模式：直接发送（不需要 FEC/Pacer）
            std::vector<uint8_t> payload;
            payload.reserve(1 + data.size());
            payload.push_back(frameType);
            payload.insert(payload.end(), data.begin(), data.end());
            auto msg = sfu::SerializeStreamData(m_sessionId, payload.data(), payload.size());
            return TcpSendAll(msg.data(), msg.size());
        }
    };

    // 断线判定：控制信令 TCP 断开 / UDP sendto 连续失败 / PONG 超时
    auto linkDown = [this, &nowUs]() -> bool {
        if (m_ctrlLinkDown.load()) return true;
        if (!m_useUdp) return false;
        if (m_udpSendFailCount.load() >= kUdpSendFailThreshold) return true;
        if (m_frameSender) {
            uint64_t last_pong = m_frameSender->lastPongTimeUs();
            if (last_pong != 0) {
                // 已收到过 PONG：超过 kPongTimeoutUs 无回包视为断线
                if (nowUs() - last_pong > kPongTimeoutUs) return true;
            } else if (m_firstSendTimeUs != 0 &&
                       nowUs() - m_firstSendTimeUs > kFirstPongTimeoutUs) {
                // 从未收到过 PONG（服务端不回 PONG 的旧版本/中间链路失效）：
                // 推流开始 kFirstPongTimeoutUs 后仍无任何回包也判定断线，防止盲发
                return true;
            }
        }
        return false;
    };

    // 重连/重试等待（1 秒，可被 Stop 中断）
    auto retryWait = [this]() {
        for (int i = 0; i < kReconnectIntervalMs / 100 && m_running.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    };

    // ── 主循环：连接 → 订阅 → 推流 → 断线后 1s 重试 ──────────
    while (m_running.load()) {
        if (m_useUdp) {
            // ── UDP 路径 ──────────────────────────────────────
            while (m_running.load()) {
                if (UdpConnect(m_serverIp, m_udpPort)) break;
                std::cerr << "[Push] UDP 连接失败，1 秒后重试..." << std::endl;
                retryWait();
            }
            if (!m_running.load()) break;

            if (!UdpDoSubscribe()) {
                std::cerr << "[Push] UDP 订阅失败，1 秒后重试" << std::endl;
                TcpClose();
                retryWait();
                continue;
            }

            // 启动回包接收线程（NACK/PONG/STATS → 重传/RTT/缓存回收）
            m_recvThread = std::thread(&Publisher::UdpRecvThreadFunc, this);
        } else {
            // ── TCP 路径 ──────────────────────────────────────
            while (m_running.load()) {
                if (TcpConnect(m_serverIp, m_port)) break;
                std::cerr << "[Push] 连接失败，1 秒后重试..." << std::endl;
                retryWait();
            }
            if (!m_running.load()) break;

            if (!DoSubscribe()) {
                std::cerr << "[Push] 订阅失败，1 秒后重试" << std::endl;
                TcpClose();
                retryWait();
                continue;
            }
        }

        // 启动可靠信令接收线程（UDP 模式独立 TCP 连接，TCP 模式复用主连接）
        m_ctrlLinkDown.store(false);
        m_ctrlRunning.store(true);
        m_ctrlRecvThread = std::thread(&Publisher::CtrlRecvThreadFunc, this);

        if (m_onConnected) m_onConnected(m_sessionId);
        std::cout << "[Push] " << (m_useUdp ? "UDP " : "")
                  << "开始推流，sessionId=" << m_sessionId << std::endl;

        // ── 发帧循环（含断线检测） ────────────────────────────
        uint64_t sendCount = 0;
        while (m_running.load()) {
            FrameItem item;
            {
                std::unique_lock<std::mutex> lk(m_queueMutex);
                m_queueCv.wait_for(lk, std::chrono::milliseconds(100),
                                   [this]{ return !m_queue.empty() || !m_running.load(); });
                if (!m_running.load() && m_queue.empty()) break;
                if (m_queue.empty()) {
                    // 空闲期也要驱动 PING / NACK 重传 / 缓存清理，并检测断线
                    if (m_useUdp) {
                        std::lock_guard<std::mutex> slk(m_senderMutex);
                        if (m_frameSender) m_frameSender->tick(nowUs());
                    }
                    if (linkDown()) break;
                    continue;
                }
                item = std::move(m_queue.front());
                m_queue.pop_front();
            }

            if (!sendFrame(item.frameType, item.data)) {
                std::cerr << "[Push] 发送失败，连接已断开" << std::endl;
                break;
            }

            if (linkDown()) break;

            if (++sendCount % 500 == 0) {
                std::cout << "[Push] 已发送 " << sendCount << " 帧 ("
                          << (item.isAudio ? "音频" : "视频") << ")" << std::endl;
            }
        }
        if (!m_running.load()) break;  // Stop() 请求，直接退出

        // ── 断线清理：复位会话状态，为下次连接做准备 ──────────
        std::cout << "[Push] 检测到连接断开，准备重连..." << std::endl;
        m_sessionId = 0;
        m_ctrlRunning.store(false);
        if (m_ctrlTcpFd >= 0) {
            ::shutdown(m_ctrlTcpFd, SHUT_RDWR);
            ::close(m_ctrlTcpFd);
            m_ctrlTcpFd = -1;
        }
        if (m_ctrlRecvThread.joinable()) m_ctrlRecvThread.join();
        TcpClose();
        if (m_recvThread.joinable()) m_recvThread.join();
        {
            std::lock_guard<std::mutex> lk(m_senderMutex);
            m_frameSender.reset();
            m_pacer.reset();
            m_bitrateController.reset();
        }
        m_udpSendFailCount.store(0);
        m_ctrlLinkDown.store(false);
        m_firstSendTimeUs = 0;
        {
            std::lock_guard<std::mutex> lk(m_queueMutex);
            m_queue.clear();
        }
        if (m_onDisconnected) m_onDisconnected();

        // 1 秒后重试
        retryWait();
    }

    m_running.store(false);  // 让接收线程一并退出
    TcpClose();
    if (m_recvThread.joinable()) m_recvThread.join();
    if (m_onDisconnected) m_onDisconnected();
    std::cout << "[Push] 网络线程退出" << std::endl;
}

// ─────────────────────────────────────────────────────────────
// P2P 信令发送（通过 TCP 连接）
// ─────────────────────────────────────────────────────────────
bool Publisher::SendP2PSignaling(const std::vector<uint8_t>& data) {
    if (m_sockFd < 0 || data.empty()) return false;
    return TcpSendAll(data.data(), data.size());
}

// ─────────────────────────────────────────────────────────────
// 遥控信令：Pub → Subs 广播（downstream）
// ─────────────────────────────────────────────────────────────
void Publisher::SendCtrlDownstream(const uint8_t* data, size_t len, bool reliable) {
    if (!m_running.load() || m_sessionId == 0) return;

    // 构造客户端→服务器格式: [ctrlType][direction=0x00][session_id=0][payload]
    std::vector<uint8_t> msg(6 + len);
    msg[0] = reliable ? static_cast<uint8_t>(SfuCtrlType::kCtrlReliable)
                      : static_cast<uint8_t>(SfuCtrlType::kCtrlUnreliable);
    msg[1] = 0x00;  // downstream
    msg[2] = msg[3] = msg[4] = msg[5] = 0;  // session_id = 0
    if (len > 0) memcpy(msg.data() + 6, data, len);

    if (reliable) {
        // 可靠：走 TCP（带 4 字节长度前缀），帧首需带 SFU header 字节
        std::vector<uint8_t> tcpMsg(1 + msg.size());
        tcpMsg[0] = MakeHeader(PROTOCOL_VERSION, static_cast<uint8_t>(MsgType::kCmd));
        memcpy(tcpMsg.data() + 1, msg.data(), msg.size());
        if (m_useUdp && m_ctrlTcpFd >= 0) {
            // UDP 模式：通过专用 TCP 连接发送
            uint8_t prefix[4] = {
                (uint8_t)(tcpMsg.size() >> 24), (uint8_t)(tcpMsg.size() >> 16),
                (uint8_t)(tcpMsg.size() >> 8),  (uint8_t)(tcpMsg.size())
            };
            ::send(m_ctrlTcpFd, prefix, 4, MSG_NOSIGNAL);
            ::send(m_ctrlTcpFd, tcpMsg.data(), tcpMsg.size(), MSG_NOSIGNAL);
        } else if (!m_useUdp && m_sockFd >= 0) {
            TcpSendAll(tcpMsg.data(), tcpMsg.size());
        }
    } else {
        // 不可靠：走 UDP
        if (m_useUdp) {
            // 需要加上 SFU header byte
            std::vector<uint8_t> udpMsg(1 + msg.size());
            udpMsg[0] = MakeHeader(PROTOCOL_VERSION, static_cast<uint8_t>(MsgType::kCmd));
            memcpy(udpMsg.data() + 1, msg.data(), msg.size());
            UdpSendAll(udpMsg.data(), udpMsg.size());
        } else if (m_sockFd >= 0) {
            // TCP 模式无 UDP，降级为 TCP
            TcpSendAll(msg.data(), msg.size());
        }
    }
}

// ─────────────────────────────────────────────────────────────
// 解析接收到的控制信令并回调
// ─────────────────────────────────────────────────────────────
void Publisher::DispatchControlMessage(const uint8_t* data, size_t len) {
    // data 格式: [header][ctrlType][direction][session_id(4)][payload]
    if (len < 8) return;
    uint8_t ctrlType = data[1];
    if (ctrlType != static_cast<uint8_t>(SfuCtrlType::kCtrlReliable) &&
        ctrlType != static_cast<uint8_t>(SfuCtrlType::kCtrlUnreliable)) return;

    uint32_t sourceSessionId =
        (static_cast<uint32_t>(data[3]) << 24) |
        (static_cast<uint32_t>(data[4]) << 16) |
        (static_cast<uint32_t>(data[5]) <<  8) |
        (static_cast<uint32_t>(data[6]));
    const uint8_t* payload = data + 7;
    size_t payloadLen = len - 7;

    if (m_onControlData) {
        m_onControlData(sourceSessionId, payload, payloadLen);
    }
}

// ─────────────────────────────────────────────────────────────
// 可靠信令 TCP 接收线程
// UDP 模式：独立 TCP 连接，接收 kCtrlReliable 消息
// TCP 模式：复用 m_sockFd，接收可靠控制消息
// ─────────────────────────────────────────────────────────────
void Publisher::CtrlRecvThreadFunc() {
    std::cout << "[Push-Ctrl] 信令接收线程启动" << std::endl;

    if (m_useUdp) {
        // 建立专用 TCP 连接用于可靠信令
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            std::cerr << "[Push-Ctrl] socket 失败" << std::endl;
            m_ctrlLinkDown.store(true);
            return;
        }

        struct timeval tv;
        tv.tv_sec = 3;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(m_port));
        inet_pton(AF_INET, m_serverIp.c_str(), &addr.sin_addr);

        if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "[Push-Ctrl] TCP 连接失败: " << strerror(errno) << std::endl;
            ::close(fd);
            m_ctrlLinkDown.store(true);
            return;
        }

        // 无限等待（控制消息低频，不设超时）
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        m_ctrlTcpFd = fd;

        // 发送订阅请求（与主会话同流/用户：UDP 模式的独立 TCP 控制会话）
        // 纯控制会话：flags 全 false（不注册流、不收媒体，仅用于信令配对）
        sfu::SubscribeFlags flags{};
        flags.audio    = false;
        flags.video    = false;
        flags.data     = false;
        flags.reserved = 0;
        auto req = sfu::SerializeSubscribeReq(sfu::SubscribeRole::kPublisher, m_streamId, m_userId, flags);
        uint8_t prefix[4] = {
            (uint8_t)(req.size() >> 24), (uint8_t)(req.size() >> 16),
            (uint8_t)(req.size() >> 8),  (uint8_t)(req.size())
        };
        ::send(fd, prefix, 4, MSG_NOSIGNAL);
        ::send(fd, req.data(), req.size(), MSG_NOSIGNAL);

        // 读取订阅响应
        uint8_t respPrefix[4];
        size_t got = 0;
        while (got < 4 && m_ctrlRunning.load()) {
            ssize_t r = ::recv(fd, respPrefix + got, 4 - got, 0);
            if (r <= 0) break;
            got += r;
        }
        if (got == 4) {
            uint32_t frameLen = (respPrefix[0] << 24) | (respPrefix[1] << 16) |
                                (respPrefix[2] << 8) | respPrefix[3];
            if (frameLen > 0 && frameLen < 1024) {
                std::vector<uint8_t> resp(frameLen);
                got = 0;
                while (got < frameLen && m_ctrlRunning.load()) {
                    ssize_t r = ::recv(fd, resp.data() + got, frameLen - got, 0);
                    if (r <= 0) break;
                    got += r;
                }
            }
        }
    }

    int tcpFd = m_useUdp ? m_ctrlTcpFd : m_sockFd;

    while (m_ctrlRunning.load()) {
        // 读取 4 字节长度前缀
        uint8_t prefix[4];
        size_t got = 0;
        while (got < 4 && m_ctrlRunning.load()) {
            ssize_t r = ::recv(tcpFd, prefix + got, 4 - got, 0);
            if (r <= 0) {
                if (!m_ctrlRunning.load()) break;
                if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                goto exit_loop;
            }
            got += r;
        }
        if (!m_ctrlRunning.load()) break;

        uint32_t frameLen = (static_cast<uint32_t>(prefix[0]) << 24) |
                            (static_cast<uint32_t>(prefix[1]) << 16) |
                            (static_cast<uint32_t>(prefix[2]) <<  8) |
                            (static_cast<uint32_t>(prefix[3]));
        if (frameLen == 0 || frameLen > 1024 * 1024) break;

        std::vector<uint8_t> frame(frameLen);
        got = 0;
        while (got < frameLen && m_ctrlRunning.load()) {
            ssize_t r = ::recv(tcpFd, frame.data() + got, frameLen - got, 0);
            if (r <= 0) {
                if (!m_ctrlRunning.load()) break;
                if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                goto exit_loop;
            }
            got += r;
        }

        if (frame.size() >= 2) {
            uint8_t ctrlType = frame[1];
            if (ctrlType == static_cast<uint8_t>(SfuCtrlType::kCtrlReliable) ||
                ctrlType == static_cast<uint8_t>(SfuCtrlType::kCtrlUnreliable)) {
                DispatchControlMessage(frame.data(), frame.size());
            } else if ((frame[0] >> 4) == fec_protocol::kProtocolVersion &&
                       (ctrlType == static_cast<uint8_t>(fec_protocol::MsgType::NACK) ||
                        ctrlType == static_cast<uint8_t>(fec_protocol::MsgType::STATS) ||
                        ctrlType == static_cast<uint8_t>(fec_protocol::MsgType::PONG) ||
                        ctrlType == static_cast<uint8_t>(fec_protocol::MsgType::PLI))) {
                // 服务器把 FEC 反馈（NACK/STATS/PONG/PLI）走 TCP 控制通道回传，
                // 规避运营商 NAT 周期性丢弃 UDP 入向映射导致的假断线循环
                std::lock_guard<std::mutex> lk(m_senderMutex);
                if (m_frameSender) {
                    uint64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    m_frameSender->onPacketReceived(frame.data(), frame.size(), now_us);
                }
            }
        }
    }

exit_loop:
    m_ctrlLinkDown.store(true);
    if (m_useUdp && m_ctrlTcpFd >= 0) {
        ::close(m_ctrlTcpFd);
        m_ctrlTcpFd = -1;
    }
    std::cout << "[Push-Ctrl] 信令接收线程退出" << std::endl;
}

// ─────────────────────────────────────────────────────────────
// 接收端反馈（STATS）→ 码率状态机 → 应用 FEC/Pacer/回调
// 注意：由 FrameSender 的 stats 回调触发，此时已持有 m_senderMutex
// ─────────────────────────────────────────────────────────────
void Publisher::HandlePeerStats(const fec_protocol::ProtocolStats& stats) {
    if (!m_bitrateController) return;

    uint64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // 周期性详细统计（接收端视角）
    static uint64_t lastDetailUs = 0;
    if (nowUs - lastDetailUs >= 2'000'000) {
        lastDetailUs = nowUs;
        std::cout << "[Push-PeerStats] loss=" << (stats.loss_rate * 100.0f) << "%"
                  << " fecRec=" << (stats.fec_recovery_rate * 100.0f) << "%"
                  << " nackRec=" << (stats.nack_recovery_rate * 100.0f) << "%"
                  << " goodput=" << stats.goodput_bps / 1000 << "kbps" << std::endl;
    }

    // 驱动状态机；未产生新建议则不做任何改动
    if (!m_bitrateController->onFeedback(nowUs, stats.goodput_bps, stats.loss_rate))
        return;

    const fec_protocol::BitrateAdvice& adv = m_bitrateController->lastAdvice();

    // 1) 应用 FEC 冗余比
    if (m_frameSender) {
        m_frameSender->setFecRatio(adv.fec_ratio);
    }

    // 2) Pacer 线速上限 = 媒体码率 × (1 + FEC) + 5% 余量
    if (m_pacer) {
        uint64_t wire = static_cast<uint64_t>(
            static_cast<double>(adv.target_bitrate_bps) * (1.0 + adv.fec_ratio) * 1.05);
        m_pacer->setBandwidth(wire);
    }

    std::cout << "[Push] BitrateAdvice: target=" << adv.target_bitrate_bps / 1000
              << " kbps fec=" << adv.fec_ratio
              << " goodput=" << adv.measured_goodput_bps / 1000 << " kbps"
              << " loss=" << (adv.loss_rate * 100.0f) << "%"
              << (adv.rate_decreased ? " (decreased)" : "") << std::endl;

    // 3) 通知应用层（调整编码器码率）
    if (m_onBitrateAdvice) m_onBitrateAdvice(adv);
}

// ─────────────────────────────────────────────────────────────
// UDP 回包接收线程：NACK → 触发重传；PONG → RTT；STATS → 缓存回收
// ─────────────────────────────────────────────────────────────
void Publisher::UdpRecvThreadFunc() {
    uint8_t buf[2048];
    auto nowUs = []() -> uint64_t {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };

    while (m_running.load()) {
        ssize_t n = ::recv(m_sockFd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (!m_running.load()) break;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;  // SO_RCVTIMEO 超时
            break;
        }
        if (n == 0) continue;

        // 检查是否为 P2P 控制消息（ctrlType 0x17~0x1C）
        if (n >= 2 && m_p2pManager) {
            uint8_t ctrlType = buf[1];
            if (ctrlType >= 0x17 && ctrlType <= 0x1C) {
                switch (ctrlType) {
                    case 0x18:  // kP2PResponse
                        m_p2pManager->HandleP2PResponse(buf + 1, static_cast<size_t>(n - 1));
                        break;
                    case 0x19:  // kP2PCandidate
                        m_p2pManager->HandleP2PCandidate(buf + 1, static_cast<size_t>(n - 1));
                        break;
                    case 0x1A:  // kP2PConnectCheck
                        m_p2pManager->HandleP2PConnectCheck(buf + 1, static_cast<size_t>(n - 1));
                        break;
                    case 0x1B:  // kP2PFallback
                        m_p2pManager->HandleP2PFallback(buf + 1, static_cast<size_t>(n - 1));
                        break;
                    default:
                        break;
                }
                continue;
            }
        }

        // 检查是否为遥控信令（kCtrlReliable=0x20, kCtrlUnreliable=0x21）
        if (n >= 7 && m_onControlData) {
            uint8_t ctrlType = buf[1];
            if (ctrlType == static_cast<uint8_t>(SfuCtrlType::kCtrlReliable) ||
                ctrlType == static_cast<uint8_t>(SfuCtrlType::kCtrlUnreliable)) {
                DispatchControlMessage(buf, static_cast<size_t>(n));
                continue;
            }
        }

        // 服务端以裸 FEC 包回发（NACK/PONG/STATS），直接交给 FEC 发送端
        std::lock_guard<std::mutex> lk(m_senderMutex);
        if (m_frameSender) {
            m_frameSender->onPacketReceived(buf, static_cast<size_t>(n), nowUs());
        }
    }
}

// ─────────────────────────────────────────────────────────────
// TCP 工具
// ─────────────────────────────────────────────────────────────
bool Publisher::TcpConnect(const std::string& ip, int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "[Push] socket() 失败: " << strerror(errno) << std::endl;
        return false;
    }

    // TCP_NODELAY 降低延迟
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // 设置连接超时（通过 select）
    struct timeval tv;
    tv.tv_sec  = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[Push] connect(" << ip << ":" << port
                  << ") 失败: " << strerror(errno) << std::endl;
        ::close(fd);
        return false;
    }

    // 连接成功后取消超时（改用阻塞 I/O，recv 超时 5s）
    tv.tv_sec  = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    // 发送不设超时（push 场景发送一般很快）
    tv.tv_sec  = 0;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    m_sockFd = fd;
    std::cout << "[Push] 已连接到 " << ip << ":" << port << std::endl;
    return true;
}

void Publisher::TcpClose() {
    if (m_sockFd >= 0) {
        ::shutdown(m_sockFd, SHUT_RDWR);
        ::close(m_sockFd);
        m_sockFd = -1;
    }
}

// 带 4 字节大端长度前缀发送（与 sfu_server 的 TCP 帧格式一致）
bool Publisher::TcpSendAll(const uint8_t* data, size_t len) {
    if (m_sockFd < 0) return false;

    // 长度前缀
    uint8_t prefix[4] = {
        (uint8_t)(len >> 24),
        (uint8_t)(len >> 16),
        (uint8_t)(len >>  8),
        (uint8_t)(len      )
    };

    auto sendExact = [this](const uint8_t* buf, size_t n) -> bool {
        size_t sent = 0;
        while (sent < n) {
            ssize_t r = ::send(m_sockFd, buf + sent, n - sent, MSG_NOSIGNAL);
            if (r <= 0) return false;
            sent += static_cast<size_t>(r);
        }
        return true;
    };

    return sendExact(prefix, 4) && sendExact(data, len);
}

// 精确读取 len 字节
bool Publisher::TcpRecvAll(uint8_t* buf, size_t len) {
    if (m_sockFd < 0) return false;
    size_t got = 0;
    while (got < len) {
        ssize_t r = ::recv(m_sockFd, buf + got, len - got, 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

// 读取一个完整的 SFU TCP 帧（4字节长度前缀 + 载荷）
bool Publisher::TcpRecvFrame(std::vector<uint8_t>& out) {
    uint8_t prefix[4];
    if (!TcpRecvAll(prefix, 4)) return false;

    uint32_t frameLen =
        (static_cast<uint32_t>(prefix[0]) << 24) |
        (static_cast<uint32_t>(prefix[1]) << 16) |
        (static_cast<uint32_t>(prefix[2]) <<  8) |
        (static_cast<uint32_t>(prefix[3]));

    if (frameLen == 0 || frameLen > 8 * 1024 * 1024) return false;  // 上限 8MB

    out.resize(frameLen);
    return TcpRecvAll(out.data(), frameLen);
}

// ─────────────────────────────────────────────────────────────
// SFU 订阅握手
// ─────────────────────────────────────────────────────────────
bool Publisher::DoSubscribe() {
    // 构造订阅请求：以发布者角色注册流（推流端不接收他人流）
    sfu::SubscribeFlags flags{};
    flags.audio    = true;   // 声明音频通道
    flags.video    = true;   // 声明视频通道
    flags.data     = true;   // 声明消息通道
    flags.reserved = 0;

    auto req = sfu::SerializeSubscribeReq(sfu::SubscribeRole::kPublisher, m_streamId, m_userId, flags);

    if (!TcpSendAll(req.data(), req.size())) {
        std::cerr << "[Push] 发送注册请求失败" << std::endl;
        return false;
    }

    std::cout << "[Push] 已发送注册请求，user=" << m_userId
              << " stream=" << m_streamId << std::endl;

    // 等待服务器回复
    std::vector<uint8_t> resp;
    if (!TcpRecvFrame(resp)) {
        std::cerr << "[Push] 读取订阅响应超时或连接断开" << std::endl;
        return false;
    }

    auto parsed = sfu::ParseSubscribeResp(resp.data(), resp.size());
    if (!parsed.valid || parsed.status != 0) {
        std::cerr << "[Push] 订阅失败: " << parsed.reason << std::endl;
        return false;
    }

    m_sessionId = parsed.sessionId;
    std::cout << "[Push] 订阅成功，sessionId=" << m_sessionId << std::endl;
    return true;
}

// ─────────────────────────────────────────────────────────────
// UDP 工具
// ─────────────────────────────────────────────────────────────
bool Publisher::UdpConnect(const std::string& ip, int port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::cerr << "[Push] UDP socket() 失败: " << strerror(errno) << std::endl;
        return false;
    }

    // 尽力把接收缓冲提到 8MB（非 root 时钳制到 rmem_max）
    int rcvbuf = sfu::SetUdpRecvBuffer(fd);
    std::cout << "[Push] UDP RCVBUF target=8388608 actual=" << rcvbuf << " bytes" << std::endl;

    struct sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port   = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, ip.c_str(), &serverAddr.sin_addr);

    if (::connect(fd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "[Push] UDP connect(" << ip << ":" << port
                  << ") 失败: " << strerror(errno) << std::endl;
        ::close(fd);
        return false;
    }

    struct timeval tv;
    tv.tv_sec  = 3;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    m_sockFd = fd;
    std::cout << "[Push] UDP 已连接到 " << ip << ":" << port << std::endl;
    return true;
}

bool Publisher::UdpSendAll(const uint8_t* data, size_t len) {
    if (m_sockFd < 0) return false;
    ssize_t n = ::send(m_sockFd, data, len, 0);
    if (n != static_cast<ssize_t>(len)) {
        m_udpSendFailCount.fetch_add(1);
        std::cerr << "[Push] UDP send 失败: sent=" << n << " expected=" << len
                  << " errno=" << errno << " (" << strerror(errno) << ")" << std::endl;
        return false;
    }
    m_udpSendFailCount.store(0);
    return true;
}

bool Publisher::UdpRecvPacket(std::vector<uint8_t>& out) {
    if (m_sockFd < 0) return false;
    uint8_t buf[65536];
    ssize_t n = ::recv(m_sockFd, buf, sizeof(buf), 0);
    if (n <= 0) return false;
    out.assign(buf, buf + n);
    return true;
}

bool Publisher::UdpDoSubscribe() {
    sfu::SubscribeFlags flags{};
    flags.audio    = true;   // 声明音频通道
    flags.video    = true;   // 声明视频通道
    flags.data     = true;   // 声明消息通道
    flags.reserved = 0;

    auto req = sfu::SerializeSubscribeReq(sfu::SubscribeRole::kPublisher, m_streamId, m_userId, flags);

    // 真实网络下请求或响应可能丢包（NAT 首包尤甚）；服务端繁忙时事件循环可能
    // 停滞十几秒才处理请求，因此重发最多 10 次（每次等待 3s）。
    // 等待期间可能先收到非响应数据报，需跳过继续等待。
    struct timeval shortTv;
    shortTv.tv_sec = 0;
    shortTv.tv_usec = 500 * 1000;
    setsockopt(m_sockFd, SOL_SOCKET, SO_RCVTIMEO, &shortTv, sizeof(shortTv));

    bool ok = false;
    for (int attempt = 1; attempt <= 10 && !ok; ++attempt) {
        if (!UdpSendAll(req.data(), req.size())) {
            std::cerr << "[Push] UDP 发送订阅请求失败" << std::endl;
            return false;
        }

        std::cout << "[Push] 已发送 UDP 注册请求 #" << attempt
                  << "，user=" << m_userId << " stream=" << m_streamId << std::endl;

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            std::vector<uint8_t> resp;
            if (!UdpRecvPacket(resp)) {
                continue;  // 超时或错误，继续等到 deadline
            }

            auto parsed = sfu::ParseSubscribeResp(resp.data(), resp.size());
            if (!parsed.valid) {
                continue;  // 非订阅响应包，跳过
            }
            if (parsed.status != 0) {
                std::cerr << "[Push] UDP 订阅失败: " << parsed.reason << std::endl;
                return false;
            }
            m_sessionId = parsed.sessionId;
            std::cout << "[Push] UDP 订阅成功，sessionId=" << m_sessionId << std::endl;
            ok = true;
            break;
        }

        if (!ok && attempt < 10) {
            std::cerr << "[Push] UDP 订阅响应超时，重试..." << std::endl;
        }
    }

    // 恢复接收超时
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(m_sockFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (!ok) {
        std::cerr << "[Push] UDP 读取订阅响应超时或连接断开" << std::endl;
    }
    return ok;
}

} // namespace push
