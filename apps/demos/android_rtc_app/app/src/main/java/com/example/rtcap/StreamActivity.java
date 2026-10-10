package com.example.rtcapp;

import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;

public class StreamActivity extends AppCompatActivity {
    private static final String TAG = "StreamActivity";
    
    private SurfaceView surfaceVideo;
    private Button btnStop;
    private TextView tvStatus;
    
    private RtcClient rtcClient;
    private Handler mainHandler;
    
    // 配置参数
    private String serverIp;
    private int serverPort;
    private String streamId;
    private String userId;
    private boolean enableAudio;
    private boolean useUdp;
    
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        
        // 全屏沉浸式：隐藏状态栏与导航栏
        if (getSupportActionBar() != null) {
            getSupportActionBar().hide();
        }
        getWindow().setFlags(
                WindowManager.LayoutParams.FLAG_FULLSCREEN,
                WindowManager.LayoutParams.FLAG_FULLSCREEN);
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        
        setContentView(R.layout.activity_stream);
        
        mainHandler = new Handler(Looper.getMainLooper());
        
        // 获取配置参数
        serverIp = getIntent().getStringExtra("server_ip");
        serverPort = getIntent().getIntExtra("server_port", 9200);
        streamId = getIntent().getStringExtra("stream_id");
        userId = getIntent().getStringExtra("user_id");
        enableAudio = getIntent().getBooleanExtra("enable_audio", true);
        useUdp = getIntent().getBooleanExtra("use_udp", true);
        
        // 验证参数
        if (serverIp == null || streamId == null || userId == null) {
            Log.e(TAG, "配置参数不完整");
            Toast.makeText(this, "配置参数不完整", Toast.LENGTH_SHORT).show();
            finish();
            return;
        }
        
        Log.d(TAG, "配置: " + serverIp + ":" + serverPort + " stream=" + streamId);
        
        // 绑定UI控件
        surfaceVideo = findViewById(R.id.surface_video);
        btnStop = findViewById(R.id.btn_stop);
        tvStatus = findViewById(R.id.tv_status);
        
        // 设置 Surface 回调
        surfaceVideo.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                Log.d(TAG, "Surface 已创建");
                startStreaming(holder.getSurface());
            }
            
            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                Log.d(TAG, "Surface 已改变: " + width + "x" + height);
            }
            
            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                Log.d(TAG, "Surface 已销毁");
                stopStreaming();
            }
        });
        
        // 停止按钮
        btnStop.setOnClickListener(v -> {
            finish();
        });
    }
    
    /**
     * 启动拉流
     */
    private void startStreaming(android.view.Surface surface) {
        try {
            updateStatus("初始化中...");
            
            // 创建 RTC 客户端
            rtcClient = new RtcClient();
            
            // 设置连接监听器
            rtcClient.setConnectionListener(new RtcClient.ConnectionListener() {
                @Override
                public void onConnected(int sessionId) {
                    runOnUiThread(() -> {
                        Log.d(TAG, "连接成功，sessionId=" + sessionId);
                        updateStatus("等待视频流...");
                        Toast.makeText(StreamActivity.this, "连接成功", Toast.LENGTH_SHORT).show();
                    });
                }
                
                @Override
                public void onDisconnected() {
                    runOnUiThread(() -> {
                        Log.w(TAG, "连接断开");
                        updateStatus("连接已断开");
                        Toast.makeText(StreamActivity.this, "连接断开", Toast.LENGTH_SHORT).show();
                    });
                }
                
                @Override
                public void onSubscribeFailed(String reason) {
                    runOnUiThread(() -> {
                        String msg;
                        if (reason != null && reason.contains("STREAM_NOT_FOUND")) {
                            msg = "流不存在，连接已断开";
                        } else {
                            msg = "连接失败: " + (reason != null ? reason : "未知原因");
                        }
                        Log.w(TAG, "订阅失败: " + msg);
                        updateStatus(msg);
                        Toast.makeText(StreamActivity.this, msg, Toast.LENGTH_LONG).show();
                    });
                }
                
                @Override
                public void onFirstVideoFrameRendered() {
                    runOnUiThread(() -> {
                        Log.d(TAG, "首帧已渲染，隐藏状态提示");
                        if (tvStatus != null) {
                            tvStatus.setVisibility(View.GONE);
                        }
                    });
                }
            });
            
            rtcClient.init();
            
            // 设置显示 Surface
            rtcClient.setSurface(surface);
            
            // 启动拉流
            updateStatus("连接服务器...");
            rtcClient.start(serverIp, serverPort, streamId, userId, enableAudio, useUdp);
            
            Log.d(TAG, "拉流已启动，等待连接...");
            
            // 添加超时处理（10秒）
            mainHandler.postDelayed(() -> {
                if (tvStatus != null && tvStatus.getText().toString().contains("连接服务器")) {
                    Log.w(TAG, "连接超时");
                    updateStatus("连接超时，请检查网络和服务器");
                    Toast.makeText(StreamActivity.this, "连接超时", Toast.LENGTH_LONG).show();
                }
            }, 10000);
            
        } catch (Exception e) {
            Log.e(TAG, "启动拉流失败", e);
            String errorMsg = e.getMessage();
            updateStatus("启动失败: " + (errorMsg != null ? errorMsg : "未知错误"));
            Toast.makeText(this, "启动失败", Toast.LENGTH_SHORT).show();
        }
    }
    
    /**
     * 停止拉流
     */
    private void stopStreaming() {
        if (rtcClient != null) {
            try {
                rtcClient.stop();
                rtcClient.release();
                rtcClient = null;
                Log.d(TAG, "拉流已停止");
            } catch (Exception e) {
                Log.e(TAG, "停止拉流异常", e);
            }
        }
    }
    
    /**
     * 更新状态显示（在主线程）
     */
    private void updateStatus(String status) {
        mainHandler.post(() -> {
            if (tvStatus != null) {
                tvStatus.setText(status);
            }
        });
    }
    
    @Override
    protected void onDestroy() {
        super.onDestroy();
        stopStreaming();
    }
}
