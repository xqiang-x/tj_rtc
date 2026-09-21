#include "SfuServer.h"
#include "SfuEvPool.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <csignal>
#include <getopt.h>
#include <unistd.h>
#include <iostream>
#include <glog/logging.h>

static sfu::SfuServerFull* g_server = nullptr;

// 信号处理器中只能调用 async-safe 函数（glog 不安全），用 write(2) 输出
static void SignalHandler(int sig) {
    const char msg[] = "\nReceived signal, shutting down...\n";
    ssize_t ignored = write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    (void)ignored;
    (void)sig;
    if (g_server) {
        g_server->Stop();
    }
    g_server = nullptr;
}

void PrintUsage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("\nOptions:\n");
    printf("  -h, --help              Show this help message\n");
    printf("  -i, --ip <addr>         Listen IP address (default: 0.0.0.0)\n");
    printf("  -p, --port <port>       TCP listen port (default: 9200)\n");
    printf("  -u, --udp-port <port>   Enable UDP on specified port\n");
    printf("  --stun-port <port>      STUN service port (default: udp_port + 1)\n");
    printf("  -m, --max-clients <n>   Max concurrent clients (default: 1024)\n");
    printf("  -s, --stats-interval <n> Stats print interval in seconds (default: 10, 0 to disable)\n");
    printf("  -t, --timeout <n>       Session timeout in seconds (default: 3600)\n");
    printf("  -v, --verbose           Enable verbose logging\n");
    printf("  -q, --quiet             Disable stats printing\n");
    printf("  --tcp-only              Disable UDP (default: TCP enabled)\n");
    printf("  --log-dir <dir>         glog output directory (default: /tmp)\n");
    printf("\nExamples:\n");
    printf("  %s                      # Start with defaults on port 9200 (TCP only)\n", prog);
    printf("  %s -u 9201              # Start TCP on 9200, UDP on 9201\n", prog);
    printf("  %s -p 9300 -u 9301 -v   # TCP 9300, UDP 9301, verbose\n", prog);
    printf("  %s --tcp-only -p 9300   # TCP only on port 9300\n", prog);
    printf("\nManagement commands (via signals during runtime):\n");
    printf("  SIGINT/SIGTERM          Shutdown server gracefully\n");
}

