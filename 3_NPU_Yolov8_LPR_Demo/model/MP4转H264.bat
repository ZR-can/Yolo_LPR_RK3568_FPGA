@echo off
chcp 65001 >nul

set FFMPEG_PATH=D:\ffmpeg-8.1-essentials_build\bin\ffmpeg.exe

if "%~1"=="" (
    echo 请把 MP4 文件拖到我上面！
    pause
    exit
)

"%FFMPEG_PATH%" -i "%~1" -vf "scale=640:640:force_original_aspect_ratio=decrease,pad=640:640:(ow-iw)/2:(oh-ih)/2:color=black" -s 640x640 -c:v libx264 -preset fast -profile baseline -an -f h264 "%~dpn1.h264" -y

echo ==========================
echo 转换完成！严格 640×640 黑边填充
echo 完全匹配 Python letter_box 效果
echo ==========================
pause