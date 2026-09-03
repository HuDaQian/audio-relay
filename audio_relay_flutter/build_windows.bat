@echo off
setlocal enabledelayedexpansion

echo ==============================================
echo   Audio Relay Windows 一键打包脚本
echo ==============================================

if not exist dist mkdir dist

echo [1/2] 正在编译 Flutter Windows 发行包...
call flutter build windows --release
if %errorlevel% neq 0 (
    echo 编译 Windows 失败！
    exit /b %errorlevel%
)

echo [2/2] 正在打包压缩至 dist\audio-relay-windows.zip ...
powershell -Command "Compress-Archive -Path 'build\windows\x64\runner\Release\*' -DestinationPath 'dist\audio-relay-windows.zip' -Force"

echo ==============================================
echo 打包成功！产物路径: dist\audio-relay-windows.zip
echo ==============================================
pause