int main(int argc, char* argv[]) {
    // Default config
    std::string listen_ip = "0.0.0.0";
    int listen_port = 9200;
    uint32_t max_clients = 1024;
    int stats_interval = 10;
    int session_timeout = 3;  // 3 seconds default for UDP
    bool verbose = false;
    bool enable_stats = true;
    bool enable_tcp = true;
    bool enable_udp = true;
    int udp_port = 0;
    int stun_port = 0;
    std::string log_dir = "";

    // Parse command line arguments
    static struct option long_options[] = {
        {"help",          no_argument,       0, 'h'},
        {"ip",            required_argument, 0, 'i'},
        {"port",          required_argument, 0, 'p'},
        {"udp-port",      required_argument, 0, 'u'},
        {"stun-port",     required_argument, 0, 2},
        {"max-clients",   required_argument, 0, 'm'},
        {"stats-interval",required_argument, 0, 's'},
        {"timeout",       required_argument, 0, 't'},
        {"verbose",       no_argument,       0, 'v'},
        {"quiet",         no_argument,       0, 'q'},
        {"tcp-only",      no_argument,       0, 1},
        {"log-dir",       required_argument, 0, 3},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "hi:p:u:m:s:t:vq", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'h':
                PrintUsage(argv[0]);
                return 0;

            case 'i':
                listen_ip = optarg;
                break;

            case 'p':
                listen_port = atoi(optarg);
                if (listen_port <= 0 || listen_port > 65535) {
                    fprintf(stderr, "Invalid port: %s\n", optarg);
                    return 1;
                }
                break;

            case 'u':
                udp_port = atoi(optarg);
                if (udp_port <= 0 || udp_port > 65535) {
                    fprintf(stderr, "Invalid UDP port: %s\n", optarg);
                    return 1;
                }
                enable_udp = true;
                break;

            case 'm':
                max_clients = static_cast<uint32_t>(atoi(optarg));
                if (max_clients == 0) {
                    fprintf(stderr, "Invalid max-clients: %s\n", optarg);
                    return 1;
                }
                break;

            case 's':
                stats_interval = atoi(optarg);
                if (stats_interval < 0) {
                    fprintf(stderr, "Invalid stats-interval: %s\n", optarg);
                    return 1;
                }
                if (stats_interval == 0) {
                    enable_stats = false;
                }
                break;

            case 't':
                session_timeout = atoi(optarg);
                if (session_timeout <= 0) {
                    fprintf(stderr, "Invalid timeout: %s\n", optarg);
                    return 1;
                }
                break;

            case 'v':
                verbose = true;
                break;

            case 'q':
                enable_stats = false;
                break;

            case 1:  // --tcp-only
                enable_tcp = true;
                enable_udp = false;
                break;

            case 2:  // --stun-port
                stun_port = atoi(optarg);
                break;

            case 3:  // --log-dir
                log_dir = optarg;
                break;

            default:
                PrintUsage(argv[0]);
                return 1;
        }
    }

    // Initialize glog
    google::InitGoogleLogging(argv[0]);
    FLAGS_log_dir = log_dir;
    FLAGS_max_log_size = 100;  // 单个日志文件最大 100MB，超出自动轮转
    FLAGS_stderrthreshold = 3;  // 只把 FATAL 打到 stderr，INFO/WARNING/ERROR 全部落文件
    if (verbose) {
        FLAGS_v = 1;  // 打开 VLOG(1)/LogDebug
    }

    // Print startup banner
    LOG(INFO) << "========================================";
    LOG(INFO) << "  SFU Server v2.0 (SfuServerBase)";
    LOG(INFO) << "  Selective Forwarding Unit";
    LOG(INFO) << "========================================";

    // Start the event loop pool first
    LOG(INFO) << "Initializing event loop pool...";
    sfu::SfuEvPool::Instance().Start();
    LOG(INFO) << "Event loop pool started (" << sfu::SfuEvPool::Instance().GetThreadCount() << " threads)";

    // Setup signal handlers
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    signal(SIGPIPE, SIG_IGN);  // Ignore SIGPIPE for broken TCP connections

    // Create server config
    sfu::SfuServerFull::Config config;
    config.listen_ip = listen_ip;
    config.listen_port = listen_port;
    config.max_clients = max_clients;
    config.enable_tcp = enable_tcp;
    config.enable_udp = enable_udp;
    config.udp_port = udp_port;
    config.stun_port = stun_port;
    config.enable_stats_print = enable_stats;
    config.stats_interval_sec = stats_interval;
    config.session_timeout_sec = session_timeout;
    config.verbose = verbose;

    LOG(INFO) << "Configuration:";
    LOG(INFO) << "  TCP listen:        " << config.listen_ip << ":" << config.listen_port
              << (config.enable_tcp ? " (ON)" : " (OFF)");
    if (config.enable_udp) {
        LOG(INFO) << "  UDP listen:        " << config.listen_ip << ":"
                  << (config.udp_port > 0 ? config.udp_port : config.listen_port + 1);
        LOG(INFO) << "  STUN service:      " << config.listen_ip << ":"
                  << (config.stun_port > 0 ? config.stun_port :
                      (config.udp_port > 0 ? config.udp_port + 1 : config.listen_port + 2));
    } else {
        LOG(INFO) << "  UDP listen:        OFF";
    }
    LOG(INFO) << "  Max clients:       " << config.max_clients;
    LOG(INFO) << "  Stats interval:    " << (enable_stats ? std::to_string(stats_interval) + "s" : "disabled");
    LOG(INFO) << "  Session timeout:   " << config.session_timeout_sec << "s";
    LOG(INFO) << "  Verbose logging:   " << (config.verbose ? "enabled" : "disabled");
    LOG(INFO) << "  glog dir:          " << (log_dir.empty() ? "/tmp (default)" : log_dir);

    // Create and start server. The server must be destroyed before the event
    // loops are freed: ~SfuServerBase -> StopAll() still stops timers on the loop.
    int ret = 0;
    {
        sfu::SfuServerFull server(config);
        g_server = &server;

        LOG(INFO) << "Starting SFU server...";

        ret = server.Start();

        if (ret == 0) {
            // Server is running, keep g_server valid for signal handler;
            // SignalHandler sets g_server to nullptr after Stop() is called
            while (g_server) {
                sleep(1);
            }
        } else {
            LOG(ERROR) << "Server exited with error code: " << ret;
        }
        g_server = nullptr;
    }

    // Event loop pool is stopped after the server is fully destroyed
    sfu::SfuEvPool::Instance().Stop();

    if (ret != 0) {
        google::ShutdownGoogleLogging();
        return 1;
    }

    LOG(INFO) << "Server stopped gracefully.";
    google::ShutdownGoogleLogging();
    return 0;
}
