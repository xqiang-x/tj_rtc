package com.example.rtcapp;

import android.content.Intent;
import android.os.Bundle;
import android.text.TextUtils;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;

public class MainActivity extends AppCompatActivity {
    private EditText etServerIp;
    private EditText etServerPort;
    private EditText etStreamId;
    private EditText etUserId;
    private CheckBox cbEnableAudio;
    private CheckBox cbUseUdp;
    private Button btnStart;
    
    private ConfigManager configManager;
    
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        
        // 初始化配置管理器
        configManager = new ConfigManager(this);
        
        // 绑定 UI 控件
        etServerIp = findViewById(R.id.et_server_ip);
        etServerPort = findViewById(R.id.et_server_port);
        etStreamId = findViewById(R.id.et_stream_id);
        etUserId = findViewById(R.id.et_user_id);
        cbEnableAudio = findViewById(R.id.cb_enable_audio);
        cbUseUdp = findViewById(R.id.cb_use_udp);
        btnStart = findViewById(R.id.btn_start);
        
        // 加载上次保存的配置
        loadSavedConfig();
        
        // 开始按钮点击事件
        btnStart.setOnClickListener(v -> {
            // 安全获取文本（避免 null）
            CharSequence serverIpSeq = etServerIp.getText();
            CharSequence portSeq = etServerPort.getText();
            CharSequence streamIdSeq = etStreamId.getText();
            CharSequence userIdSeq = etUserId.getText();
            
            String serverIp = serverIpSeq != null ? serverIpSeq.toString().trim() : "";
            String portStr = portSeq != null ? portSeq.toString().trim() : "";
            String streamId = streamIdSeq != null ? streamIdSeq.toString().trim() : "";
            String userId = userIdSeq != null ? userIdSeq.toString().trim() : "";
            boolean enableAudio = cbEnableAudio.isChecked();
            boolean useUdp = cbUseUdp.isChecked();
            
            // 验证输入
            if (TextUtils.isEmpty(serverIp)) {
                Toast.makeText(this, "请输入服务器地址", Toast.LENGTH_SHORT).show();
                return;
            }
            
            if (TextUtils.isEmpty(portStr)) {
                Toast.makeText(this, "请输入服务器端口", Toast.LENGTH_SHORT).show();
                return;
            }
            
            int port;
            try {
                port = Integer.parseInt(portStr);
                if (port <= 0 || port > 65535) {
                    Toast.makeText(this, "端口号无效", Toast.LENGTH_SHORT).show();
                    return;
                }
            } catch (NumberFormatException e) {
                Toast.makeText(this, "端口号格式错误", Toast.LENGTH_SHORT).show();
                return;
            }
            
            if (TextUtils.isEmpty(streamId)) {
                Toast.makeText(this, "请输入流名", Toast.LENGTH_SHORT).show();
                return;
            }
            
            if (TextUtils.isEmpty(userId)) {
                Toast.makeText(this, "请输入用户ID", Toast.LENGTH_SHORT).show();
                return;
            }
            
            // 保存配置
            configManager.saveConfig(serverIp, port, streamId, userId, enableAudio, useUdp);
            
            // 启动拉流界面
            Intent intent = new Intent(this, StreamActivity.class);
            intent.putExtra("server_ip", serverIp);
            intent.putExtra("server_port", port);
            intent.putExtra("stream_id", streamId);
            intent.putExtra("user_id", userId);
            intent.putExtra("enable_audio", enableAudio);
            intent.putExtra("use_udp", useUdp);
            startActivity(intent);
        });
    }
    
    /**
     * 加载保存的配置
     */
    private void loadSavedConfig() {
        etServerIp.setText(configManager.getServerIp());
        etServerPort.setText(String.valueOf(configManager.getServerPort()));
        etStreamId.setText(configManager.getStreamId());
        etUserId.setText(configManager.getUserId());
        cbEnableAudio.setChecked(configManager.isAudioEnabled());
        cbUseUdp.setChecked(configManager.isUseUdp());
    }
}
