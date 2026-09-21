// Subscriber.cpp
// 纯 C++ TCP 拉流客户端：connect → subscribe → receive forwarded data

#include "Subscriber.h"

// SFU 协议头（仅使用 inline 函数，无需链接）
#include "SfuSubscribeProtocol.h"
#include "SfuProtocol.h"
#include "SfuP2PProtocol.h"
#include "SfuSockOpt.h"
#include "P2PManager.h"

// FEC 分片
#include "receiver.h"
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

namespace pull {

// SFU 转发的数据类型
static constexpr uint8_t kForwardDataCtrlType = 0x14;  // kForwardData

// ─────────────────────────────────────────────────────────────
Subscriber::Subscriber()  = default;
Subscriber::~Subscriber() { Stop(); }

// ─────────────────────────────────────────────────────────────
bool Subscriber::Start(const std::string& serverIp, int port,
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

    m_netThread = std::thread(&Subscriber::NetThreadFunc, this);
    return true;
}

fec_protocol::ProtocolStats Subscriber::GetStats() const {
    if (m_frameReceiver) return m_frameReceiver->getStats();
    return {};
}

void Subscriber::Stop() {
    if (!m_running.load()) {
        if (m_netThread.joinable()) {
            m_netThread.join();
        }
        m_ctrlRunning.store(false);
        if (m_ctrlRecvThread.joinable()) {
            m_ctrlRecvThread.join();
        }
        return;
    }

    m_running.store(false);
    m_ctrlRunning.store(false);

    // 关闭 socket 让 recv 返回
    TcpClose();

    // 关闭可靠信令 TCP 连接（先 shutdown 唤醒接收线程）
    if (m_ctrlTcpFd >= 0) {
        ::shutdown(m_ctrlTcpFd, SHUT_RDWR);
        ::close(m_ctrlTcpFd);
        m_ctrlTcpFd = -1;
    }

    if (m_netThread.joinable()) {
        m_netThread.join();
    }
    if (m_ctrlRecvThread.joinable()) {
        m_ctrlRecvThread.join();
    }
}

// ─────────────────────────────────────────────────────────────
// P2P 信令发送（通过 TCP 连接）
// ─────────────────────────────────────────────────────────────
bool Subscriber::SendP2PSignaling(const std::vector<uint8_t>& data) {
    if (m_sockFd < 0 || data.empty()) return false;
    return TcpSendAll(data.data(), data.size());
}

void Subscriber::ProcessExternalFec(const uint8_t* fecData, size_t fecLen) {
    if (!m_frameReceiver || !fecData || fecLen == 0) return;

    uint64_t nowUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    m_frameReceiver->onPacketReceived(fecData, fecLen, nowUs);
    m_frameReceiver->tick(nowUs);
}

// FEC 接收端初始化：内部帧按序提交；缺帧超过 2s 缓存期丢弃并请求关键帧（PLI），
// 避免旧策略下（无限等待）缺帧时视频长时间停顿
void Subscriber::InitFrameReceiver() {
    fec_protocol::ReceiverConfig recvCfg;
    recvCfg.frame_timeout_ms = 2000;       // 2s 缓存：超时丢弃并请求关键帧重建
    // NACK 重试上限与 2s 帧超时生命周期对齐：50ms 间隔 × 40 ≈ 2s，
    // 缺块超时后对应帧已丢弃，继续重试只会形成风暴
    recvCfg.max_nack_retries = 40;
    // pending 帧数受 2s 帧超时兜底约束（< 帧率×2s），且须远小于 int16 diff 回绕窗口
    // （±32768），否则帧序游标 diff 判断失效；4096 覆盖 375fps×2s≈750 帧的极端场景
    recvCfg.max_pending_frames = 4096;
    recvCfg.stats_interval_ms = 300;  // 300ms 反馈窗口，驱动推流端自适应码率
    m_frameReceiver = std::make_unique<fec_protocol::FrameReceiver>(recvCfg);

    // NACK/STATS 回传通道：封装为 SFU kStreamData，目标为推流端 sessionId，
    // 服务端按 sessionId 路由到该推流端的 ProxyReceiver
    m_frameReceiver->setSendCallback([this](const uint8_t* data, size_t len) {
        if (m_sourceSessionId == 0) return;  // 尚未收到任何推流端数据
        auto msg = sfu::SerializeStreamData(m_sourceSessionId, data, len);
        if (m_useUdp) {
            UdpSendAll(msg.data(), msg.size());
        } else {
            TcpSendAll(msg.data(), msg.size());
        }
    });

    m_frameReceiver->setFrameReadyCallback([this](fec_protocol::Frame&& frame) {
        // FEC 重组完成的完整帧，通过回调传递
        static int readyCount = 0;
        if (++readyCount % 100 == 0) {
            std::cout << "[Pull-FEC] Frame ready: size=" << frame.data.size()
                      << " type=" << static_cast<int>(frame.type) << std::endl;
        }

        // 根据 FEC frame.type 映射回推流端的 frameType
        // 推流端：VIDEO_PARAMS=0x01, VIDEO_IDR=0x02, VIDEO=0x03, AUDIO_PCM=0x10
        // FEC:    VIDEO_PARAMS=0x04, VIDEO_IDR=0x03, VIDEO=0x02, AUDIO=0x01
        uint8_t frameType = 0;
        if (frame.type == fec_protocol::frame_type::VIDEO_PARAMS) {
            frameType = 0x01;  // VIDEO_PARAMS
        } else if (frame.type == fec_protocol::frame_type::VIDEO_IDR) {
            frameType = 0x02;  // VIDEO_IDR
        } else if (frame.type == fec_protocol::frame_type::VIDEO) {
            frameType = 0x03;  // VIDEO_P
        } else if (frame.type == fec_protocol::frame_type::AUDIO) {
            frameType = 0x10;  // AUDIO_PCM
        } else {
            std::cout << "[Pull] Unknown FEC frame type: " << static_cast<int>(frame.type) << std::endl;
            return;
        }

        // 构造 [frameType][payload数据] 格式
        std::vector<uint8_t> payload;
        payload.reserve(1 + frame.data.size());
        payload.push_back(frameType);
        payload.insert(payload.end(), frame.data.begin(), frame.data.end());

        if (m_onFrame) m_onFrame(0, std::move(payload));
    });
}

// ─────────────────────────────────────────────────────────────
// 遥控信令：Sub → Pub 上游（upstream）
// ─────────────────────────────────────────────────────────────
void Subscriber::SendCtrlUpstream(const uint8_t* data, size_t len, bool reliable) {
    if (!m_running.load() || m_sourceSessionId == 0) return;

    // 构造客户端→服务器格式: [ctrlType][direction=0x01][publisherSessionId][payload]
    std::vector<uint8_t> msg(6 + len);
    msg[0] = reliable ? static_cast<uint8_t>(sfu::SfuCtrlType::kCtrlReliable)
                      : static_cast<uint8_t>(sfu::SfuCtrlType::kCtrlUnreliable);
    msg[1] = 0x01;  // upstream
    msg[2] = (m_sourceSessionId >> 24) & 0xFF;
    msg[3] = (m_sourceSessionId >> 16) & 0xFF;
    msg[4] = (m_sourceSessionId >> 8)  & 0xFF;
    msg[5] =  m_sourceSessionId        & 0xFF;
    if (len > 0) memcpy(msg.data() + 6, data, len);

    if (reliable) {
        // 可靠：走 TCP（带 4 字节长度前缀），帧首需带 SFU header 字节
        std::vector<uint8_t> tcpMsg(1 + msg.size());
        tcpMsg[0] = sfu::MakeHeader(sfu::PROTOCOL_VERSION, static_cast<uint8_t>(sfu::MsgType::kCmd));
        memcpy(tcpMsg.data() + 1, msg.data(), msg.size());
        if (m_useUdp && m_ctrlTcpFd >= 0) {
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
        // 不可靠：走 UDP（需加 SFU header byte）
        if (m_useUdp) {
            std::vector<uint8_t> udpMsg(1 + msg.size());
            udpMsg[0] = sfu::MakeHeader(sfu::PROTOCOL_VERSION, static_cast<uint8_t>(sfu::MsgType::kCmd));
            memcpy(udpMsg.data() + 1, msg.data(), msg.size());
            UdpSendAll(udpMsg.data(), udpMsg.size());
        } else {
            // TCP 模式无 UDP，降级为 TCP
            TcpSendAll(msg.data(), msg.size());
        }
    }
}

// ─────────────────────────────────────────────────────────────
// 解析接收到的控制信令并回调
// ─────────────────────────────────────────────────────────────
void Subscriber::DispatchControlMessage(const uint8_t* data, size_t len) {
    // data 格式: [header][ctrlType][direction][session_id(4)][payload]
    if (len < 8) return;
    uint8_t ctrlType = data[1];
    if (ctrlType != static_cast<uint8_t>(sfu::SfuCtrlType::kCtrlReliable) &&
        ctrlType != static_cast<uint8_t>(sfu::SfuCtrlType::kCtrlUnreliable)) return;

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
// 可靠信令 TCP 接收线程：读取 m_ctrlTcpFd 上的长度前缀帧
// ─────────────────────────────────────────────────────────────
void Subscriber::CtrlRecvThreadFunc() {
    std::cout << "[Pull-Ctrl] 信令接收线程启动" << std::endl;

    int tcpFd = m_ctrlTcpFd;
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
            if (ctrlType == static_cast<uint8_t>(sfu::SfuCtrlType::kCtrlReliable) ||
                ctrlType == static_cast<uint8_t>(sfu::SfuCtrlType::kCtrlUnreliable)) {
                DispatchControlMessage(frame.data(), frame.size());
            }
        }
    }

exit_loop:
    std::cout << "[Pull-Ctrl] 信令接收线程退出" << std::endl;
}

// ─────────────────────────────────────────────────────────────
// 网络线程：连接 → 订阅 → 循环接收转发数据
// ─────────────────────────────────────────────────────────────
void Subscriber::NetThreadFunc() {
    std::cout << "[Pull] 网络线程启动 (" << (m_useUdp ? "UDP" : "TCP") << ")" << std::endl;

    if (m_useUdp) {
        // ── UDP 路径 ──────────────────────────────────────────
        while (m_running.load()) {
            if (UdpConnect(m_serverIp, m_udpPort)) break;
            std::cerr << "[Pull] UDP 连接失败，3 秒后重试..." << std::endl;
            for (int i = 0; i < 30 && m_running.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!m_running.load()) return;

        if (!UdpDoSubscribe()) {
            std::cerr << "[Pull] UDP 订阅失败，退出" << std::endl;
            TcpClose();
            m_running.store(false);
            // 订阅失败（如流不存在）时带上服务器原因通知上层，否则 App 会一直停在"连接中"直至误报超时
            if (m_onSubscribeFailed) m_onSubscribeFailed(m_failReason);
            return;
        }

        // 建立 TCP 连接用于可靠信令
        {
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd >= 0) {
                struct timeval tv;
                tv.tv_sec = 3;
                tv.tv_usec = 0;
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

                struct sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(static_cast<uint16_t>(m_port));
                inet_pton(AF_INET, m_serverIp.c_str(), &addr.sin_addr);

                if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                    // 发送注册请求：纯控制会话，flags 全 false（不收媒体、不注册流，仅用于信令配对）
                    sfu::SubscribeFlags flags{};
                    flags.audio    = false;
                    flags.video    = false;
                    flags.data     = false;
                    flags.reserved = 0;
                    auto req = sfu::SerializeSubscribeReq(sfu::SubscribeRole::kSubscriber, m_streamId, m_userId, flags);
                    uint8_t prefix[4] = {
                        (uint8_t)(req.size() >> 24), (uint8_t)(req.size() >> 16),
                        (uint8_t)(req.size() >> 8),  (uint8_t)(req.size())
                    };
                    ::send(fd, prefix, 4, MSG_NOSIGNAL);
                    ::send(fd, req.data(), req.size(), MSG_NOSIGNAL);

                    // 读取响应
                    uint8_t respPrefix[4];
                    size_t got = 0;
                    while (got < 4) {
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
                            while (got < frameLen) {
                                ssize_t r = ::recv(fd, resp.data() + got, frameLen - got, 0);
                                if (r <= 0) break;
                                got += r;
                            }
                        }
                    }
                    m_ctrlTcpFd = fd;
                    std::cout << "[Pull] 可靠信令 TCP 连接已建立" << std::endl;

                    // 启动可靠信令接收线程
                    if (m_ctrlRecvThread.joinable()) m_ctrlRecvThread.join();
                    m_ctrlRunning.store(true);
                    m_ctrlRecvThread = std::thread([this]() { CtrlRecvThreadFunc(); });
                } else {
                    ::close(fd);
                    std::cerr << "[Pull] 可靠信令 TCP 连接失败: " << strerror(errno) << std::endl;
                }
            }
        }

