/**
 * FEC Protocol Demo - Client (Sender)
 *
 * Reads an MP4 file using ffmpeg, extracts encoded video packets,
 * and sends them through fec_protocol over UDP to the server.
 *
 * Usage: ./fec_demo_client <mp4_file> <server_ip> <server_port>
 */

#include "sender.h"
#include "receiver.h"
#include "types.h"
#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
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
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
}

static uint64_t now_us() {
    auto tp = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(
        tp.time_since_epoch()).count();
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <mp4_file> <server_ip> <server_port>\n", argv[0]);
        return 1;
    }

    const char* input_file = argv[1];
    const char* server_ip = argv[2];
    int server_port = atoi(argv[3]);

    // --- Create UDP socket ---
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    // Bind to any port for receiving NACK/PONG from server
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = 0;
    if (bind(sock, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
        perror("bind");
        close(sock);
        return 1;
    }

    // Set non-blocking
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    printf("[Client] Sending to %s:%d\n", server_ip, server_port);

    // --- Setup fec_protocol sender ---
    fec_protocol::SenderConfig sender_cfg;
    sender_cfg.mtu = 1400;
    sender_cfg.fec_ratio = 0.6f;
    sender_cfg.cache_timeout_ms = 5000;
    sender_cfg.ping_interval_ms = 1000;

    fec_protocol::FrameSender sender(sender_cfg);

    sender.setSendCallback([&](const uint8_t* data, size_t len) {
        sendto(sock, data, len, 0,
               (struct sockaddr*)&server_addr, sizeof(server_addr));
    });

    // Also create a receiver to handle STATS from server (for RTT etc)
    fec_protocol::ReceiverConfig recv_cfg;
    fec_protocol::FrameReceiver receiver(recv_cfg);
    receiver.setSendCallback([&](const uint8_t* data, size_t len) {
        sendto(sock, data, len, 0,
               (struct sockaddr*)&server_addr, sizeof(server_addr));
    });

    // --- Open input file with ffmpeg ---
    AVFormatContext* fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, input_file, NULL, NULL) < 0) {
        fprintf(stderr, "[Client] Failed to open input file: %s\n", input_file);
        close(sock);
        return 1;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "[Client] Failed to find stream info\n");
        avformat_close_input(&fmt_ctx);
        close(sock);
        return 1;
    }

    // Find video stream
    int video_stream_idx = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = (int)i;
            break;
        }
    }

    if (video_stream_idx < 0) {
        fprintf(stderr, "[Client] No video stream found\n");
        avformat_close_input(&fmt_ctx);
        close(sock);
        return 1;
    }

    AVStream* video_stream = fmt_ctx->streams[video_stream_idx];
    AVRational time_base = video_stream->time_base;

    printf("[Client] Video stream: %dx%d, codec_id=%d\n",
           video_stream->codecpar->width,
           video_stream->codecpar->height,
           video_stream->codecpar->codec_id);

    // --- Read and send packets ---
    AVPacket* pkt = av_packet_alloc();
    int64_t start_time_us = now_us();
    int64_t first_pts = AV_NOPTS_VALUE;
    uint64_t frame_count = 0;

    // Open dump file for input frames
    FILE* dump_file = fopen("client_frames.bin", "wb");
    if (!dump_file) {
        fprintf(stderr, "[Client] Warning: cannot open client_frames.bin for writing\n");
    }

    // Send codec extradata (SPS/PPS) as a VIDEO_PARAMS frame first
    if (video_stream->codecpar->extradata_size > 0) {
        printf("[Client] Sending codec extradata (%d bytes)\n",
               video_stream->codecpar->extradata_size);

        // Dump extradata frame
        if (dump_file) {
            uint32_t idx = static_cast<uint32_t>(frame_count);
            uint8_t type_byte = fec_protocol::frame_type::VIDEO_PARAMS;
            uint32_t size = static_cast<uint32_t>(video_stream->codecpar->extradata_size);
            fwrite(&idx, 4, 1, dump_file);
            fwrite(&type_byte, 1, 1, dump_file);
            fwrite(&size, 4, 1, dump_file);
            fwrite(video_stream->codecpar->extradata, 1, size, dump_file);
        }
        frame_count++;

        sender.sendFrame(video_stream->codecpar->extradata,
                         video_stream->codecpar->extradata_size,
                         fec_protocol::frame_type::VIDEO_PARAMS);
    }

    // std::this_thread::sleep_for(std::chrono::seconds(1));
    // return 0;

    printf("[Client] Starting to send video frames...\n");

    while (true) {
        // Receive control packets from server (non-blocking)
        uint8_t recv_buf[2048];
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        ssize_t n;
        while ((n = recvfrom(sock, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr*)&from_addr, &from_len)) > 0) {
            sender.onPacketReceived(recv_buf, n);
        }

        // Read next packet
        int ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                printf("[Client] End of file reached. Total frames sent: %llu\n",
                       (unsigned long long)frame_count);
            } else {
                fprintf(stderr, "[Client] Error reading frame: %d\n", ret);
            }
            break;
        }

        // Only process video packets
        if (pkt->stream_index != video_stream_idx) {
            av_packet_unref(pkt);
            continue;
        }

        // Timing: wait until it's time to send this packet
        if (first_pts == AV_NOPTS_VALUE) {
            first_pts = pkt->pts;
        }

        int64_t pts_diff = pkt->pts - first_pts;
        int64_t target_time_us = start_time_us +
            av_rescale_q(pts_diff, time_base, (AVRational){1, 1000000});

        int64_t current = now_us();
        if (target_time_us > current) {
            int64_t sleep_us = target_time_us - current;
            // Tick while waiting
            while (sleep_us > 0) {
                int64_t tick_sleep = (sleep_us > 5000) ? 5000 : sleep_us;
                std::this_thread::sleep_for(std::chrono::microseconds(tick_sleep));

                uint64_t t = now_us();
                sender.tick(t);

                // Check for control packets
                while ((n = recvfrom(sock, recv_buf, sizeof(recv_buf), 0,
                                     (struct sockaddr*)&from_addr, &from_len)) > 0) {
                    sender.onPacketReceived(recv_buf, n);
                }

                sleep_us -= tick_sleep;
            }
        }

        // Determine frame type
        fec_protocol::FrameType ftype = fec_protocol::frame_type::VIDEO;
        if (pkt->flags & AV_PKT_FLAG_KEY) {
            ftype = fec_protocol::frame_type::VIDEO_IDR;
        }

        // Dump frame to file: [4B frame_index][1B type][4B size][data]
        if (dump_file) {
            uint32_t idx = static_cast<uint32_t>(frame_count);
            uint8_t type_byte = ftype;
            uint32_t size = static_cast<uint32_t>(pkt->size);
            fwrite(&idx, 4, 1, dump_file);
            fwrite(&type_byte, 1, 1, dump_file);
            fwrite(&size, 4, 1, dump_file);
            fwrite(pkt->data, 1, pkt->size, dump_file);
        }

        // Send via fec_protocol
        bool ok = sender.sendFrame(pkt->data, pkt->size, ftype);
        if (!ok) {
            fprintf(stderr, "[Client] sendFrame failed for frame %llu (size=%d)\n",
                    (unsigned long long)frame_count, pkt->size);
        }

        frame_count++;
        if (frame_count % 100 == 0) {
            const auto& stats = sender.getStats();
            printf("[Client] Sent %llu frames, RTT=%ums, loss=%.2f%%\n",
                   (unsigned long long)frame_count,
                   (unsigned)stats.rtt_ms,
                   stats.loss_rate * 100.0f);
        }

        sender.tick(now_us());
        av_packet_unref(pkt);
    }

    // Wait a bit to flush remaining retransmits
    printf("[Client] Flushing...\n");
    for (int i = 0; i < 100; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        uint64_t t = now_us();
        sender.tick(t);

        uint8_t recv_buf[2048];
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        ssize_t n;
        while ((n = recvfrom(sock, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr*)&from_addr, &from_len)) > 0) {
            sender.onPacketReceived(recv_buf, n);
        }
    }

    const auto& final_stats = sender.getStats();
    printf("[Client] Final stats: RTT=%ums, loss=%.2f%%, blocks_sent=%llu\n",
           (unsigned)final_stats.rtt_ms,
           final_stats.loss_rate * 100.0f,
           (unsigned long long)final_stats.blocks_sent);

    av_packet_free(&pkt);
    avformat_close_input(&fmt_ctx);
    close(sock);
    if (dump_file) {
        fclose(dump_file);
        printf("[Client] Frame dump written to client_frames.bin\n");
    }

    printf("[Client] Done.\n");
    return 0;
}

