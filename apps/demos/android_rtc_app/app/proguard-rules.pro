# Android RTC App - ProGuard Rules

# 保留 JNI 方法（不被混淆）
-keep class com.example.rtcapp.RtcClient {
    native <methods>;
}

# 保留 JNI 回调方法
-keepclassmembers class com.example.rtcapp.RtcClient {
    private void onVideoFrame(int, byte[]);
    private void onAudioFrame(int, byte[]);
    private void onConnected(int);
    private void onDisconnected();
}

# 保留 native 库
-keepclasseswithmembernames,includedescriptorclasses class * {
    native <methods>;
}

# 保留配置管理类
-keep class com.example.rtcapp.ConfigManager { *; }

# 保留 Activity
-keep class com.example.rtcapp.** { *; }

# AndroidX
-keep class androidx.** { *; }
-keep interface androidx.** { *; }

# Material Design
-keep class com.google.android.material.** { *; }