        // FEC 接收端：内部帧按序提交；缺帧超 2s 缓存期丢弃并请求关键帧；
        // NACK/STATS 回传走 UDP 通道（InitFrameReceiver 内按 m_useUdp 分发）
        InitFrameReceiver();

        if (m_onConnected) m_onConnected(m_sessionId);
        std::cout << "[Pull] UDP 开始接收，sessionId=" << m_sessionId << std::endl;

        // UDP NAT 保活：运营商 NAT 映射通常 30~120s 无上行流量即过期，过期后
        // 服务器下行的媒体包会被 NAT 丢弃（表现为订阅成功但始终收不到数据）。
        // 每 1s 发一个携带自身 sessionId 的 kStreamData 空包：服务器 UdpRecvCb
        // 对 0x13 类型刷新会话活跃时间，HandleStreamData 对自身会话安全忽略。
        auto heartbeatNowUs = []() -> uint64_t {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        };
        uint64_t lastHeartbeatUs = heartbeatNowUs();
        constexpr uint64_t kHeartbeatIntervalUs = 1000 * 1000;  // 1s

        while (m_running.load()) {
            uint64_t nowUs = heartbeatNowUs();
            if (nowUs - lastHeartbeatUs >= kHeartbeatIntervalUs) {
                lastHeartbeatUs = nowUs;
                // [header][0x13 kStreamData][自身 sessionId] 空包：
                // 服务器 0x13 分支刷新会话活跃，HandleStreamData 对自身会话安全忽略
                auto hb = sfu::SerializeStreamData(m_sessionId, nullptr, 0);
                UdpSendAll(hb.data(), hb.size());
            }

            std::vector<uint8_t> pkt;
            if (UdpRecvPacket(pkt)) {
                // std::cout << "[Pull-DEBUG] UDP recv: " << pkt.size() << " bytes" << std::endl;
                
                // UDP 包格式：[SFU Header][ctrlType][sessionId][FEC分片包]
                // 先解析 SFU 协议
                if (pkt.size() < 6) {
                    // std::cout << "[Pull-DEBUG] Packet too small: " << pkt.size() << " bytes" << std::endl;
                    continue;  // 至少 header(1) + ctrlType(1) + sessionId(4)
                }
                
                uint8_t ctrlType = pkt[1];
                
                // 提取 sessionId（服务端按大端写入）
                uint32_t sessionId =
                    (static_cast<uint32_t>(pkt[2]) << 24) |
                    (static_cast<uint32_t>(pkt[3]) << 16) |
                    (static_cast<uint32_t>(pkt[4]) <<  8) |
                    (static_cast<uint32_t>(pkt[5]));
                
                // std::cout << "[Pull-DEBUG] ctrlType=0x" << std::hex << (int)ctrlType << std::dec 
                //           << " sessionId=" << sessionId << std::endl;
                
                // 处理 kForwardData (0x14) - 服务端转发的流数据
                if (ctrlType == 0x14) {
                    // 记录推流端 sessionId，供 NACK/STATS 回传路由
                    m_sourceSessionId = sessionId;

                    // payload 从第 6 字节开始（sessionId 之后）
                    const uint8_t* fecData = pkt.data() + 6;
                    size_t fecLen = pkt.size() - 6;

                    uint64_t nowUs = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());

                    // FEC 分片包交给 FEC 接收端重组
                    m_frameReceiver->onPacketReceived(fecData, fecLen, nowUs);

                    // 每 500 包打印一次 FEC 统计
                    static int pktCount = 0;
                    if (++pktCount % 500 == 0) {
                        auto stats = m_frameReceiver->getStats();
                        std::cout << "[Pull] FEC 接收统计: frames_completed=" << stats.frames_completed
                                  << " dropped=" << stats.frames_dropped
                                  << " fec_recovery_rate=" << stats.fec_recovery_rate
                                  << " nack_recovery_rate=" << stats.nack_recovery_rate << std::endl;
                    }

                    // 手动 PLI 请求：在网络线程内执行，保证 FrameReceiver 单线程访问
                    if (m_sourceSessionId != 0 && m_pliRequested.exchange(false)) {
                        std::cout << "[Pull] PLI requested - sending key frame request" << std::endl;
                        m_frameReceiver->requestKeyFrame(nowUs);
                    }

                    // tick 驱动 NACK/统计
                    m_frameReceiver->tick(nowUs);
                } else if (ctrlType == 0x13) {
                    // std::cout << "[Pull-DEBUG] Unexpected kStreamData (should be kForwardData)" << std::endl;
                } else if (ctrlType >= 0x17 && ctrlType <= 0x1C && m_p2pManager) {
                    // P2P 控制消息
                    switch (ctrlType) {
                        case 0x18:  // kP2PResponse
                            m_p2pManager->HandleP2PResponse(pkt.data() + 1, pkt.size() - 1);
                            break;
                        case 0x19:  // kP2PCandidate
                            m_p2pManager->HandleP2PCandidate(pkt.data() + 1, pkt.size() - 1);
                            break;
                        case 0x1A:  // kP2PConnectCheck
                            m_p2pManager->HandleP2PConnectCheck(pkt.data() + 1, pkt.size() - 1);
                            break;
                        case 0x1B:  // kP2PFallback
                            m_p2pManager->HandleP2PFallback(pkt.data() + 1, pkt.size() - 1);
                            break;
                        default:
                            break;
                    }
                } else if ((ctrlType == static_cast<uint8_t>(sfu::SfuCtrlType::kCtrlReliable) ||
                            ctrlType == static_cast<uint8_t>(sfu::SfuCtrlType::kCtrlUnreliable)) &&
                           pkt.size() >= 7) {
                    // 遥控信令
                    DispatchControlMessage(pkt.data(), pkt.size());
                } else {
                    // std::cout << "[Pull-DEBUG] Ignoring ctrlType=0x" << std::hex << (int)ctrlType << std::dec << std::endl;
                }
            }
            // 超时或错误：继续等待（不要退出循环）
        }
    } else {
        // ── TCP 路径 ──────────────────────────────────────────
        while (m_running.load()) {
            if (TcpConnect(m_serverIp, m_port)) break;
            std::cerr << "[Pull] 连接失败，3 秒后重试..." << std::endl;
            for (int i = 0; i < 30 && m_running.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!m_running.load()) return;

        if (!DoSubscribe()) {
            std::cerr << "[Pull] 订阅失败，退出" << std::endl;
            TcpClose();
            m_running.store(false);
            // 订阅失败（如流不存在）时带上服务器原因通知上层，否则 App 会一直停在"连接中"直至误报超时
            if (m_onSubscribeFailed) m_onSubscribeFailed(m_failReason);
            return;
        }

        // FEC 接收端：与 UDP 路径共用的初始化，NACK/STATS 回传走本 TCP 连接
        InitFrameReceiver();

        if (m_onConnected) m_onConnected(m_sessionId);
        std::cout << "[Pull] 开始接收数据，sessionId=" << m_sessionId << std::endl;

        while (m_running.load()) {
            std::vector<uint8_t> frame;
            if (!TcpRecvFrame(frame)) {
                if (m_running.load())
                    std::cerr << "[Pull] 接收失败，连接可能已断开" << std::endl;
                break;
            }
            if (frame.size() < 6) continue;
            uint8_t ctrlType = frame[1];
            if (ctrlType == kForwardDataCtrlType) {
                // 与 UDP 路径一致：转发分片交给 FEC 接收端重组，
                // 组装完成的完整帧经 setFrameReadyCallback 回调 m_onFrame
                uint32_t sourceSessionId =
                    (static_cast<uint32_t>(frame[2]) << 24) |
                    (static_cast<uint32_t>(frame[3]) << 16) |
                    (static_cast<uint32_t>(frame[4]) <<  8) |
                    (static_cast<uint32_t>(frame[5]));
                m_sourceSessionId = sourceSessionId;

                const uint8_t* fecData = frame.data() + 6;
                size_t fecLen = frame.size() - 6;

                uint64_t nowUs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());

                m_frameReceiver->onPacketReceived(fecData, fecLen, nowUs);

                // 手动 PLI 请求：在网络线程内执行，保证 FrameReceiver 单线程访问
                if (m_sourceSessionId != 0 && m_pliRequested.exchange(false)) {
                    std::cout << "[Pull] PLI requested - sending key frame request" << std::endl;
                    m_frameReceiver->requestKeyFrame(nowUs);
                }

                // tick 驱动 NACK/统计
                m_frameReceiver->tick(nowUs);
            } else if ((ctrlType == static_cast<uint8_t>(sfu::SfuCtrlType::kCtrlReliable) ||
                        ctrlType == static_cast<uint8_t>(sfu::SfuCtrlType::kCtrlUnreliable)) &&
                       frame.size() >= 7) {
                DispatchControlMessage(frame.data(), frame.size());
            }
        }

        TcpClose();
        if (m_onDisconnected) m_onDisconnected();
    }
    std::cout << "[Pull] 网络线程退出" << std::endl;
}

