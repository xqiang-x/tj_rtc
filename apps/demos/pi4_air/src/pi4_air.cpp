// SPDX-License-Identifier: BSD-2-Clause
// pi4_air —— 树莓派 4 专用 air 端（摄像头采集 + H.264 硬编 + Pixhawk 串口 MAVLink 桥）
//
// 由 demos/raspberry_pi_pusher（视频）与 rtc_local_air（MAVLink 桥）合并而来：
//   libcamera(CSI/USB) --YUV420 DMABUF--> H264V4l2Encoder(/dev/video11)
//       --H.264 ES--> NaluSplitter --(SPS/PPS|IDR|P)--> Publisher --SFU--> 手机(QOpenHD)
//   /dev/ttyACM0(Pixhawk) --MAVLink--> SendCtrlDownstream --> 手机
//   手机 --SendCtrlUpstream--> SetOnControlData --> 写回 /dev/ttyACM0
//
// 串口热插拔：Pixhawk 未插入时照常推视频；插入后 ~1s 内自动挂接；拔出自动摘除。
//
// 构建：cmake -S demos/pi4_air -B demos/pi4_air/build && cmake --build demos/pi4_air/build -j
// 运行：./pi4_air --stream openhd0
// 注意：不能与本地 rtc_local_air 同时运行（同一 stream 重复注册会互相顶掉）。

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Publisher.h"

#include "H264V4l2Encoder.h"
#include "LibcameraVideoSource.h"
#include "NaluSplitter.h"

namespace {

std::atomic<bool> g_running{true};

void handle_signal(int /*sig*/) { g_running.store(false); }

// ── MAVLink 最小分帧（v1 0xFE / v2 0xFD；不做 CRC，仅分帧与统计）──
constexpr size_t kMavResync = static_cast<size_t>(-1);

// 返回完整帧长；0 = 需要更多字节；kMavResync = 非帧首字节（丢弃 1B 重同步）
size_t mav_frame_len(const uint8_t* p, size_t n) {
  if (n < 1) return 0;
  if (p[0] == 0xFD) {  // v2
    if (n < 3) return 0;
    size_t fl = 10 + static_cast<size_t>(p[1]) + 2;
    if ((p[2] & 0x01) != 0) fl += 13;  // incompat_flags 含签名
    return n < fl ? 0 : fl;
  }
  if (p[0] == 0xFE) {  // v1
    if (n < 2) return 0;
    size_t fl = 6 + static_cast<size_t>(p[1]) + 2;
    return n < fl ? 0 : fl;
  }
  return kMavResync;
}

uint32_t mav_msgid(const uint8_t* p) {
  if (p[0] == 0xFD) {
    return static_cast<uint32_t>(p[7]) | (static_cast<uint32_t>(p[8]) << 8) |
           (static_cast<uint32_t>(p[9]) << 16);
  }
  return p[5];
}

uint8_t mav_sysid(const uint8_t* p) { return p[0] == 0xFD ? p[5] : p[3]; }

struct Options {
  // 视频
  unsigned int width = 1280;
  unsigned int height = 720;
  unsigned int fps = 30;
  unsigned int bitrate_kbps = 2500;
  unsigned int gop = 30;
  // SFU
  std::string server = "106.15.177.248";
  int server_port = 9200;
  std::string stream_id = "openhd0";
  std::string user_id = "pi4_air";
  bool use_udp = true;
  uint64_t pacer_mbps = 20;
  // MAVLink 串口
  std::string serial = "/dev/ttyACM0";
  int baud = 57600;  // USB CDC 忽略该值
  bool no_upstream = false;
  // 杂项
  int seconds = 0;  // 0 = 无限
  bool verbose = false;
};

void print_usage(const char* prog) {
  std::fprintf(stderr,
               "Usage: %s [options]\n"
               " 视频:\n"
               "  --width W         采集宽 (默认 1280)\n"
               "  --height H        采集高 (默认 720)\n"
               "  --fps  N          帧率 (默认 30)\n"
               "  --bitrate KBPS    编码码率 kbps (默认 2500)\n"
               "  --gop  N          GOP 长度 (默认 30)\n"
               " SFU:\n"
               "  --server IP       SFU 服务器 (默认 106.15.177.248)\n"
               "  --port  PORT      SFU 端口 (默认 9200)\n"
               "  --tcp             使用 TCP 推流 (默认 UDP)\n"
               "  --stream  ID      流名 (默认 openhd0，与手机订阅一致)\n"
               "  --user  ID        用户 ID (默认 pi4_air)\n"
               "  --bandwidth MBPS  UDP Pacer 带宽 Mbps (默认 20)\n"
               " MAVLink:\n"
               "  --serial  PATH    Pixhawk 串口 (默认 /dev/ttyACM0，热插拔)\n"
               "  --baud  N         串口波特率 (默认 57600)\n"
               "  --no-upstream     只记录上行 ctrl，不写串口\n"
               " 杂项:\n"
               "  --seconds N       运行秒数, 0=无限 (默认 0)\n"
               "  --verbose         打印每帧/每条 MAVLink 统计\n"
               "  -h, --help        显示本帮助\n",
               prog);
}

bool parse_args(int argc, char** argv, Options& opt) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "缺少 %s 的参数\n", name);
        return nullptr;
      }
      return argv[++i];
    };
    if (a == "-h" || a == "--help") {
      print_usage(argv[0]);
      return false;
    } else if (a == "--width") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.width = std::stoul(v);
    } else if (a == "--height") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.height = std::stoul(v);
    } else if (a == "--fps") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.fps = std::stoul(v);
    } else if (a == "--bitrate") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.bitrate_kbps = std::stoul(v);
    } else if (a == "--gop") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.gop = std::stoul(v);
    } else if (a == "--server") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.server = v;
    } else if (a == "--port") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.server_port = std::stoi(v);
    } else if (a == "--tcp") {
      opt.use_udp = false;
    } else if (a == "--stream") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.stream_id = v;
    } else if (a == "--user") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.user_id = v;
    } else if (a == "--bandwidth") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.pacer_mbps = std::stoull(v);
    } else if (a == "--serial") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.serial = v;
    } else if (a == "--baud") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.baud = std::stoi(v);
    } else if (a == "--no-upstream") {
      opt.no_upstream = true;
    } else if (a == "--seconds") {
      auto v = need(a.c_str()); if (v == nullptr) return false;
      opt.seconds = std::stoi(v);
    } else if (a == "--verbose") {
      opt.verbose = true;
    } else {
      std::fprintf(stderr, "未知参数: %s\n", a.c_str());
      print_usage(argv[0]);
      return false;
    }
  }
  return true;
}

