package com.example.rtcapp;

import android.content.Context;
import android.content.SharedPreferences;

public class ConfigManager {
    private static final String PREFS_NAME = "rtc_config";
    private static final String KEY_SERVER_IP = "server_ip";
    private static final String KEY_SERVER_PORT = "server_port";
    private static final String KEY_STREAM_ID = "stream_id";
    private static final String KEY_USER_ID = "user_id";
    private static final String KEY_ENABLE_AUDIO = "enable_audio";
    private static final String KEY_USE_UDP = "use_udp";
    
    private SharedPreferences prefs;
    
    public ConfigManager(Context context) {
        prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
    }
    
    /**
     * 保存配置
     */
    public void saveConfig(String serverIp, int serverPort, String streamId, 
                           String userId, boolean enableAudio, boolean useUdp) {
        prefs.edit()
            .putString(KEY_SERVER_IP, serverIp)
            .putInt(KEY_SERVER_PORT, serverPort)
            .putString(KEY_STREAM_ID, streamId)
            .putString(KEY_USER_ID, userId)
            .putBoolean(KEY_ENABLE_AUDIO, enableAudio)
            .putBoolean(KEY_USE_UDP, useUdp)
            .apply();
    }
    
    /**
     * 获取服务器地址
     */
    public String getServerIp() {
        return prefs.getString(KEY_SERVER_IP, "106.15.177.248");
    }
    
    /**
     * 获取服务器端口
     */
    public int getServerPort() {
        return prefs.getInt(KEY_SERVER_PORT, 9200);
    }
    
    /**
     * 获取流名
     */
    public String getStreamId() {
        return prefs.getString(KEY_STREAM_ID, "stream1");
    }
    
    /**
     * 获取用户ID
     */
    public String getUserId() {
        return prefs.getString(KEY_USER_ID, "android_user");
    }
    
    /**
     * 是否启用音频
     */
    public boolean isAudioEnabled() {
        return prefs.getBoolean(KEY_ENABLE_AUDIO, true);
    }
    
    /**
     * 是否使用 UDP
     */
    public boolean isUseUdp() {
        return prefs.getBoolean(KEY_USE_UDP, true);  // 默认使用 UDP
    }
}
