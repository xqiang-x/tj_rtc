#!/bin/bash

# Android Studio Gradle 本地配置脚本
# 解决 Gradle 下载超时问题

set -e

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

echo_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

echo_debug() {
    echo -e "${BLUE}[DEBUG]${NC} $1"
}

echo "========================================="
echo "  Gradle 本地配置工具"
echo "========================================="
echo ""

# 检查 Gradle 是否已下载
GRADLE_ZIP="/Users/mac/Downloads/gradle-8.0-bin.zip"
GRADLE_DIR="/Users/mac/Downloads/gradle-8.0"

if [ ! -f "$GRADLE_ZIP" ]; then
    echo_warn "未找到 Gradle 安装包: $GRADLE_ZIP"
    echo_info "请先下载 gradle-8.0-bin.zip 到 ~/Downloads/ 目录"
    echo_info "下载地址: https://services.gradle.org/distributions/gradle-8.0-bin.zip"
    exit 1
fi

echo_info "发现 Gradle 安装包: $GRADLE_ZIP"

# 解压（如果还没解压）
if [ ! -d "$GRADLE_DIR" ]; then
    echo_info "正在解压 Gradle..."
    cd /Users/mac/Downloads
    unzip -q gradle-8.0-bin.zip
    echo_info "✓ Gradle 已解压到: $GRADLE_DIR"
else
    echo_info "✓ Gradle 已解压: $GRADLE_DIR"
fi

# 检查 Android Studio 项目
ANDROID_PROJECT="/Users/mac/Documents/qoder_libuv/android_rtc_app"

if [ ! -d "$ANDROID_PROJECT" ]; then
    echo_warn "未找到 Android 项目: $ANDROID_PROJECT"
    exit 1
fi

echo_info "发现 Android 项目: $ANDROID_PROJECT"
echo ""

# 方法 1: 修改 gradle-wrapper.properties 使用本地文件
WRAPPER_PROPS="$ANDROID_PROJECT/gradle/wrapper/gradle-wrapper.properties"

echo_debug "配置方法 1: 修改 gradle-wrapper.properties..."

# 备份原文件
cp "$WRAPPER_PROPS" "$WRAPPER_PROPS.bak"

# 修改为使用本地文件
cat > "$WRAPPER_PROPS" << EOF
distributionBase=GRADLE_USER_HOME
distributionPath=wrapper/dists
distributionUrl=file\\:///Users/mac/Downloads/gradle-8.0-bin.zip
zipStoreBase=GRADLE_USER_HOME
zipStorePath=wrapper/dists
EOF

echo_info "✓ 已配置 gradle-wrapper.properties 使用本地 Gradle"
echo ""

# 方法 2: 直接复制到 Gradle 缓存
echo_debug "配置方法 2: 复制到 Gradle 缓存..."

GRADLE_CACHE_DIR="$HOME/.gradle/wrapper/dists/gradle-8.0-bin"

if [ ! -d "$GRADLE_CACHE_DIR" ]; then
    mkdir -p "$GRADLE_CACHE_DIR"
    echo_info "✓ 创建 Gradle 缓存目录: $GRADLE_CACHE_DIR"
fi

# 复制 zip 到缓存
cp "$GRADLE_ZIP" "$GRADLE_CACHE_DIR/"
echo_info "✓ 已复制 Gradle 到缓存"
echo ""

# 创建标记文件（告诉 Gradle 已解压）
CACHE_SUBDIR=$(ls "$GRADLE_CACHE_DIR" 2>/dev/null | head -1 || echo "gradle-8.0")
if [ -n "$CACHE_SUBDIR" ] && [ "$CACHE_SUBDIR" != "gradle-8.0-bin.zip" ]; then
    MARKER_FILE="$GRADLE_CACHE_DIR/$CACHE_SUBDIR/gradle-8.0-bin.zip.ok"
    mkdir -p "$GRADLE_CACHE_DIR/$CACHE_SUBDIR"
    touch "$MARKER_FILE"
    echo_info "✓ 创建完成标记: $MARKER_FILE"
fi

echo ""
echo_info "========================================="
echo_info "  ✅ Gradle 配置完成"
echo_info "========================================="
echo ""
echo_info "现在可以打开 Android Studio 了！"
echo ""
echo_info "如果仍然遇到问题，请手动配置："
echo_info "  1. 打开 Android Studio"
echo_info "  2. Settings → Build Tools → Gradle"
echo_info "  3. Gradle user home 选择: 'Specified location'"
echo_info "  4. 路径设置为: $GRADLE_DIR"
echo_info "  5. 点击 Apply 和 OK"
echo ""
echo_info "或者使用命令行编译："
echo_info "  cd $ANDROID_PROJECT"
echo_info "  ./gradlew assembleDebug"
echo ""
