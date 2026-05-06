@echo off
:: 解决Windows终端中文乱码
chcp 65001 >nul

:: ====================== 你的FFmpeg路径 ======================
set FFMPEG_PATH=D:\ffmpeg-8.1-essentials_build\bin\ffmpeg.exe
:: ===========================================================

if "%~1"=="" (
    echo 请把 MP4 文件拖到这个脚本图标上使用！
    pause
    exit
)

echo 正在转换为 H.265 裸流...
echo 输入文件：%~1
echo 输出文件：%~dpn1.h265

:: 核心转换命令（100%无损，适配RK3568）
"%FFMPEG_PATH%" -i "%~1" -vcodec copy -bsf:v hevc_mp4toannexb "%~dpn1.h265" -y

echo.
echo 转换完成！
pause