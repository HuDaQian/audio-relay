#!/usr/bin/env bash
set -e

# Colors for output
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

DIST_DIR="$SCRIPT_DIR/dist"
mkdir -p "$DIST_DIR"
rm -rf "$DIST_DIR"/*

# Locate Flutter
if command -v flutter >/dev/null 2>&1; then
    FLUTTER_CMD="flutter"
elif [ -x "/Users/hudaqian/fvm/default/bin/flutter" ]; then
    FLUTTER_CMD="/Users/hudaqian/fvm/default/bin/flutter"
else
    echo -e "${RED}错误：未找到 flutter 命令，请确保 flutter 在 PATH 中。${NC}"
    exit 1
fi

echo -e "${BLUE}==============================================${NC}"
echo -e "${BLUE}  Audio Relay 极简跨平台一键打包脚本         ${NC}"
echo -e "${BLUE}  输出目录: $DIST_DIR                         ${NC}"
echo -e "${BLUE}==============================================${NC}"

OS="$(uname -s)"

# 1. 打包 Android APK (全平台支持)
echo -e "\n${YELLOW}[1/3] 正在打包 Android 客户端 (APK)...${NC}"
$FLUTTER_CMD build apk --release

APK_SRC="build/app/outputs/flutter-apk/app-release.apk"
if [ -f "$APK_SRC" ]; then
    cp "$APK_SRC" "$DIST_DIR/audio-relay-android.apk"
    echo -e "${GREEN}✓ Android APK 打包成功: dist/audio-relay-android.apk${NC}"
else
    echo -e "${RED}✗ Android APK 打包失败，未找到输出文件。${NC}"
fi

# 2. 如果是 macOS 主机，打包 macOS 与 iOS
if [ "$OS" = "Darwin" ]; then
    # 2.1 打包 macOS 桌面端
    echo -e "\n${YELLOW}[2/3] 正在打包 macOS 桌面端 (.app / .zip)...${NC}"
    $FLUTTER_CMD build macos --release
    
    MACOS_APP_SRC="build/macos/Build/Products/Release/audio_relay_flutter.app"
    if [ -d "$MACOS_APP_SRC" ]; then
        cd "build/macos/Build/Products/Release"
        zip -r -q "$DIST_DIR/audio-relay-macos.zip" "audio_relay_flutter.app"
        cd "$SCRIPT_DIR"
        echo -e "${GREEN}✓ macOS 桌面端打包成功: dist/audio-relay-macos.zip${NC}"
    else
        echo -e "${RED}✗ macOS 桌面端打包失败。${NC}"
    fi

    # 2.2 打包 iOS 客户端 (免签侧载 IPA)
    echo -e "\n${YELLOW}[3/3] 正在打包 iOS 客户端 (.ipa 侧载包)...${NC}"
    $FLUTTER_CMD build ios --release --no-codesign

    IOS_APP_SRC="build/ios/iphoneos/Runner.app"
    if [ -d "$IOS_APP_SRC" ]; then
        TEMP_PAYLOAD="$SCRIPT_DIR/build/ios/Payload"
        rm -rf "$TEMP_PAYLOAD"
        mkdir -p "$TEMP_PAYLOAD"
        cp -r "$IOS_APP_SRC" "$TEMP_PAYLOAD/"
        cd "$SCRIPT_DIR/build/ios"
        zip -r -q "$DIST_DIR/audio-relay-ios.ipa" "Payload"
        rm -rf "$TEMP_PAYLOAD"
        cd "$SCRIPT_DIR"
        echo -e "${GREEN}✓ iOS 侧载包打包成功: dist/audio-relay-ios.ipa (支持 TrollStore/AltStore/Sideloadly 直接安装)${NC}"
    else
        echo -e "${RED}✗ iOS 客户端打包失败。${NC}"
    fi
else
    echo -e "\n${YELLOW}提示：当前环境非 macOS，跳过 macOS 与 iOS 构建。${NC}"
fi

# 3. Windows 平台提示
echo -e "\n${BLUE}==============================================${NC}"
echo -e "${BLUE}  打包完成！生成物料列表：                     ${NC}"
echo -e "${BLUE}==============================================${NC}"
ls -lh "$DIST_DIR"

if [ "$OS" != "MINGW"* ] && [ "$OS" != "MSYS"* ] && [ "$OS" != "CYGWIN"* ]; then
    echo -e "\n${YELLOW}【关于 Windows 安装包提示】${NC}"
    echo -e "Flutter 的 Windows 原生可执行文件（.exe）受底层 MSVC/CMake 机制限制，需在 Windows 环境下编译。"
    echo -e "我们已同步为你准备了："
    echo -e "  1. Windows 一键打包脚本: ${GREEN}build_windows.bat${NC} (在 Windows 机器上双击运行即可输出 .exe)"
    echo -e "  2. GitHub Actions 自动化脚本: ${GREEN}.github/workflows/build_all.yml${NC} (推送到 GitHub 可直接自动构建包含 Windows 在内的全平台安装包！)"
fi