speed_t baud_to_speed(int baud) {
  switch (baud) {
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default:     return B57600;
  }
}

int open_serial(const std::string& path, int baud) {
  const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) return -1;
  struct termios tio{};
  if (::tcgetattr(fd, &tio) != 0) { ::close(fd); return -1; }
  ::cfmakeraw(&tio);
  const speed_t sp = baud_to_speed(baud);
  ::cfsetispeed(&tio, sp);
  ::cfsetospeed(&tio, sp);
  tio.c_cflag |= CLOCAL | CREAD;
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 0;
  if (::tcsetattr(fd, TCSANOW, &tio) != 0) { ::close(fd); return -1; }
  return fd;
}

// 串口句柄：读线程负责按需打开/关闭（USB 热插拔），
// SDK 网络线程只经 write() 写入，二者由 mu 串行化。
struct SerialLink {
  SerialLink() = default;
  SerialLink(std::string p, int b) : path(std::move(p)), baud(b) {}

  std::string path;
  int baud = 57600;
  std::mutex mu;
  int fd = -1;  // 受 mu 保护
  std::atomic<bool> present{false};

  bool ensure_open() {
    std::lock_guard<std::mutex> lk(mu);
    if (fd < 0) fd = open_serial(path, baud);
    present.store(fd >= 0);
    return fd >= 0;
  }
  void close_link() {
    std::lock_guard<std::mutex> lk(mu);
    if (fd >= 0) ::close(fd);
    fd = -1;
    present.store(false);
  }
  int get() {
    std::lock_guard<std::mutex> lk(mu);
    return fd;
  }
  // 供 SDK 网络线程调用；返回写入字节数，-1 = 串口未就绪
  ssize_t write_upstream(const uint8_t* d, size_t n) {
    std::lock_guard<std::mutex> lk(mu);
    if (fd < 0) return -1;
    return ::write(fd, d, n);
  }
};

