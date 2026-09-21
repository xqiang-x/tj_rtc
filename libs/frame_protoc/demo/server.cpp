/**
 * FEC Protocol Demo - Server (Receiver)
 *
 * Listens on a UDP port, receives encoded video packets through fec_protocol,
 * decodes them with ffmpeg, and displays with SDL2.
 *
 * Usage: ./fec_demo_server <port>
 */

#include "receiver.h"
#include "sender.h"
#include "types.h"
#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <string>

// Networking
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

// FFmpeg
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

// SDL2
#include <SDL2/SDL.h>

static uint64_t now_us() {
    auto tp = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(
        tp.time_since_epoch()).count();
}

// Global dump file for received frames
static FILE* g_dump_file = NULL;

// Global state for SDL display
struct DisplayContext {
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
    int width;
    int height;
    bool initialized;
};

struct DecoderContext {
    const AVCodec* codec;
    AVCodecContext* codec_ctx;
    AVFrame* frame;
    AVPacket* pkt;
    SwsContext* sws_ctx;
    bool initialized;
    std::vector<uint8_t> extradata;
};

static DisplayContext g_display = {NULL, NULL, NULL, 0, 0, false};
static DecoderContext g_decoder = {NULL, NULL, NULL, NULL, NULL, false, {}};

static bool init_decoder(AVCodecID codec_id, const uint8_t* extradata, int extradata_size) {
    g_decoder.codec = avcodec_find_decoder(codec_id);
    if (!g_decoder.codec) {
        fprintf(stderr, "[Server] Codec not found: %d\n", codec_id);
        return false;
    }

    g_decoder.codec_ctx = avcodec_alloc_context3(g_decoder.codec);
    if (!g_decoder.codec_ctx) {
        fprintf(stderr, "[Server] Failed to allocate codec context\n");
        return false;
    }

    if (extradata && extradata_size > 0) {
        g_decoder.codec_ctx->extradata = (uint8_t*)av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(g_decoder.codec_ctx->extradata, extradata, extradata_size);
        g_decoder.codec_ctx->extradata_size = extradata_size;
    }

    if (avcodec_open2(g_decoder.codec_ctx, g_decoder.codec, NULL) < 0) {
        fprintf(stderr, "[Server] Failed to open codec\n");
        return false;
    }

    g_decoder.frame = av_frame_alloc();
    g_decoder.pkt = av_packet_alloc();
    g_decoder.initialized = true;

    printf("[Server] Decoder initialized: %s\n", g_decoder.codec->name);
    return true;
}