// ─────────────────────────────────────────────────────────────
// TCP 工具
// ─────────────────────────────────────────────────────────────
bool Subscriber::TcpConnect(const std::string& ip, int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "[Pull] socket() 失败: " << strerror(errno) << std::endl;
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
        std::cerr << "[Pull] connect(" << ip << ":" << port
                  << ") 失败: " << strerror(errno) << std::endl;
        ::close(fd);
        return false;
    }

    // 连接成功后取消所有超时：使用阻塞 I/O，recv 无限等待
    // （推流端可能延迟数秒才开始推流，不能用短超时）
    tv.tv_sec  = 0;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    m_sockFd = fd;
    std::cout << "[Pull] 已连接到 " << ip << ":" << port << std::endl;
    return true;
}

void Subscriber::TcpClose() {
    if (m_sockFd >= 0) {
        ::shutdown(m_sockFd, SHUT_RDWR);
        ::close(m_sockFd);
        m_sockFd = -1;
    }
}

// 带 4 字节大端长度前缀发送（与 sfu_server 的 TCP 帧格式一致）
bool Subscriber::TcpSendAll(const uint8_t* data, size_t len) {
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
bool Subscriber::TcpRecvAll(uint8_t* buf, size_t len) {
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
bool Subscriber::TcpRecvFrame(std::vector<uint8_t>& out) {
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
// 订阅协议
// ─────────────────────────────────────────────────────────────
bool Subscriber::DoSubscribe() {
    // 构造订阅请求：以订阅者角色订阅指定流（接收音频/视频/消息）
    sfu::SubscribeFlags flags{};
    flags.audio    = true;   // 接收音频
    flags.video    = true;   // 接收视频
    flags.data     = true;   // 接收消息
    flags.reserved = 0;

    auto req = sfu::SerializeSubscribeReq(sfu::SubscribeRole::kSubscriber, m_streamId, m_userId, flags);

    if (!TcpSendAll(req.data(), req.size())) {
        std::cerr << "[Pull] 发送订阅请求失败" << std::endl;
        return false;
    }

    std::cout << "[Pull] 已发送订阅请求，user=" << m_userId
              << " stream=" << m_streamId << std::endl;

    // 等待服务器回复
    std::vector<uint8_t> resp;
    if (!TcpRecvFrame(resp)) {
        std::cerr << "[Pull] 读取订阅响应超时或连接断开" << std::endl;
        m_failReason = "RESPONSE_TIMEOUT";
        return false;
    }

    auto parsed = sfu::ParseSubscribeResp(resp.data(), resp.size());
    if (!parsed.valid || parsed.status != 0) {
        std::cerr << "[Pull] 订阅失败: " << parsed.reason << std::endl;
        m_failReason = parsed.reason;
        return false;
    }

    m_sessionId = parsed.sessionId;
    std::cout << "[Pull] 订阅成功，sessionId=" << m_sessionId << std::endl;
    return true;
}

// ─────────────────────────────────────────────────────────────
// UDP 工具
// ─────────────────────────────────────────────────────────────
bool Subscriber::UdpConnect(const std::string& ip, int port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::cerr << "[Pull] UDP socket() 失败: " << strerror(errno) << std::endl;
        return false;
    }

    // 尽力把接收缓冲提到 8MB（非 root 时钳制到 rmem_max）
    int rcvbuf = sfu::SetUdpRecvBuffer(fd);
    std::cout << "[Pull] UDP RCVBUF target=8388608 actual=" << rcvbuf << " bytes" << std::endl;

    struct sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port   = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, ip.c_str(), &serverAddr.sin_addr);

    // connect UDP socket：绑定远端地址，使 send/recv 无需指定地址
    if (::connect(fd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "[Pull] UDP connect(" << ip << ":" << port
                  << ") 失败: " << strerror(errno) << std::endl;
        ::close(fd);
        return false;
    }

    // 接收超时 3 秒（订阅响应等待）
    struct timeval tv;
    tv.tv_sec  = 3;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    m_sockFd = fd;
    std::cout << "[Pull] UDP 已连接到 " << ip << ":" << port << std::endl;
    return true;
}

// UDP 发送（无长度前缀，一个包即一条消息）
bool Subscriber::UdpSendAll(const uint8_t* data, size_t len) {
    if (m_sockFd < 0) return false;
    ssize_t n = ::send(m_sockFd, data, len, 0);
    return (n == static_cast<ssize_t>(len));
}

// UDP 接收一个包（无长度前缀，recv 一次 = 一条消息）
bool Subscriber::UdpRecvPacket(std::vector<uint8_t>& out) {
    if (m_sockFd < 0) return false;
    uint8_t buf[65536];
    ssize_t n = ::recv(m_sockFd, buf, sizeof(buf), 0);
    if (n <= 0) return false;
    out.assign(buf, buf + n);
    return true;
}

// UDP 订阅握手
bool Subscriber::UdpDoSubscribe() {
    sfu::SubscribeFlags flags{};
    flags.audio    = true;   // 接收音频
    flags.video    = true;   // 接收视频
    flags.data     = true;   // 接收消息
    flags.reserved = 0;

    auto req = sfu::SerializeSubscribeReq(sfu::SubscribeRole::kSubscriber, m_streamId, m_userId, flags);

    // UDP 发送：[header=0x15][payload]（无 4 字节长度前缀）
    // SerializeSubscribeReq 已经包含了 header byte，直接发送。
    // 真实网络下请求或响应可能丢包；服务端繁忙时事件循环可能停滞十几秒才处理请求，
    // 因此重发最多 10 次（每次等待 3s）直到收到有效响应。
    // 注意：等待期间可能先收到转发媒体包等非响应数据报，需跳过继续等待。
    struct timeval shortTv;
    shortTv.tv_sec = 0;
    shortTv.tv_usec = 500 * 1000;
    setsockopt(m_sockFd, SOL_SOCKET, SO_RCVTIMEO, &shortTv, sizeof(shortTv));

    bool ok = false;
    for (int attempt = 1; attempt <= 10 && !ok; ++attempt) {
        if (!UdpSendAll(req.data(), req.size())) {
            std::cerr << "[Pull] UDP 发送订阅请求失败" << std::endl;
            return false;
        }

        std::cout << "[Pull] 已发送 UDP 订阅请求 #" << attempt
                  << "，user=" << m_userId << " stream=" << m_streamId << std::endl;

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            std::vector<uint8_t> resp;
            if (!UdpRecvPacket(resp)) {
                continue;  // 超时或错误，继续等到 deadline
            }

            auto parsed = sfu::ParseSubscribeResp(resp.data(), resp.size());
            if (!parsed.valid) {
                continue;  // 非订阅响应包（如转发媒体数据），跳过
            }
            if (parsed.status != 0) {
                std::cerr << "[Pull] UDP 订阅失败: " << parsed.reason << std::endl;
                m_failReason = parsed.reason;
                return false;
            }
            m_sessionId = parsed.sessionId;
            std::cout << "[Pull] UDP 订阅成功，sessionId=" << m_sessionId << std::endl;
            ok = true;
            break;
        }

        if (!ok && attempt < 10) {
            std::cerr << "[Pull] UDP 订阅响应超时，重试..." << std::endl;
        }
    }

    // 恢复接收超时
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(m_sockFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (!ok) {
        std::cerr << "[Pull] UDP 读取订阅响应超时或连接断开" << std::endl;
        m_failReason = "RESPONSE_TIMEOUT";
    }
    return ok;
}

} // namespace pull