struct MavStats {
  std::atomic<uint64_t> tx_msgs{0};    // FC → 手机（下行）
  std::atomic<uint64_t> tx_bytes{0};
  std::atomic<uint64_t> hb{0};         // 下行 msgid=0
  std::atomic<uint64_t> rx_msgs{0};    // 手机 → FC（上行）
  std::atomic<uint64_t> rx_bytes{0};
  std::atomic<uint64_t> up_write{0};   // 实际写串口字节
  std::atomic<uint64_t> up_drop{0};    // 串口未就绪丢弃的上行条数
  std::atomic<uint64_t> resync{0};
  std::mutex mu;
  std::map<uint32_t, uint64_t> tx_ids;
  std::map<uint32_t, uint64_t> rx_ids;
};

// 首次出现的 msgid 记一条日志，之后仅计数（--verbose 时全打）
void log_new_id(std::map<uint32_t, uint64_t>& ids, const char* dir, uint32_t id,
                uint8_t sysid, size_t len, bool verbose) {
  const uint64_t c = ++ids[id];
  if (c == 1 || verbose) {
    std::printf("[pi4] %s MAVLink msgid=%u sysid=%u len=%zu (第 %llu 条)\n",
                dir, id, sysid, len, static_cast<unsigned long long>(c));
    std::fflush(stdout);
  }
}

