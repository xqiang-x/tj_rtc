package com.example.rtcapp;

import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.util.Log;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.Arrays;

public class RtcClient {
    private static final String TAG = "RtcClient";
    
    // 连接状态回调
    public interface ConnectionListener {
        void onConnected(int sessionId);
        void onDisconnected();
        // 订阅被服务器拒绝（如流不存在），reason 为服务器返回的原因
        default void onSubscribeFailed(String reason) {}
        // 首次成功解码并渲染一帧画面（用于隐藏"等待视频流"提示）
        default void onFirstVideoFrameRendered() {}
    }
    
    private ConnectionListener mConnectionListener;
    private volatile boolean mFirstFrameNotified = false;
    
    // 加载 native 库
    static {
        try {
            System.loadLibrary("rtc_native");
            Log.d(TAG, "Native 库加载成功");
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "Native 库加载失败: " + e.getMessage());
        }
    }
    
    private long mNativePtr;
    private MediaCodec mVideoDecoder;
    private AudioTrack mAudioTrack;
    private Surface mSurface;
    private boolean mEnableAudio;
    private boolean mHasSPSPPS = false;
    private int mFrameCount = 0;  // 帧计数器（用于调试）
    
    // SPS/PPS 缓存（从 NAL 流中扫描提取）
    private byte[] mSps;
    private byte[] mPps;
    
    // 输出渲染线程
    private Thread mRenderThread;
    private volatile boolean mRenderRunning = false;
    
    // Native 方法声明
    public native long nativeInit();
    public native void nativeStart(long ptr, String serverIp, int serverPort,
                                    String streamId, String userId, boolean enableAudio, boolean useUdp);
    public native void nativeStop(long ptr);
    public native void nativeRelease(long ptr);
    
    /**
     * 设置连接监听器
     */
    public void setConnectionListener(ConnectionListener listener) {
        mConnectionListener = listener;
    }
    
    /**
     * 初始化 RTC 客户端
     */
    public void init() {
        Log.d(TAG, "初始化 RtcClient");
        mNativePtr = nativeInit();
    }
    
    /**
     * 启动拉流（订阅流名）
     */
    public void start(String serverIp, int serverPort, String streamId, 
                      String userId, boolean enableAudio, boolean useUdp) {
        // 安全检查
        if (serverIp == null) serverIp = "";
        if (streamId == null) streamId = "";
        if (userId == null) userId = "";
        
        Log.d(TAG, "启动拉流: " + serverIp + ":" + serverPort + " stream=" + streamId + " udp=" + useUdp);
        mEnableAudio = enableAudio;
        initVideoDecoder();
        if (enableAudio) {
            initAudioTrack();
        }
        nativeStart(mNativePtr, serverIp, serverPort, streamId, userId, enableAudio, useUdp);
    }
    
    /**
     * 停止拉流
     */
    public void stop() {
        Log.d(TAG, "停止拉流");
        if (mNativePtr != 0) {
            nativeStop(mNativePtr);
        }
        releaseDecoder();
    }
    
    /**
     * 释放资源
     */
    public void release() {
        Log.d(TAG, "释放 RtcClient");
        if (mNativePtr != 0) {
            nativeRelease(mNativePtr);
            mNativePtr = 0;
        }
        releaseDecoder();
    }
    
    /**
     * 设置显示 Surface
     */
    public void setSurface(Surface surface) {
        Log.d(TAG, "设置 Surface");
        mSurface = surface;
        // 不在此处 configure / start，等收到 SPS/PPS 后再做
    }
    
    /**
     * 初始化视频解码器
     */
    private void initVideoDecoder() {
        try {
            // 优先用 AOSP 软解：小米设备上高通硬解 c2.qti.avc.decoder
            // 对每个输入都报 "Unsupported input buffer"，一帧都解不出来
            MediaCodec decoder = null;
            try {
                decoder = MediaCodec.createByCodecName("c2.android.avc.decoder");
                Log.d(TAG, "使用软解 c2.android.avc.decoder");
            } catch (Exception e) {
                decoder = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
                Log.d(TAG, "软解不可用，回退默认解码器");
            }
            mVideoDecoder = decoder;
            Log.d(TAG, "MediaCodec 解码器已创建");
        } catch (Exception e) {
            Log.e(TAG, "创建 MediaCodec 失败", e);
        }
    }
    
    /**
     * 配置视频解码器（拿到 SPS+PPS 之后调用）
     */
    private void configureVideoDecoder() {
        if (mVideoDecoder == null || mHasSPSPPS) return;
        if (mSps == null || mPps == null) return;
        if (mSurface == null) {
            Log.w(TAG, "[wqwq] mSurface 为空，无法配置");
            return;
        }
        
        try {
            // 从 SPS 解析真实分辨率；软解能力上限较小，写死大分辨率会配置失败
            int[] dims = parseSpsDimensions(mSps);
            Log.d(TAG, "[wqwq] 配置 MediaCodec, SPS=" + mSps.length + " PPS=" + mPps.length
                    + " 分辨率=" + dims[0] + "x" + dims[1]);
            
            MediaFormat format = MediaFormat.createVideoFormat(
                MediaFormat.MIMETYPE_VIDEO_AVC, dims[0], dims[1]);
            // csd-0 = SPS、csd-1 = PPS：必须是裸 NALU（不带起始码），
            // 否则高通硬件解码器启动即报 UNKNOWN_ERROR 崩溃
            format.setByteBuffer("csd-0", ByteBuffer.wrap(mSps));
            format.setByteBuffer("csd-1", ByteBuffer.wrap(mPps));
            
            mVideoDecoder.configure(format, mSurface, null, 0);
            mVideoDecoder.start();
            mHasSPSPPS = true;
            Log.d(TAG, "[wqwq] ✅ 视频解码器配置并启动完成");
            
            // 启动输出渲染线程
            startRenderThread();
        } catch (Exception e) {
            Log.e(TAG, "[wqwq] ❌ 配置视频解码器失败", e);
        }
    }
    
    /**
     * 在 Annex-B 流中扫描 NAL 单元，提取/比对 SPS(type=7) / PPS(type=8)。
     * 传输链路的帧 = 完整访问单元：IDR 帧内嵌 SPS/PPS，因此每帧都扫描：
     *   返回 0 = 未配齐或参数集无变化；
     *   返回 1 = 首次配齐 SPS/PPS（需要首次配置解码器）；
     *   返回 2 = 已配置后参数集发生变化（需要重建解码器）。
     */
    private int scanSpsPps(byte[] payload) {
        int len = payload.length;
        boolean changed = false;
        int i = 0;
        while (i < len) {
            // 查找起始码 00 00 00 01 或 00 00 01
            int nalStart = -1;
            int hdrSize = 0;
            for (int j = i; j + 2 < len; j++) {
                if (payload[j] == 0 && payload[j + 1] == 0) {
                    if (j + 3 < len && payload[j + 2] == 0 && payload[j + 3] == 1) {
                        nalStart = j + 4; hdrSize = 4; break;
                    } else if (payload[j + 2] == 1) {
                        nalStart = j + 3; hdrSize = 3; break;
                    }
                }
            }
            if (nalStart < 0) break;
            
            // 找下一个起始码确定 NAL 边界
            int nalEnd = len;
            for (int j = nalStart + 1; j + 2 < len; j++) {
                if (payload[j] == 0 && payload[j + 1] == 0 &&
                    (payload[j + 2] == 1 ||
                     (j + 3 < len && payload[j + 2] == 0 && payload[j + 3] == 1))) {
                    nalEnd = j;
                    break;
                }
            }
            
            int nalType = payload[nalStart] & 0x1F;
            if (nalType == 7) {
                byte[] sps = Arrays.copyOfRange(payload, nalStart, nalEnd);
                if (mSps == null) {
                    mSps = sps;
                    Log.d(TAG, "[wqwq] 提取到 SPS, len=" + mSps.length);
                } else if (!Arrays.equals(mSps, sps)) {
                    Log.d(TAG, "[wqwq] SPS 变化, " + mSps.length + "->" + sps.length);
                    mSps = sps;
                    changed = true;
                }
            } else if (nalType == 8) {
                byte[] pps = Arrays.copyOfRange(payload, nalStart, nalEnd);
                if (mPps == null) {
                    mPps = pps;
                    Log.d(TAG, "[wqwq] 提取到 PPS, len=" + mPps.length);
                } else if (!Arrays.equals(mPps, pps)) {
                    Log.d(TAG, "[wqwq] PPS 变化, " + mPps.length + "->" + pps.length);
                    mPps = pps;
                    changed = true;
                }
            }
            
            i = nalEnd;
        }
        if (mSps == null || mPps == null) return 0;
        if (!mHasSPSPPS) return 1;
        return changed ? 2 : 0;
    }
    
    /**
     * 极简 SPS 解析：返回 {width, height}。解析失败回退 640x480。
     */
    private static int[] parseSpsDimensions(byte[] sps) {
        try {
            // 跳过 NAL 头字节（裸 NALU 第 0 字节为 0x67）
            BitReader r = new BitReader(sps, 1);
            r.readBits(8);  // profile_idc
            r.readBits(8);  // constraint flags + reserved
            r.readBits(8);  // level_idc（u(8) 定长，非 exp-golomb）
            r.readUE();     // seq_parameter_set_id

            int profile = sps[1] & 0xFF;
            if (profile == 100 || profile == 110 || profile == 122 ||
                profile == 244 || profile == 44 || profile == 83 ||
                profile == 86 || profile == 118 || profile == 128) {
                int chromaFormatIdc = r.readUE();
                if (chromaFormatIdc == 3) r.readBits(1);
                r.readUE(); // bit_depth_luma_minus8
                r.readUE(); // bit_depth_chroma_minus8
                r.readBits(1); // qpprime_y_zero_transform_bypass
                if (r.readBits(1) == 1) { // seq_scaling_matrix_present
                    int cnt = (chromaFormatIdc != 3) ? 8 : 12;
                    for (int i = 0; i < cnt; i++) {
                        if (r.readBits(1) == 1) { // scaling_list_present
                            int size = (i < 6) ? 16 : 64;
                            int last = 8, next = 8;
                            for (int j = 0; j < size; j++) {
                                if (next != 0) {
                                    int delta = r.readSE();
                                    next = (last + delta + 256) % 256;
                                }
                                last = (next == 0) ? last : next;
                            }
                        }
                    }
                }
            }

            r.readUE(); // log2_max_frame_num_minus4
            int picOrderCntType = r.readUE();
            if (picOrderCntType == 0) {
                r.readUE();
            } else if (picOrderCntType == 1) {
                r.readBits(1);
                r.readSE();
                r.readSE();
                int n = r.readUE();
                for (int i = 0; i < n; i++) r.readSE();
            }
            r.readUE(); // max_num_ref_frames
            r.readBits(1); // gaps_in_frame_num

            int picWidthInMbsMinus1 = r.readUE();
            int picHeightInMapUnitsMinus1 = r.readUE();
            boolean frameMbsOnly = r.readBits(1) == 1;
            if (!frameMbsOnly) r.readBits(1);
            r.readBits(1); // direct_8x8_inference

            int cropL = 0, cropR = 0, cropT = 0, cropB = 0;
            if (r.readBits(1) == 1) { // frame_cropping
                cropL = r.readUE();
                cropR = r.readUE();
                cropT = r.readUE();
                cropB = r.readUE();
            }

            int width = (picWidthInMbsMinus1 + 1) * 16 - (cropL + cropR) * 2;
            int height = (2 - (frameMbsOnly ? 1 : 0)) * (picHeightInMapUnitsMinus1 + 1) * 16
                    - (cropT + cropB) * 2;
            if (width > 0 && height > 0) return new int[]{width, height};
        } catch (Exception e) {
            Log.w(TAG, "SPS 解析失败，回退 640x480", e);
        }
        return new int[]{640, 480};
    }

    /** 位流读取器（exp-golomb） */
    private static class BitReader {
        private final byte[] data;
        private int pos; // 字节位置
        private int bit; // 当前字节内剩余位数

        BitReader(byte[] data, int offset) {
            this.data = data;
            this.pos = offset;
            this.bit = 0;
        }

        int readBits(int n) {
            int val = 0;
            for (int i = 0; i < n; i++) {
                if (pos >= data.length) throw new IllegalStateException("SPS 位流越界");
                val = (val << 1) | ((data[pos] >> (7 - bit)) & 1);
                if (++bit == 8) { bit = 0; pos++; }
            }
            return val;
        }

        int readUE() {
            int zeros = 0;
            while (readBits(1) == 0 && zeros < 32) zeros++;
            return (1 << zeros) - 1 + (zeros == 0 ? 0 : readBits(zeros));
        }

        int readSE() {
            int v = readUE();
            return (v % 2 == 0) ? -(v / 2) : (v + 1) / 2;
        }
    }

    /**
     * 启动 MediaCodec 输出渲染线程
     */
    private void startRenderThread() {
        if (mRenderRunning) return;
        mRenderRunning = true;
        mRenderThread = new Thread(() -> {
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            while (mRenderRunning) {
                try {
                    int outIdx = mVideoDecoder.dequeueOutputBuffer(info, 10000);
                    if (outIdx >= 0) {
                        // render 到 Surface
                        mVideoDecoder.releaseOutputBuffer(outIdx, true);
                        // 第一次成功渲染时通知 UI
                        if (!mFirstFrameNotified) {
                            mFirstFrameNotified = true;
                            Log.d(TAG, "[wqwq] 🎬 首帧已渲染");
                            if (mConnectionListener != null) {
                                try { mConnectionListener.onFirstVideoFrameRendered(); }
                                catch (Exception ignore) {}
                            }
                        }
                    } else if (outIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                        Log.d(TAG, "[wqwq] 输出格式变化: " + mVideoDecoder.getOutputFormat());
                    }
                } catch (IllegalStateException e) {
                    Log.e(TAG, "[wqwq] 渲染线程异常", e);
                    break;
                } catch (Exception e) {
                    Log.e(TAG, "[wqwq] 渲染线程错误", e);
                }
            }
            Log.d(TAG, "[wqwq] 渲染线程退出");
        }, "RtcRenderThread");
        mRenderThread.start();
    }
    
    /**
     * 初始化音频播放器
     */
    private void initAudioTrack() {
        int sampleRate = 48000;
        int channelConfig = AudioFormat.CHANNEL_OUT_STEREO;
        int audioFormat = AudioFormat.ENCODING_PCM_16BIT;
        int bufferSize = AudioTrack.getMinBufferSize(sampleRate, channelConfig, audioFormat);
        
        mAudioTrack = new AudioTrack(
            AudioManager.STREAM_MUSIC,
            sampleRate,
            channelConfig,
            audioFormat,
            bufferSize * 2,
            AudioTrack.MODE_STREAM
        );
        mAudioTrack.play();
        Log.d(TAG, "AudioTrack 已启动");
    }
    
    /**
     * 释放解码器
     */
    private void releaseDecoder() {
        mRenderRunning = false;
        if (mRenderThread != null) {
            try { mRenderThread.join(500); } catch (InterruptedException ignore) {}
            mRenderThread = null;
        }
        
        if (mVideoDecoder != null) {
            try {
                mVideoDecoder.stop();
                mVideoDecoder.release();
            } catch (Exception e) {
                Log.e(TAG, "释放 MediaCodec 失败", e);
            }
            mVideoDecoder = null;
        }
        
        if (mAudioTrack != null) {
            try {
                mAudioTrack.stop();
                mAudioTrack.release();
            } catch (Exception e) {
                Log.e(TAG, "释放 AudioTrack 失败", e);
            }
            mAudioTrack = null;
        }
        
        mHasSPSPPS = false;
        mSps = null;
        mPps = null;
        mFirstFrameNotified = false;
    }
    
    /**
     * 仅释放解码器与渲染线程，保留 SPS/PPS（用于参数集变化后重建解码器）
     */
    private void releaseDecoderKeepParams() {
        mRenderRunning = false;
        if (mRenderThread != null) {
            try { mRenderThread.join(500); } catch (InterruptedException ignore) {}
            mRenderThread = null;
        }
        
        if (mVideoDecoder != null) {
            try {
                mVideoDecoder.stop();
                mVideoDecoder.release();
            } catch (Exception e) {
                Log.e(TAG, "释放 MediaCodec 失败", e);
            }
            mVideoDecoder = null;
        }
        
        mHasSPSPPS = false;
        mFirstFrameNotified = false;
    }
    
    // ========== JNI 回调方法 ==========
    
    /**
     * 接收视频帧（从 JNI 调用）
     * 协议：data[0]=frameType (0x01=纯参数集, 0x02=IDR, 0x03=P)，data[1..]=Annex-B H.264
     * 链路以完整访问单元为帧：IDR 帧内嵌 SPS/PPS（关键帧 = [SPS][PPS][IDR]），
     * 因此对每一帧都扫描 NAL 提取/比对 SPS/PPS。
     */
    private void onVideoFrame(int sourceSessionId, byte[] data) {
        if (data == null || data.length < 2) return;
        
        byte frameType = data[0];
        byte[] payload = Arrays.copyOfRange(data, 1, data.length);
        
        if (mFrameCount < 5) {
            Log.d(TAG, "[wqwq] 视频帧 #" + mFrameCount + " type=0x" + String.format("%02X", frameType) + " size=" + payload.length);
            mFrameCount++;
        }
        
        // 每帧扫描参数集：首次配齐则配置解码器；已配置后参数集变化则重建
        boolean justConfigured = false;
        int scanResult = scanSpsPps(payload);
        if (scanResult == 1) {
            configureVideoDecoder();
            justConfigured = mHasSPSPPS;
        } else if (scanResult == 2 && mHasSPSPPS) {
            Log.i(TAG, "[wqwq] 参数集变化，重建解码器");
            releaseDecoderKeepParams();
            initVideoDecoder();
            configureVideoDecoder();
            justConfigured = mHasSPSPPS;
        }
        
        // 解码器未配置好之前，帧没法独立解码，丢弃
        if (!mHasSPSPPS) return;
        
        // 刚完成配置的那一帧必须是 IDR（含 IDR slice 才能启动解码），否则丢弃
        if (justConfigured && !containsIdrNal(payload)) return;
        
        // 已配置：喂给 MediaCodec 解码
        try {
            int inputBufferId = mVideoDecoder.dequeueInputBuffer(10000);
            if (inputBufferId >= 0) {
                ByteBuffer buffer = mVideoDecoder.getInputBuffer(inputBufferId);
                if (buffer != null) {
                    buffer.clear();
                    buffer.put(payload);
                    long pts = System.nanoTime() / 1000;
                    mVideoDecoder.queueInputBuffer(inputBufferId, 0, payload.length, pts, 0);
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "[wqwq] 解码视频帧失败", e);
        }
    }
    
    /**
     * 判断 Annex-B 流中是否包含 IDR slice (NAL type 5)
     */
    private boolean containsIdrNal(byte[] payload) {
        int len = payload.length;
        for (int j = 0; j + 3 < len; j++) {
            int nalStart = -1;
            if (payload[j] == 0 && payload[j + 1] == 0) {
                if (j + 3 < len && payload[j + 2] == 0 && payload[j + 3] == 1) {
                    nalStart = j + 4;
                } else if (payload[j + 2] == 1) {
                    nalStart = j + 3;
                }
            }
            if (nalStart > 0 && nalStart < len) {
                int nalType = payload[nalStart] & 0x1F;
                if (nalType == 5) return true;
            }
        }
        return false;
    }
    
    /**
     * 接收音频帧（从 JNI 调用）
     */
    private void onAudioFrame(int sourceSessionId, byte[] data) {
        if (!mEnableAudio || data == null || data.length < 2) return;
        
        // 跳过帧类型字节
        byte[] payload = Arrays.copyOfRange(data, 1, data.length);
        
        // 播放音频
        if (mAudioTrack != null && mAudioTrack.getPlayState() == AudioTrack.PLAYSTATE_PLAYING) {
            mAudioTrack.write(payload, 0, payload.length);
        }
    }
    
    /**
     * 连接成功回调（从 JNI 调用）
     */
    private void onConnected(int sessionId) {
        Log.d(TAG, "已连接到服务器, sessionId=" + sessionId);
        if (mConnectionListener != null) {
            mConnectionListener.onConnected(sessionId);
        }
    }
    
    /**
     * 断开连接回调（从 JNI 调用）
     */
    private void onDisconnected() {
        Log.d(TAG, "与服务器断开连接");
        if (mConnectionListener != null) {
            mConnectionListener.onDisconnected();
        }
    }

    /**
     * 订阅失败回调（从 JNI 调用），reason 为服务器返回的原因（如 STREAM_NOT_FOUND）
     */
    private void onSubscribeFailed(String reason) {
        Log.w(TAG, "订阅失败: " + reason);
        if (mConnectionListener != null) {
            mConnectionListener.onSubscribeFailed(reason);
        }
    }
}