static bool init_display(int width, int height) {
    if (g_display.initialized && g_display.width == width && g_display.height == height) {
        return true;
    }

    if (!g_display.initialized) {
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            fprintf(stderr, "[Server] SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }
    }

    if (g_display.texture) SDL_DestroyTexture(g_display.texture);
    if (g_display.renderer) SDL_DestroyRenderer(g_display.renderer);
    if (g_display.window) SDL_DestroyWindow(g_display.window);

    g_display.window = SDL_CreateWindow("FEC Protocol Demo - Server",
                                        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        width, height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!g_display.window) {
        fprintf(stderr, "[Server] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    g_display.renderer = SDL_CreateRenderer(g_display.window, -1,
                                            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_display.renderer) {
        fprintf(stderr, "[Server] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }

    g_display.texture = SDL_CreateTexture(g_display.renderer,
                                          SDL_PIXELFORMAT_IYUV,
                                          SDL_TEXTUREACCESS_STREAMING,
                                          width, height);
    if (!g_display.texture) {
        fprintf(stderr, "[Server] SDL_CreateTexture failed: %s\n", SDL_GetError());
        return false;
    }

    g_display.width = width;
    g_display.height = height;
    g_display.initialized = true;

    printf("[Server] Display initialized: %dx%d\n", width, height);
    return true;
}

static void display_frame(AVFrame* frame) {
    if (!init_display(frame->width, frame->height)) {
        return;
    }

    // Update texture with YUV data
    SDL_UpdateYUVTexture(g_display.texture, NULL,
                         frame->data[0], frame->linesize[0],
                         frame->data[1], frame->linesize[1],
                         frame->data[2], frame->linesize[2]);

    SDL_RenderClear(g_display.renderer);
    SDL_RenderCopy(g_display.renderer, g_display.texture, NULL, NULL);
    SDL_RenderPresent(g_display.renderer);
}

static void decode_and_display(const uint8_t* data, size_t size) {
    if (!g_decoder.initialized) {
        return;
    }

    g_decoder.pkt->data = const_cast<uint8_t*>(data);
    g_decoder.pkt->size = (int)size;

    int ret = avcodec_send_packet(g_decoder.codec_ctx, g_decoder.pkt);
    if (ret < 0) {
        fprintf(stderr, "[Server] avcodec_send_packet error: %d\n", ret);
        return;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(g_decoder.codec_ctx, g_decoder.frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            fprintf(stderr, "[Server] avcodec_receive_frame error: %d\n", ret);
            break;
        }

        display_frame(g_decoder.frame);
    }
}

static void on_frame_ready(fec_protocol::Frame&& frame) {
    static uint64_t frame_count = 0;

    // Dump frame to file: [4B frame_index][1B type][4B size][data]
    if (g_dump_file) {
        uint32_t idx = static_cast<uint32_t>(frame_count);
        uint8_t type_byte = frame.type;
        uint32_t size = frame.size;
        fwrite(&idx, 4, 1, g_dump_file);
        fwrite(&type_byte, 1, 1, g_dump_file);
        fwrite(&size, 4, 1, g_dump_file);
        fwrite(frame.data.data(), 1, frame.data.size(), g_dump_file);
        printf("[Server] Dumped frame %llu to file\n", (unsigned long long)frame.data.size());
        fflush(g_dump_file);
    }

    frame_count++;

    if (frame.type == fec_protocol::frame_type::VIDEO_PARAMS) {
        printf("[Server] Received codec extradata (%u bytes)\n", frame.size);
        g_decoder.extradata.assign(frame.data.begin(), frame.data.end());
        // Try to initialize decoder with extradata
        if (!g_decoder.initialized) {
            // Assume H.264 - most common
            init_decoder(AV_CODEC_ID_H264, g_decoder.extradata.data(),
                         (int)g_decoder.extradata.size());
        }
        return;
    }

    if (frame.type == fec_protocol::frame_type::VIDEO ||
        frame.type == fec_protocol::frame_type::VIDEO_IDR) {

        // Lazy init decoder if not initialized yet
        if (!g_decoder.initialized) {
            const uint8_t* ed = g_decoder.extradata.empty() ? NULL : g_decoder.extradata.data();
            int ed_size = (int)g_decoder.extradata.size();
            if (!init_decoder(AV_CODEC_ID_H264, ed, ed_size)) {
                return;
            }
        }

        decode_and_display(frame.data.data(), frame.data.size());

        if (frame_count % 100 == 0) {
            printf("[Server] Decoded %llu frames\n", (unsigned long long)frame_count);
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);

    // Open dump file for received frames
    g_dump_file = fopen("server_frames.bin", "wb");
    if (!g_dump_file) {
        fprintf(stderr, "[Server] Warning: cannot open server_frames.bin for writing\n");
    }

    // --- Create UDP socket ---
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(port);

    if (bind(sock, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
        perror("bind");
        close(sock);
        return 1;
    }

    // Set non-blocking
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    printf("[Server] Listening on UDP port %d\n", port);

    // --- Setup fec_protocol receiver ---
    fec_protocol::ReceiverConfig recv_cfg;
    recv_cfg.frame_timeout_ms = 3000;
    recv_cfg.nack_delay_ms = 10;
    recv_cfg.nack_interval_ms = 50;
    recv_cfg.max_pending_frames = 64;

    fec_protocol::FrameReceiver receiver(recv_cfg);

    // Store client address when first packet arrives
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    bool has_client = false;

    receiver.setSendCallback([&](const uint8_t* data, size_t len) {
        if (has_client) {
            sendto(sock, data, len, 0,
                   (struct sockaddr*)&client_addr, client_addr_len);
        }
    });

    receiver.setFrameReadyCallback(on_frame_ready);

    receiver.setFrameDroppedCallback([](fec_protocol::FrameType type, uint32_t size) {
        printf("[Server] Frame dropped: type=%d, size=%u\n", type, size);
    });

    // --- Main loop ---
    printf("[Server] Waiting for data...\n");

    bool running = true;
    uint8_t recv_buf[65536];

    while (running) {
        // Poll for SDL events
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
                break;
            }
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
                running = false;
                break;
            }
        }

        if (!running) break;

        // Receive packets
        struct pollfd pfd;
        pfd.fd = sock;
        pfd.events = POLLIN;
        int poll_ret = poll(&pfd, 1, 5); // 5ms timeout

        if (poll_ret > 0 && (pfd.revents & POLLIN)) {
            ssize_t n;
            while ((n = recvfrom(sock, recv_buf, sizeof(recv_buf), 0,
                                 (struct sockaddr*)&client_addr, &client_addr_len)) > 0) {
                if (!has_client) {
                    char addr_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));
                    printf("[Server] Client connected from %s:%d\n",
                           addr_str, ntohs(client_addr.sin_port));
                    has_client = true;
                }
                receiver.onPacketReceived(recv_buf, n);
            }
        }

        // Tick protocol
        uint64_t t = now_us();
        receiver.tick(t);
    }

    // Cleanup
    printf("[Server] Shutting down...\n");

    const auto& stats = receiver.getStats();
    printf("[Server] Final stats: loss=%.2f%%, fec_recovery=%.2f%%, nack_recovery=%.2f%%\n",
           stats.loss_rate * 100.0f,
           stats.fec_recovery_rate * 100.0f,
           stats.nack_recovery_rate * 100.0f);

    if (g_decoder.frame) av_frame_free(&g_decoder.frame);
    if (g_decoder.pkt) av_packet_free(&g_decoder.pkt);
    if (g_decoder.codec_ctx) avcodec_free_context(&g_decoder.codec_ctx);

    if (g_display.texture) SDL_DestroyTexture(g_display.texture);
    if (g_display.renderer) SDL_DestroyRenderer(g_display.renderer);
    if (g_display.window) SDL_DestroyWindow(g_display.window);
    SDL_Quit();

    close(sock);
    if (g_dump_file) {
        fclose(g_dump_file);
        printf("[Server] Frame dump written to server_frames.bin\n");
    }
    printf("[Server] Done.\n");
    return 0;
}