void print_top_ids(const std::map<uint32_t, uint64_t>& ids, const char* dir) {
  std::vector<std::pair<uint64_t, uint32_t>> v;
  v.reserve(ids.size());
  for (const auto& kv : ids) v.emplace_back(kv.second, kv.first);
  std::sort(v.begin(), v.end(), std::greater<>());
  std::printf("[pi4] %s msgid 分布 (top %zu): ", dir, v.size() > 8 ? 8 : v.size());
  for (size_t i = 0; i < v.size() && i < 8; ++i) {
    std::printf("%u×%llu%s", v[i].second,
                static_cast<unsigned long long>(v[i].first),
                (i + 1 < v.size() && i + 1 < 8) ? " " : "");
  }
  std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_args(argc, argv, opt)) return 1;

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  std::signal(SIGPIPE, SIG_IGN);

  SerialLink link{opt.serial, opt.baud};

  // ── 统计量 ──────────────────────────────────────────────
  std::atomic<bool> connected{false};
  std::atomic<uint64_t> enc_frames{0};   // 编码器输出次数（≈采集帧数）
  std::atomic<uint64_t> enc_bytes{0};    // 编码输出总字节
  std::atomic<uint64_t> enc_drop{0};     // 编码器入队失败丢弃的采集帧
  std::atomic<uint64_t> n_aus{0};        // 推送的 AU 数
  std::atomic<uint64_t> n_key{0};
  std::atomic<uint64_t> n_param{0};
  std::atomic<uint64_t> n_pli{0};
  std::atomic<bool> enc_ready{false};
  MavStats mav;

  // ── SDK Publisher ───────────────────────────────────────
  push::Publisher pub;
  pub.SetBandwidth(opt.pacer_mbps * 1'000'000ULL);
  pub.SetOnConnected([&connected](uint32_t sid) {
    connected.store(true);
    std::printf("[pi4] CONNECTED sessionId=%u\n", sid);
    std::fflush(stdout);
  });
  pub.SetOnDisconnected([] {
    std::printf("[pi4] DISCONNECTED (SDK 将自动重连)\n");
    std::fflush(stdout);
  });
  // UDP 接收线程内触发，须非阻塞：只计数；IDR 由编码器 GOP 周期自然满足
  pub.SetOnKeyFrameRequest([&n_pli] { n_pli.fetch_add(1); });
  pub.SetOnControlData(
      [&](uint32_t source_session, const uint8_t* data, size_t len) {
        // SDK 网络线程回调：手机上行命令（完整 MAVLink 帧，可能多帧一条消息）
        mav.rx_msgs.fetch_add(1);
        mav.rx_bytes.fetch_add(len);
        size_t off = 0;
        while (off < len) {
          const size_t fl = mav_frame_len(data + off, len - off);
          if (fl == 0 || fl == kMavResync) {
            std::fprintf(stderr, "[pi4][warn] 上行 ctrl 非 MAVLink 帧 (len=%zu)\n",
                         len - off);
            break;
          }
          const uint32_t id = mav_msgid(data + off);
          const uint8_t sid = mav_sysid(data + off);
          {
            std::lock_guard<std::mutex> lk(mav.mu);
            log_new_id(mav.rx_ids, "上行", id, sid, fl, opt.verbose);
          }
          off += fl;
        }
        if (!opt.no_upstream) {
          const ssize_t w = link.write_upstream(data, len);
          if (w < 0) {
            if (mav.up_drop.fetch_add(1) == 0) {
              std::fprintf(stderr,
                           "[pi4][warn] 串口未就绪，上行 MAVLink 丢弃 (Pixhawk 未插入?)\n");
            }
          } else {
            mav.up_write.fetch_add(static_cast<uint64_t>(w));
            if (static_cast<size_t>(w) != len) {
              std::fprintf(stderr, "[pi4][warn] 串口部分写入 %zd/%zu\n", w, len);
            }
          }
        }
        (void)source_session;
      });

  if (!pub.Start(opt.server, opt.server_port, opt.stream_id, opt.user_id,
                 opt.use_udp, 0)) {
    std::fprintf(stderr, "[pi4] Publisher Start 失败\n");
    return 1;
  }

  // ── 串口读线程：Pixhawk → 手机（含热插拔重试）───────────
  std::thread serial_thread([&] {
    std::vector<uint8_t> buf;
    buf.reserve(4096);
    std::vector<uint8_t> chunk(512);
    bool announced_absent = false;
    while (g_running.load()) {
      if (!link.ensure_open()) {
        if (!announced_absent) {
          std::printf("[pi4] 等待串口 %s (Pixhawk 未插入不影响视频推流)\n",
                      opt.serial.c_str());
          std::fflush(stdout);
          announced_absent = true;
        }
        for (int i = 0; i < 10 && g_running.load(); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        continue;
      }
      if (announced_absent) {
        std::printf("[pi4] 串口 %s 已挂接，开始 MAVLink 桥接\n", opt.serial.c_str());
        std::fflush(stdout);
        announced_absent = false;
      }

      const int fd = link.get();
      struct pollfd pfd{};
      pfd.fd = fd;
      pfd.events = POLLIN;
      const int pr = ::poll(&pfd, 1, 200);
      if (pr < 0) {
        if (errno == EINTR) continue;
        link.close_link();
        buf.clear();
        continue;
      }
      if (pr == 0) continue;
      if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        std::printf("[pi4] 串口 %s 断开，等待重新插入\n", opt.serial.c_str());
        std::fflush(stdout);
        link.close_link();
        buf.clear();
        // 退避：设备节点尚在时会立即重开成功，避免空转
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        continue;
      }

      const ssize_t n = ::read(fd, chunk.data(), chunk.size());
      if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) continue;
        std::fprintf(stderr, "[pi4][warn] 读串口失败: %s，复位重开\n",
                     std::strerror(errno));
        link.close_link();
        buf.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        continue;
      }
      if (n == 0) continue;
      buf.insert(buf.end(), chunk.begin(), chunk.begin() + n);
      size_t off = 0;
      while (off < buf.size()) {
        const size_t fl = mav_frame_len(buf.data() + off, buf.size() - off);
        if (fl == 0) break;  // 半帧，等更多字节
        if (fl == kMavResync) {
          mav.resync.fetch_add(1);
          ++off;
          continue;
        }
        const uint32_t id = mav_msgid(buf.data() + off);
        const uint8_t sid = mav_sysid(buf.data() + off);
        mav.tx_msgs.fetch_add(1);
        mav.tx_bytes.fetch_add(fl);
        if (id == 0) mav.hb.fetch_add(1);
        {
          std::lock_guard<std::mutex> lk(mav.mu);
          log_new_id(mav.tx_ids, "下行", id, sid, fl, opt.verbose);
        }
        pub.SendCtrlDownstream(buf.data() + off, fl, /*reliable=*/false);
        off += fl;
      }
      if (off > 0) buf.erase(buf.begin(), buf.begin() + off);
      if (buf.size() > 8 * 1024) {  // 防呆：异常流不无限堆积
        std::fprintf(stderr, "[pi4][warn] 串口缓冲 %zuB 无有效帧，清空\n",
                     buf.size());
        buf.clear();
      }
    }
  });

  // ── 视频管线：libcamera → V4L2 硬编 → NAL 拆分 → Publisher ──
  H264V4l2Encoder encoder;
  NaluSplitter splitter;

  splitter.SetOnNalu([&](NaluSplitter::Kind kind, const uint8_t* p, std::size_t n) {
    const bool is_param = (kind == NaluSplitter::Kind::kParams);
    const bool is_key = (kind == NaluSplitter::Kind::kIdr);
    n_aus.fetch_add(1);
    if (is_key) n_key.fetch_add(1);
    if (is_param) n_param.fetch_add(1);
    if (opt.verbose) {
      std::printf("[pi4] AU #%llu %zuB param=%d key=%d\n",
                  static_cast<unsigned long long>(n_aus.load()), n,
                  is_param ? 1 : 0, is_key ? 1 : 0);
    }
    pub.PushVideoFrame(p, n, is_param, is_key);
  });

  encoder.SetOnOutput([&](const uint8_t* data, std::size_t size,
                          int64_t /*ts_us*/, bool /*keyframe*/) {
    // 一次编码输出含完整一帧（可含 SPS/PPS + 多 slice），交给拆分器
    splitter.Process(data, size);
    enc_frames.fetch_add(1);
    enc_bytes.fetch_add(size);
    if (opt.verbose) {
      std::printf("[pi4] enc #%llu %zuB\n",
                  static_cast<unsigned long long>(enc_frames.load()), size);
    }
  });

  LibcameraVideoSource source;
  source.SetOnFrame([&](const LibcameraVideoSource::Frame& f) {
    if (!enc_ready.load()) return;  // 编码器尚未启动的极短窗口
    if (!encoder.EncodeBuffer(f.dmabuf_fd, f.size, f.timestamp_us)) {
      enc_drop.fetch_add(1);  // 编码器输入队列满，丢弃
    }
  });

  LibcameraVideoSource::Config src_cfg;
  src_cfg.width = opt.width;
  src_cfg.height = opt.height;
  src_cfg.fps = opt.fps;
  src_cfg.buffer_count = 6;
  if (!source.Start(src_cfg)) {
    std::fprintf(stderr, "[pi4] 相机启动失败 (libcamera)\n");
    g_running.store(false);
    if (serial_thread.joinable()) serial_thread.join();
    pub.Stop();
    return 1;
  }

  H264V4l2Encoder::Config enc_cfg;
  enc_cfg.width = source.Width();
  enc_cfg.height = source.Height();
  enc_cfg.stride = source.Stride();
  enc_cfg.fps = opt.fps;
  enc_cfg.bitrate_bps = opt.bitrate_kbps * 1000U;
  enc_cfg.intra_period = opt.gop;
  enc_cfg.rec709_colorspace = (opt.height >= 720);
  if (!encoder.Start(enc_cfg)) {
    std::fprintf(stderr, "[pi4] 编码器启动失败 (/dev/video11)\n");
    source.Stop();
    g_running.store(false);
    if (serial_thread.joinable()) serial_thread.join();
    pub.Stop();
    return 1;
  }
  enc_ready.store(true);

  std::printf(
      "[pi4] 树莓派 air 启动: stream=%s user=%s -> SFU %s:%d (%s)\n"
      "[pi4] 视频: libcamera %ux%u@%u (实际 %ux%u stride=%u) -> H.264 %u kbps gop=%u\n"
      "[pi4] MAVLink: %s @%d (%s)，串口热插拔已启用\n",
      opt.stream_id.c_str(), opt.user_id.c_str(), opt.server.c_str(),
      opt.server_port, opt.use_udp ? "UDP" : "TCP", opt.width, opt.height,
      opt.fps, source.Width(), source.Height(), source.Stride(),
      opt.bitrate_kbps, opt.gop, opt.serial.c_str(), opt.baud,
      opt.no_upstream ? "只记录上行不写 FC" : "上下行都写 FC");
  std::fflush(stdout);

  // ── 主循环：统计打印与退出判定 ──────────────────────────
  const bool has_deadline = opt.seconds > 0;
  const auto t0 = std::chrono::steady_clock::now();
  const auto deadline = t0 + std::chrono::seconds(opt.seconds);
  auto last_print = t0;
  uint64_t last_frames = 0;
  uint64_t last_aus = 0;
  uint64_t last_bytes = 0;
  uint64_t last_tx_msgs = 0;
  uint64_t last_rx_msgs = 0;
  uint64_t last_pli_seen = 0;

  while (g_running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (has_deadline && std::chrono::steady_clock::now() >= deadline) break;

    const uint64_t pli = n_pli.load();
    if (pli != last_pli_seen) {
      std::printf(
          "[pi4] 收到 PLI 关键帧请求 (累计 %llu) —— 由 GOP 周期(%.1fs)自然出 IDR\n",
          static_cast<unsigned long long>(pli),
          static_cast<double>(opt.gop) / static_cast<double>(opt.fps));
      last_pli_seen = pli;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - last_print >= std::chrono::seconds(1)) {
      const double dt =
          std::chrono::duration<double>(now - last_print).count();
      const uint64_t frames = enc_frames.load();
      const uint64_t aus = n_aus.load();
      const uint64_t bytes = enc_bytes.load();
      const uint64_t txm = mav.tx_msgs.load();
      const uint64_t rxm = mav.rx_msgs.load();
      std::printf(
          "[pi4] 1s: enc=%llu(+%llu) fps=%.1f aus=%llu(+%llu) key=%llu "
          "param=%llu kbps=%.0f drop=%llu mav_tx=%llu(+%llu hb=%llu) "
          "mav_rx=%llu(+%llu) up_write=%llu up_drop=%llu resync=%llu "
          "pli=%llu serial=%s\n",
          static_cast<unsigned long long>(frames),
          static_cast<unsigned long long>(frames - last_frames),
          static_cast<double>(frames - last_frames) / dt,
          static_cast<unsigned long long>(aus),
          static_cast<unsigned long long>(aus - last_aus),
          static_cast<unsigned long long>(n_key.load()),
          static_cast<unsigned long long>(n_param.load()),
          static_cast<double>(bytes - last_bytes) * 8.0 / dt / 1000.0,
          static_cast<unsigned long long>(enc_drop.load()),
          static_cast<unsigned long long>(txm),
          static_cast<unsigned long long>(txm - last_tx_msgs),
          static_cast<unsigned long long>(mav.hb.load()),
          static_cast<unsigned long long>(rxm),
          static_cast<unsigned long long>(rxm - last_rx_msgs),
          static_cast<unsigned long long>(mav.up_write.load()),
          static_cast<unsigned long long>(mav.up_drop.load()),
          static_cast<unsigned long long>(mav.resync.load()),
          static_cast<unsigned long long>(pli),
          link.present.load() ? "OK" : "无");
      std::fflush(stdout);
      last_print = now;
      last_frames = frames;
      last_aus = aus;
      last_bytes = bytes;
      last_tx_msgs = txm;
      last_rx_msgs = rxm;
    }
  }

  // ── 收尾 ────────────────────────────────────────────────
  std::printf("[pi4] 正在停止...\n");
  encoder.Stop();
  source.Stop();
  g_running.store(false);
  if (serial_thread.joinable()) serial_thread.join();
  pub.Stop();

  std::printf(
      "[pi4] 结束: enc=%llu bytes=%llu aus=%llu key=%llu param=%llu drop=%llu "
      "mav_tx=%llu(hb=%llu) mav_rx=%llu up_write=%llu up_drop=%llu "
      "resync=%llu pli=%llu\n",
      static_cast<unsigned long long>(enc_frames.load()),
      static_cast<unsigned long long>(enc_bytes.load()),
      static_cast<unsigned long long>(n_aus.load()),
      static_cast<unsigned long long>(n_key.load()),
      static_cast<unsigned long long>(n_param.load()),
      static_cast<unsigned long long>(enc_drop.load()),
      static_cast<unsigned long long>(mav.tx_msgs.load()),
      static_cast<unsigned long long>(mav.hb.load()),
      static_cast<unsigned long long>(mav.rx_msgs.load()),
      static_cast<unsigned long long>(mav.up_write.load()),
      static_cast<unsigned long long>(mav.up_drop.load()),
      static_cast<unsigned long long>(mav.resync.load()),
      static_cast<unsigned long long>(n_pli.load()));
  {
    std::lock_guard<std::mutex> lk(mav.mu);
    print_top_ids(mav.tx_ids, "下行 FC→手机");
    print_top_ids(mav.rx_ids, "上行 手机→FC");
  }
  return 0;
}