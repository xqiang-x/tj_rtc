#!/bin/bash

# Android RTC App 编译脚本
# 使用方法: ./build.sh

set -e

echo "========================================="
echo "  Android RTC App 编译脚本"
echo "========================================="

# 检查是否在正确的目录
if [ ! -f "build.gradle" ]; then
    echo "错误: 请在 android_rtc_app 目录下运行此脚本"
    exit 1
fi

# 检查 gradlew 是否存在
if [ ! -f "gradlew" ]; then
    echo "错误: 找不到 gradlew，请先用 Android Studio 打开项目"
    exit 1
fi

# 清理旧的构建
echo ""
echo "清理旧的构建..."
./gradlew clean

# 编译 Debug 版本
echo ""
echo "编译 Debug 版本..."
./gradlew assembleDebug

# 检查编译结果
if [ -f "app/build/outputs/apk/debug/app-debug.apk" ]; then
    echo ""
    echo "========================================="
    echo "  ✅ 编译成功！"
    echo "========================================="
    echo ""
    echo "APK 位置: app/build/outputs/apk/debug/app-debug.apk"
    echo ""
    
    # 询问是否安装
    read -p "是否安装到连接的设备？(y/n) " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        echo "安装 APK..."
        adb install -r app/build/outputs/apk/debug/app-debug.apk
        echo ""
        echo "✅ 安装完成！"
    fi
else
    echo ""
    echo "========================================="
    echo "  ❌ 编译失败"
    echo "========================================="
    exit 1
fi
