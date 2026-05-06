#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "image_drawing.h"
#include "font.h"
#include "plate_font.h"
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) < (b)) ? (a) : (b))
static int resize_bilinear_c1(const unsigned char* src_pixels, int w, int h, unsigned char* dst_pixels, int w2, int h2);
// 通用字符查找函数
static const unsigned char* get_plate_char_bitmap(const char* text, int* step) {
    unsigned char ch = (unsigned char)text[0];
    
    // 1. 中文字符处理 
    if (ch >= 0xE0) {
        *step = 3;
        // 注意：循环上限修改为 81
        for (int i = 0; i < 81; i++) {
            if (plate_char_map[i].bytes == 3) {
                if (strncmp(text, plate_char_map[i].name, 3) == 0) {
                    return plate_font_data[plate_char_map[i].index];
                }
            }
        }
        return NULL; 
    }
    
    // 2. ASCII 字符处理
    *step = 1;
    // 注意：循环上限修改为 81
    for (int i = 0; i < 81; i++) {
        if (plate_char_map[i].bytes == 1) {
            if (text[0] == plate_char_map[i].name[0]) {
                return plate_font_data[plate_char_map[i].index];
            }
        }
    }
    // 兜底逻辑
    if (ch >= ' ' && ch <= '~') {
        return mono_font_data[ch - ' '];
    }
    return NULL;
}
// 统一绘制函数 (以RGB888格式为例)
static void draw_text_c3(unsigned char* pixels, int w, int h, const char* text, int x, int y, int fontpixelsize, unsigned int color) {
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 3;
    unsigned char* resized_font_bitmap = malloc(fontpixelsize * fontpixelsize * 2);
    const int n = strlen(text);

    int cursor_x = x;
    int cursor_y = y;
    
    for (int i = 0; i < n; ) {
        if (text[i] == '\n') {
            cursor_x = x;
            cursor_y += fontpixelsize * 2;
            i++;
            continue;
        }

        int step = 1;
        const unsigned char* font_bitmap = get_plate_char_bitmap(&text[i], &step);

        if (font_bitmap != NULL) {
            
            // ================= 核心修改区域 =================
            int char_w, char_h, y_offset, advance_x;
            
            if (step == 3) {
                // 中文：采用默认传入的尺寸 (例如 24x48)
                char_w = fontpixelsize;
                char_h = fontpixelsize * 2;
                y_offset = 0;               // 无需偏移
                advance_x = fontpixelsize;  // 中文字符较宽，步进一整个字宽
            } else {
                // 英文/数字：根据字体生成时的比例(18/36 = 0.5)强制缩小一半
                char_w = fontpixelsize / 2;             // 宽度减半 (例如 12)
                char_h = fontpixelsize;                 // 高度减半 (例如 24)
                y_offset = (fontpixelsize * 2 - char_h) / 2; // Y轴补偿，使其与中文垂直居中对齐
                advance_x = char_w + 2;                 // 英文步进为自身宽度 + 2像素的字间距
            }

            // 根据算出的目标宽高进行双线性缩放
            resize_bilinear_c1(font_bitmap, 20, 40, resized_font_bitmap, char_w, char_h);

            // 绘制当前字符
            for (int j = 0; j < char_h; j++) {
                int draw_y = cursor_y + y_offset + j; // 加入 Y 轴补偿对齐
                if (draw_y < 0) continue;
                if (draw_y >= h) break;

                const unsigned char* palpha = resized_font_bitmap + j * char_w;
                unsigned char* p = pixels + stride * draw_y;

                for (int k = 0; k < char_w; k++) {
                    int draw_x = cursor_x + k;
                    if (draw_x < 0) continue;
                    if (draw_x >= w) break;

                    unsigned char alpha = palpha[k]; // 获取抗锯齿透明度
                    
                    // Alpha 通道抗锯齿融合
                    p[draw_x * 3 + 0] = (p[draw_x * 3 + 0] * (255 - alpha) + pen_color[0] * alpha) / 255;
                    p[draw_x * 3 + 1] = (p[draw_x * 3 + 1] * (255 - alpha) + pen_color[1] * alpha) / 255;
                    p[draw_x * 3 + 2] = (p[draw_x * 3 + 2] * (255 - alpha) + pen_color[2] * alpha) / 255;
                }
            }
            cursor_x += advance_x; // 使用动态步进，防止英文字符之间间隙过大
            // ===============================================
        }
        i += step; // 字符串指针按字符字节长度步进
    }
    free(resized_font_bitmap);
}

static void draw_text_c4(unsigned char* pixels, int w, int h, int stride_bytes, // 必须传入真实的字节步幅
                         const char* text, int x, int y, int fontpixelsize, unsigned int color) 
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    
    // 使用外部传入的 stride_bytes，确保在对齐的显存上位置准确
    int stride = stride_bytes; 

    unsigned char* resized_font_bitmap = (unsigned char*)malloc(fontpixelsize * fontpixelsize * 2);
    const int n = strlen(text);

    int cursor_x = x;
    int cursor_y = y;

    for (int i = 0; i < n; ) {
        if (text[i] == '\n') {
            cursor_x = x;
            cursor_y += fontpixelsize * 2;
            i++;
            continue;
        }

        int step = 1;
        const unsigned char* font_bitmap = get_plate_char_bitmap(&text[i], &step);

        if (font_bitmap != NULL) {
            int char_w, char_h, y_offset, advance_x;

            if (step == 3) { // 中文
                char_w = fontpixelsize;
                char_h = fontpixelsize * 2;
                y_offset = 0;
                advance_x = fontpixelsize;
            } else { // 英文/数字
                char_w = fontpixelsize / 2;
                char_h = fontpixelsize;
                y_offset = (fontpixelsize * 2 - char_h) / 2;
                advance_x = char_w + 2;
            }

            resize_bilinear_c1(font_bitmap, 20, 40, resized_font_bitmap, char_w, char_h);

            for (int j = 0; j < char_h; j++) {
                int draw_y = cursor_y + y_offset + j;
                if (draw_y < 0 || draw_y >= h) continue;

                const unsigned char* palpha = resized_font_bitmap + j * char_w;
                // 计算该行起始地址
                unsigned char* p = pixels + stride * draw_y;

                for (int k = 0; k < char_w; k++) {
                    int draw_x = cursor_x + k;
                    if (draw_x < 0 || draw_x >= w) continue;

                    unsigned char alpha = palpha[k];
                    if (alpha == 0) continue;

                    // 获取像素点在 4 通道下的偏移量
                    int offset = draw_x * 4;

                    // 引入 Alpha 融合公式，消除字体边缘锯齿 (Anti-aliasing)
                    // C_new = (C_bg * (255 - alpha) + C_pen * alpha) / 255
                    p[offset + 0] = (p[offset + 0] * (255 - alpha) + pen_color[0] * alpha) >> 8; // R
                    p[offset + 1] = (p[offset + 1] * (255 - alpha) + pen_color[1] * alpha) >> 8; // G
                    p[offset + 2] = (p[offset + 2] * (255 - alpha) + pen_color[2] * alpha) >> 8; // B
                    p[offset + 3] = 0xFF; // DRM 显存通常忽略 Alpha 或设为不透明
                }
            }
            cursor_x += advance_x;
        }
        i += step;
    }

    free(resized_font_bitmap);
}

static void draw_text_c1(unsigned char* pixels, int w, int h, const char* text, int x, int y, int fontpixelsize,
                         unsigned int color)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w;

    unsigned char* resized_font_bitmap = malloc(fontpixelsize * fontpixelsize * 2);
    const int n = strlen(text);

    int cursor_x = x;
    int cursor_y = y;
    
    for (int i = 0; i < n; ) {
        if (text[i] == '\n') {
            cursor_x = x;
            cursor_y += fontpixelsize * 2;
            i++;
            continue;
        }

        int step = 1;
        const unsigned char* font_bitmap = get_plate_char_bitmap(&text[i], &step);

        if (font_bitmap != NULL) {
            int char_w, char_h, y_offset, advance_x;
            
            if (step == 3) {
                // 中文字符尺寸计算
                char_w = fontpixelsize;
                char_h = fontpixelsize * 2;
                y_offset = 0;               
                advance_x = fontpixelsize;  
            } else {
                // 英文/数字字符尺寸及对齐补偿
                char_w = fontpixelsize / 2;             
                char_h = fontpixelsize;                 
                y_offset = (fontpixelsize * 2 - char_h) / 2; 
                advance_x = char_w + 2;                 
            }

            resize_bilinear_c1(font_bitmap, 20, 40, resized_font_bitmap, char_w, char_h);

            for (int j = 0; j < char_h; j++) {
                int draw_y = cursor_y + y_offset + j; 
                if (draw_y < 0) continue;
                if (draw_y >= h) break;

                const unsigned char* palpha = resized_font_bitmap + j * char_w;
                unsigned char* p = pixels + stride * draw_y;

                for (int k = 0; k < char_w; k++) {
                    int draw_x = cursor_x + k;
                    if (draw_x < 0) continue;
                    if (draw_x >= w) break;

                    unsigned char alpha = palpha[k]; 
                    
                    // 单通道 Alpha 融合
                    p[draw_x] = (p[draw_x] * (255 - alpha) + pen_color[0] * alpha) / 255;
                }
            }
            cursor_x += advance_x; 
        }
        i += step; 
    }

    free(resized_font_bitmap);
}

static void draw_text_c2(unsigned char* pixels, int w, int h, const char* text, int x, int y, int fontpixelsize,
                         unsigned int color)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 2; // 双通道，每像素占 2 字节 (U 和 V)

    unsigned char* resized_font_bitmap = malloc(fontpixelsize * fontpixelsize * 2);
    const int n = strlen(text);

    int cursor_x = x;
    int cursor_y = y;
    
    for (int i = 0; i < n; ) {
        if (text[i] == '\n') {
            cursor_x = x;
            cursor_y += fontpixelsize * 2;
            i++;
            continue;
        }

        int step = 1;
        const unsigned char* font_bitmap = get_plate_char_bitmap(&text[i], &step);

        if (font_bitmap != NULL) {
            int char_w, char_h, y_offset, advance_x;
            
            if (step == 3) {
                char_w = fontpixelsize;
                char_h = fontpixelsize * 2;
                y_offset = 0;               
                advance_x = fontpixelsize;  
            } else {
                char_w = fontpixelsize / 2;             
                char_h = fontpixelsize;                 
                y_offset = (fontpixelsize * 2 - char_h) / 2; 
                advance_x = char_w + 2;                 
            }

            resize_bilinear_c1(font_bitmap, 20, 40, resized_font_bitmap, char_w, char_h);

            for (int j = 0; j < char_h; j++) {
                int draw_y = cursor_y + y_offset + j; 
                if (draw_y < 0) continue;
                if (draw_y >= h) break;

                const unsigned char* palpha = resized_font_bitmap + j * char_w;
                unsigned char* p = pixels + stride * draw_y;

                for (int k = 0; k < char_w; k++) {
                    int draw_x = cursor_x + k;
                    if (draw_x < 0) continue;
                    if (draw_x >= w) break;

                    unsigned char alpha = palpha[k]; 
                    
                    // 双通道 Alpha 融合 (U 和 V)
                    p[draw_x * 2 + 0] = (p[draw_x * 2 + 0] * (255 - alpha) + pen_color[0] * alpha) / 255;
                    p[draw_x * 2 + 1] = (p[draw_x * 2 + 1] * (255 - alpha) + pen_color[1] * alpha) / 255;
                }
            }
            cursor_x += advance_x; 
        }
        i += step; 
    }

    free(resized_font_bitmap);
}

// src color format(ARGB888) To dest format color
static unsigned int convert_color(unsigned int src_color, image_format_t dst_fmt)
{
    unsigned int dst_color = 0x0;
    unsigned char* p_src_color = (unsigned char*)&src_color;
    unsigned char* p_dst_color = (unsigned char*)&dst_color;
    
    // 强制使用 unsigned char，避免 RGB 值大于 127 时被误判为负数
    unsigned char r = p_src_color[2];
    unsigned char g = p_src_color[1];
    unsigned char b = p_src_color[0];
    unsigned char a = p_src_color[3];

    switch (dst_fmt)
    {
    case IMAGE_FORMAT_GRAY8:
        p_dst_color[0] = a;
        break;
    case IMAGE_FORMAT_RGB888:
        p_dst_color[0] = r;
        p_dst_color[1] = g;
        p_dst_color[2] = b;
        break;
    case IMAGE_FORMAT_RGBA8888:
        p_dst_color[0] = r;
        p_dst_color[1] = g;
        p_dst_color[2] = b;
        p_dst_color[3] = a;
        break;
    case IMAGE_FORMAT_YUV420SP_NV12:
        // YUV 转换公式：Y 通道范围 0-255，U/V 通道必须加上 128 偏移量
        p_dst_color[0] = (unsigned char)(0.299 * r + 0.587 * g + 0.114 * b);
        p_dst_color[1] = (unsigned char)(-0.169 * r - 0.331 * g + 0.500 * b + 128); // U
        p_dst_color[2] = (unsigned char)(0.500 * r - 0.419 * g - 0.081 * b + 128);  // V
        break;
    case IMAGE_FORMAT_YUV420SP_NV21:
        p_dst_color[0] = (unsigned char)(0.299 * r + 0.587 * g + 0.114 * b);
        p_dst_color[1] = (unsigned char)(0.500 * r - 0.419 * g - 0.081 * b + 128);  // V
        p_dst_color[2] = (unsigned char)(-0.169 * r - 0.331 * g + 0.500 * b + 128); // U
        break;
    default:
        break;
    }
    return dst_color;
}

static void draw_rectangle_c1(unsigned char* pixels, int w, int h, int rx, int ry, int rw, int rh, unsigned int color,
                              int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w;

    if (thickness == -1) {
        // filled
        for (int y = ry; y < ry + rh; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx; x < rx + rw; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x] = pen_color[0];
            }
        }

        return;
    }

    const int t0 = thickness / 2;
    const int t1 = thickness - t0;

    // draw top
    {
        for (int y = ry - t0; y < ry + t1; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx - t0; x < rx + rw + t1; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x] = pen_color[0];
            }
        }
    }

    // draw bottom
    {
        for (int y = ry + rh - t0; y < ry + rh + t1; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx - t0; x < rx + rw + t1; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x] = pen_color[0];
            }
        }
    }

    // draw left
    for (int x = rx - t0; x < rx + t1; x++) {
        if (x < 0)
            continue;

        if (x >= w)
            break;

        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            p[x] = pen_color[0];
        }
    }

    // draw right
    for (int x = rx + rw - t0; x < rx + rw + t1; x++) {
        if (x < 0)
            continue;

        if (x >= w)
            break;

        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            p[x] = pen_color[0];
        }
    }
}

static void draw_rectangle_c2(unsigned char* pixels, int w, int h, int rx, int ry, int rw, int rh, unsigned int color,
                              int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 2;

    if (thickness == -1) {
        // filled
        for (int y = ry; y < ry + rh; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx; x < rx + rw; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x * 2 + 0] = pen_color[0];
                p[x * 2 + 1] = pen_color[1];
            }
        }

        return;
    }

    const int t0 = thickness / 2;
    const int t1 = thickness - t0;

    // draw top
    {
        for (int y = ry - t0; y < ry + t1; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx - t0; x < rx + rw + t1; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x * 2 + 0] = pen_color[0];
                p[x * 2 + 1] = pen_color[1];
            }
        }
    }

    // draw bottom
    {
        for (int y = ry + rh - t0; y < ry + rh + t1; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx - t0; x < rx + rw + t1; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x * 2 + 0] = pen_color[0];
                p[x * 2 + 1] = pen_color[1];
            }
        }
    }

    // draw left
    for (int x = rx - t0; x < rx + t1; x++) {
        if (x < 0)
            continue;

        if (x >= w)
            break;

        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            p[x * 2 + 0] = pen_color[0];
            p[x * 2 + 1] = pen_color[1];
        }
    }

    // draw right
    for (int x = rx + rw - t0; x < rx + rw + t1; x++) {
        if (x < 0)
            continue;

        if (x >= w)
            break;

        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            p[x * 2 + 0] = pen_color[0];
            p[x * 2 + 1] = pen_color[1];
        }
    }
}

static void draw_rectangle_c3(unsigned char* pixels, int w, int h, int rx, int ry, int rw, int rh, unsigned int color,
                              int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 3;

    if (thickness == -1) {
        // filled
        for (int y = ry; y < ry + rh; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx; x < rx + rw; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x * 3 + 0] = pen_color[0];
                p[x * 3 + 1] = pen_color[1];
                p[x * 3 + 2] = pen_color[2];
            }
        }

        return;
    }

    const int t0 = thickness / 2;
    const int t1 = thickness - t0;

    // draw top
    {
        for (int y = ry - t0; y < ry + t1; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx - t0; x < rx + rw + t1; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x * 3 + 0] = pen_color[0];
                p[x * 3 + 1] = pen_color[1];
                p[x * 3 + 2] = pen_color[2];
            }
        }
    }

    // draw bottom
    {
        for (int y = ry + rh - t0; y < ry + rh + t1; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx - t0; x < rx + rw + t1; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x * 3 + 0] = pen_color[0];
                p[x * 3 + 1] = pen_color[1];
                p[x * 3 + 2] = pen_color[2];
            }
        }
    }

    // draw left
    for (int x = rx - t0; x < rx + t1; x++) {
        if (x < 0)
            continue;

        if (x >= w)
            break;

        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            p[x * 3 + 0] = pen_color[0];
            p[x * 3 + 1] = pen_color[1];
            p[x * 3 + 2] = pen_color[2];
        }
    }

    // draw right
    for (int x = rx + rw - t0; x < rx + rw + t1; x++) {
        if (x < 0)
            continue;

        if (x >= w)
            break;

        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            p[x * 3 + 0] = pen_color[0];
            p[x * 3 + 1] = pen_color[1];
            p[x * 3 + 2] = pen_color[2];
        }
    }
}

static void draw_rectangle_c4(unsigned char* pixels, int w, int h, int rx, int ry, int rw, int rh, unsigned int color,
                              int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 4;

    if (thickness == -1) {
        // filled
        for (int y = ry; y < ry + rh; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx; x < rx + rw; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x * 4 + 0] = pen_color[0];
                p[x * 4 + 1] = pen_color[1];
                p[x * 4 + 2] = pen_color[2];
                p[x * 4 + 3] = pen_color[3];
            }
        }

        return;
    }

    const int t0 = thickness / 2;
    const int t1 = thickness - t0;

    // draw top
    {
        for (int y = ry - t0; y < ry + t1; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx - t0; x < rx + rw + t1; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x * 4 + 0] = pen_color[0];
                p[x * 4 + 1] = pen_color[1];
                p[x * 4 + 2] = pen_color[2];
                p[x * 4 + 3] = pen_color[3];
            }
        }
    }

    // draw bottom
    {
        for (int y = ry + rh - t0; y < ry + rh + t1; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = rx - t0; x < rx + rw + t1; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                p[x * 4 + 0] = pen_color[0];
                p[x * 4 + 1] = pen_color[1];
                p[x * 4 + 2] = pen_color[2];
                p[x * 4 + 3] = pen_color[3];
            }
        }
    }

    // draw left
    for (int x = rx - t0; x < rx + t1; x++) {
        if (x < 0)
            continue;

        if (x >= w)
            break;

        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            p[x * 4 + 0] = pen_color[0];
            p[x * 4 + 1] = pen_color[1];
            p[x * 4 + 2] = pen_color[2];
            p[x * 4 + 3] = pen_color[3];
        }
    }

    // draw right
    for (int x = rx + rw - t0; x < rx + rw + t1; x++) {
        if (x < 0)
            continue;

        if (x >= w)
            break;

        for (int y = ry + t1; y < ry + rh - t0; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            p[x * 4 + 0] = pen_color[0];
            p[x * 4 + 1] = pen_color[1];
            p[x * 4 + 2] = pen_color[2];
            p[x * 4 + 3] = pen_color[3];
        }
    }
}

static void draw_rectangle_yuv420sp(unsigned char* yuv420sp, int w, int h, int rx, int ry, int rw, int rh,
                                    unsigned int color, int thickness)
{
    // assert w % 2 == 0
    // assert h % 2 == 0
    // assert rx % 2 == 0
    // assert ry % 2 == 0
    // assert rw % 2 == 0
    // assert rh % 2 == 0
    // assert thickness % 2 == 0

    const unsigned char* pen_color = (const unsigned char*)&color;

    unsigned int v_y;
    unsigned int v_uv;
    unsigned char* pen_color_y = (unsigned char*)&v_y;
    unsigned char* pen_color_uv = (unsigned char*)&v_uv;
    pen_color_y[0] = pen_color[0];
    pen_color_uv[0] = pen_color[1];
    pen_color_uv[1] = pen_color[2];

    unsigned char* Y = yuv420sp;
    draw_rectangle_c1(Y, w, h, rx, ry, rw, rh, v_y, thickness);

    unsigned char* UV = yuv420sp + w * h;
    int thickness_uv = thickness == -1 ? thickness : max(thickness / 2, 1);
    draw_rectangle_c2(UV, w / 2, h / 2, rx / 2, ry / 2, rw / 2, rh / 2, v_uv, thickness_uv);
}

static inline int distance_lessequal(int x0, int y0, int x1, int y1, float r)
{
    int dx = x0 - x1;
    int dy = y0 - y1;
    int q = dx * dx + dy * dy;
    return q <= r * r;
}

static inline int distance_inrange(int x0, int y0, int x1, int y1, float r0, float r1)
{
    int dx = x0 - x1;
    int dy = y0 - y1;
    int q = dx * dx + dy * dy;
    return q >= r0 * r0 && q < r1 * r1;
}

static void draw_circle_c1(unsigned char* pixels, int w, int h, int cx, int cy, int radius, unsigned int color,
                           int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w;

    if (thickness == -1) {
        // filled
        for (int y = cy - (radius - 1); y < cy + radius; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = cx - (radius - 1); x < cx + radius; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                // distance from cx cy
                if (distance_lessequal(x, y, cx, cy, radius)) {
                    p[x] = pen_color[0];
                }
            }
        }

        return;
    }

    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    for (int y = cy - (radius - 1) - t0; y < cy + radius + t1; y++) {
        if (y < 0)
            continue;

        if (y >= h)
            break;

        unsigned char* p = pixels + stride * y;

        for (int x = cx - (radius - 1) - t0; x < cx + radius + t1; x++) {
            if (x < 0)
                continue;

            if (x >= w)
                break;

            // distance from cx cy
            if (distance_inrange(x, y, cx, cy, radius - t0, radius + t1)) {
                p[x] = pen_color[0];
            }
        }
    }
}

static void draw_circle_c2(unsigned char* pixels, int w, int h, int cx, int cy, int radius, unsigned int color,
                           int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 2;

    if (thickness == -1) {
        // filled
        for (int y = cy - (radius - 1); y < cy + radius; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = cx - (radius - 1); x < cx + radius; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                // distance from cx cy
                if (distance_lessequal(x, y, cx, cy, radius)) {
                    p[x * 2 + 0] = pen_color[0];
                    p[x * 2 + 1] = pen_color[1];
                }
            }
        }

        return;
    }

    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    for (int y = cy - radius - t0; y < cy + radius + t1; y++) {
        if (y < 0)
            continue;

        if (y >= h)
            break;

        unsigned char* p = pixels + stride * y;

        for (int x = cx - radius - t0; x < cx + radius + t1; x++) {
            if (x < 0)
                continue;

            if (x >= w)
                break;

            // distance from cx cy
            if (distance_inrange(x, y, cx, cy, radius - t0, radius + t1)) {
                p[x * 2 + 0] = pen_color[0];
                p[x * 2 + 1] = pen_color[1];
            }
        }
    }
}

static void draw_circle_c3(unsigned char* pixels, int w, int h, int cx, int cy, int radius, unsigned int color,
                           int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 3;

    if (thickness == -1) {
        // filled
        for (int y = cy - (radius - 1); y < cy + radius; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = cx - (radius - 1); x < cx + radius; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                // distance from cx cy
                if (distance_lessequal(x, y, cx, cy, radius)) {
                    p[x * 3 + 0] = pen_color[0];
                    p[x * 3 + 1] = pen_color[1];
                    p[x * 3 + 2] = pen_color[2];
                }
            }
        }

        return;
    }

    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    for (int y = cy - radius - t0; y < cy + radius + t1; y++) {
        if (y < 0)
            continue;

        if (y >= h)
            break;

        unsigned char* p = pixels + stride * y;

        for (int x = cx - radius - t0; x < cx + radius + t1; x++) {
            if (x < 0)
                continue;

            if (x >= w)
                break;

            // distance from cx cy
            if (distance_inrange(x, y, cx, cy, radius - t0, radius + t1)) {
                p[x * 3 + 0] = pen_color[0];
                p[x * 3 + 1] = pen_color[1];
                p[x * 3 + 2] = pen_color[2];
            }
        }
    }
}

static void draw_circle_c4(unsigned char* pixels, int w, int h, int cx, int cy, int radius, unsigned int color,
                           int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 4;

    if (thickness == -1) {
        // filled
        for (int y = cy - (radius - 1); y < cy + radius; y++) {
            if (y < 0)
                continue;

            if (y >= h)
                break;

            unsigned char* p = pixels + stride * y;

            for (int x = cx - (radius - 1); x < cx + radius; x++) {
                if (x < 0)
                    continue;

                if (x >= w)
                    break;

                // distance from cx cy
                if (distance_lessequal(x, y, cx, cy, radius)) {
                    p[x * 4 + 0] = pen_color[0];
                    p[x * 4 + 1] = pen_color[1];
                    p[x * 4 + 2] = pen_color[2];
                    p[x * 4 + 3] = pen_color[3];
                }
            }
        }

        return;
    }

    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    for (int y = cy - (radius - 1) - t0; y < cy + radius + t1; y++) {
        if (y < 0)
            continue;

        if (y >= h)
            break;

        unsigned char* p = pixels + stride * y;

        for (int x = cx - (radius - 1) - t0; x < cx + radius + t1; x++) {
            if (x < 0)
                continue;

            if (x >= w)
                break;

            // distance from cx cy
            if (distance_inrange(x, y, cx, cy, radius - t0, radius + t1)) {
                p[x * 4 + 0] = pen_color[0];
                p[x * 4 + 1] = pen_color[1];
                p[x * 4 + 2] = pen_color[2];
                p[x * 4 + 3] = pen_color[3];
            }
        }
    }
}

static void draw_circle_yuv420sp(unsigned char* yuv420sp, int w, int h, int cx, int cy, int radius, unsigned int color,
                                 int thickness)
{
    // assert w % 2 == 0
    // assert h % 2 == 0
    // assert cx % 2 == 0
    // assert cy % 2 == 0
    // assert radius % 2 == 0
    // assert thickness % 2 == 0

    const unsigned char* pen_color = (const unsigned char*)&color;

    unsigned int v_y;
    unsigned int v_uv;
    unsigned char* pen_color_y = (unsigned char*)&v_y;
    unsigned char* pen_color_uv = (unsigned char*)&v_uv;
    pen_color_y[0] = pen_color[0];
    pen_color_uv[0] = pen_color[1];
    pen_color_uv[1] = pen_color[2];

    unsigned char* Y = yuv420sp;
    draw_circle_c1(Y, w, h, cx, cy, radius, v_y, thickness);

    unsigned char* UV = yuv420sp + w * h;
    int thickness_uv = thickness == -1 ? thickness : max(thickness / 2, 1);
    draw_circle_c2(UV, w / 2, h / 2, cx / 2, cy / 2, radius / 2, v_uv, thickness_uv);
}

static inline int distance_lessthan(int x, int y, int x0, int y0, int x1, int y1, float t)
{
    int dx01 = x1 - x0;
    int dy01 = y1 - y0;
    int dx0 = x - x0;
    int dy0 = y - y0;

    float r = (float)(dx0 * dx01 + dy0 * dy01) / (dx01 * dx01 + dy01 * dy01);

    if (r < 0 || r > 1)
        return 0;

    float px = x0 + dx01 * r;
    float py = y0 + dy01 * r;
    float dx = x - px;
    float dy = y - py;
    float p = dx * dx + dy * dy;
    return p < t;
}

static void draw_line_c1(unsigned char* pixels, int w, int h, int x0, int y0, int x1, int y1, unsigned int color,
                         int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w;

    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    int x_min = min(x0, x1);
    int x_max = max(x0, x1);
    int y_min = min(y0, y1);
    int y_max = max(y0, y1);

    for (int y = y_min - t0; y < y_max + t1; y++) {
        if (y < 0)
            continue;

        if (y >= h)
            break;

        unsigned char* p = pixels + stride * y;

        for (int x = x_min - t0; x < x_max + t1; x++) {
            if (x < 0)
                continue;

            if (x >= w)
                break;

            // distance from line
            if (distance_lessthan(x, y, x0, y0, x1, y1, t1)) {
                p[x] = pen_color[0];
            }
        }
    }
}

static void draw_line_c2(unsigned char* pixels, int w, int h, int x0, int y0, int x1, int y1, unsigned int color,
                         int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 2;

    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    int x_min = min(x0, x1);
    int x_max = max(x0, x1);
    int y_min = min(y0, y1);
    int y_max = max(y0, y1);

    for (int y = y_min - t0; y < y_max + t1; y++) {
        if (y < 0)
            continue;

        if (y >= h)
            break;

        unsigned char* p = pixels + stride * y;

        for (int x = x_min - t0; x < x_max + t1; x++) {
            if (x < 0)
                continue;

            if (x >= w)
                break;

            // distance from line
            if (distance_lessthan(x, y, x0, y0, x1, y1, t1)) {
                p[x * 2 + 0] = pen_color[0];
                p[x * 2 + 1] = pen_color[1];
            }
        }
    }
}

static void draw_line_c3(unsigned char* pixels, int w, int h, int x0, int y0, int x1, int y1, unsigned int color,
                         int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 3;

    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    int x_min = min(x0, x1);
    int x_max = max(x0, x1);
    int y_min = min(y0, y1);
    int y_max = max(y0, y1);

    for (int y = y_min - t0; y < y_max + t1; y++) {
        if (y < 0)
            continue;

        if (y >= h)
            break;

        unsigned char* p = pixels + stride * y;

        for (int x = x_min - t0; x < x_max + t1; x++) {
            if (x < 0)
                continue;

            if (x >= w)
                break;

            // distance from line
            if (distance_lessthan(x, y, x0, y0, x1, y1, t1)) {
                p[x * 3 + 0] = pen_color[0];
                p[x * 3 + 1] = pen_color[1];
                p[x * 3 + 2] = pen_color[2];
            }
        }
    }
}

static void draw_line_c4(unsigned char* pixels, int w, int h, int x0, int y0, int x1, int y1, unsigned int color,
                         int thickness)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 4;

    const float t0 = thickness / 2.f;
    const float t1 = thickness - t0;

    int x_min = min(x0, x1);
    int x_max = max(x0, x1);
    int y_min = min(y0, y1);
    int y_max = max(y0, y1);

    for (int y = y_min - t0; y < y_max + t1; y++) {
        if (y < 0)
            continue;

        if (y >= h)
            break;

        unsigned char* p = pixels + stride * y;

        for (int x = x_min - t0; x < x_max + t1; x++) {
            if (x < 0)
                continue;

            if (x >= w)
                break;

            // distance from line
            if (distance_lessthan(x, y, x0, y0, x1, y1, t1)) {
                p[x * 4 + 0] = pen_color[0];
                p[x * 4 + 1] = pen_color[1];
                p[x * 4 + 2] = pen_color[2];
                p[x * 4 + 3] = pen_color[3];
            }
        }
    }
}

static void draw_line_yuv420sp(unsigned char* yuv420sp, int w, int h, int x0, int y0, int x1, int y1,
                               unsigned int color, int thickness)
{
    // assert w % 2 == 0
    // assert h % 2 == 0
    // assert x0 % 2 == 0
    // assert y0 % 2 == 0
    // assert x1 % 2 == 0
    // assert y1 % 2 == 0
    // assert thickness % 2 == 0

    const unsigned char* pen_color = (const unsigned char*)&color;

    unsigned int v_y;
    unsigned int v_uv;
    unsigned char* pen_color_y = (unsigned char*)&v_y;
    unsigned char* pen_color_uv = (unsigned char*)&v_uv;
    pen_color_y[0] = pen_color[0];
    pen_color_uv[0] = pen_color[1];
    pen_color_uv[1] = pen_color[2];

    unsigned char* Y = yuv420sp;
    draw_line_c1(Y, w, h, x0, y0, x1, y1, v_y, thickness);

    unsigned char* UV = yuv420sp + w * h;
    int thickness_uv = thickness == -1 ? thickness : max(thickness / 2, 1);
    draw_line_c2(UV, w / 2, h / 2, x0 / 2, y0 / 2, x1 / 2, y1 / 2, v_uv, thickness_uv);
}

static void get_text_drawing_size(const char* text, int fontpixelsize, int* w, int* h)
{
    *w = 0;
    *h = 0;

    const int n = strlen(text);

    int line_w = 0;
    for (int i = 0; i < n; i++) {
        char ch = text[i];

        if (ch == '\n') {
            // newline
            *w = max(*w, line_w);
            *h += fontpixelsize * 2;
            line_w = 0;
        }

        if (isprint(ch) != 0) {
            line_w += fontpixelsize;
        }
    }

    *w = max(*w, line_w);
    *h += fontpixelsize * 2;
}

static int resize_bilinear_c1(const unsigned char* src_pixels, int w, int h, unsigned char* dst_pixels, int w2, int h2)
{
    int A, B, C, D, x, y, index, gray;
    float x_ratio = ((float)(w - 1)) / w2;
    float y_ratio = ((float)(h - 1)) / h2;
    float x_diff, y_diff, ya, yb;
    int offset = 0;
    for (int i = 0; i < h2; i++) {
        for (int j = 0; j < w2; j++) {
            x = (int)(x_ratio * j);
            y = (int)(y_ratio * i);
            x_diff = (x_ratio * j) - x;
            y_diff = (y_ratio * i) - y;
            index = y * w + x;

            // range is 0 to 255 thus bitwise AND with 0xff
            A = src_pixels[index] & 0xff;
            B = src_pixels[index + 1] & 0xff;
            C = src_pixels[index + w] & 0xff;
            D = src_pixels[index + w + 1] & 0xff;

            // Y = A(1-w)(1-h) + B(w)(1-h) + C(h)(1-w) + Dwh
            gray = (int)(A * (1 - x_diff) * (1 - y_diff) + B * (x_diff) * (1 - y_diff) + C * (y_diff) * (1 - x_diff) +
                         D * (x_diff * y_diff));

            dst_pixels[offset++] = gray;
        }
    }
    return 0;
}
/*
static void draw_text_c1(unsigned char* pixels, int w, int h, const char* text, int x, int y, int fontpixelsize,
                         unsigned int color)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w;

    unsigned char* resized_font_bitmap = malloc(fontpixelsize * fontpixelsize * 2);

    const int n = strlen(text);

    int cursor_x = x;
    int cursor_y = y;
    for (int i = 0; i < n; i++) {
        char ch = text[i];

        if (ch == '\n') {
            // newline
            cursor_x = x;
            cursor_y += fontpixelsize * 2;
        }

        if (isprint(ch) != 0) {
            const unsigned char* font_bitmap = mono_font_data[ch - ' '];

            // draw resized character
            resize_bilinear_c1(font_bitmap, 20, 40, resized_font_bitmap, fontpixelsize, fontpixelsize * 2);

            for (int j = cursor_y; j < cursor_y + fontpixelsize * 2; j++) {
                if (j < 0)
                    continue;

                if (j >= h)
                    break;

                const unsigned char* palpha = resized_font_bitmap + (j - cursor_y) * fontpixelsize;
                unsigned char* p = pixels + stride * j;

                for (int k = cursor_x; k < cursor_x + fontpixelsize; k++) {
                    if (k < 0)
                        continue;

                    if (k >= w)
                        break;

                    unsigned char alpha = palpha[k - cursor_x];

                    p[k] = (p[k] * (255 - alpha) + pen_color[0] * alpha) / 255;
                }
            }

            cursor_x += fontpixelsize;
        }
    }

    free(resized_font_bitmap);
}
*/
/*
static void draw_text_c2(unsigned char* pixels, int w, int h, const char* text, int x, int y, int fontpixelsize,
                         unsigned int color)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 2;

    unsigned char* resized_font_bitmap = malloc(fontpixelsize * fontpixelsize * 2);

    const int n = strlen(text);

    int cursor_x = x;
    int cursor_y = y;
    for (int i = 0; i < n; i++) {
        char ch = text[i];

        if (ch == '\n') {
            // newline
            cursor_x = x;
            cursor_y += fontpixelsize * 2;
        }

        if (isprint(ch) != 0) {
            int font_bitmap_index = ch - ' ';
            const unsigned char* font_bitmap = mono_font_data[font_bitmap_index];

            // draw resized character
            resize_bilinear_c1(font_bitmap, 20, 40, resized_font_bitmap, fontpixelsize, fontpixelsize * 2);

            for (int j = cursor_y; j < cursor_y + fontpixelsize * 2; j++) {
                if (j < 0)
                    continue;

                if (j >= h)
                    break;

                const unsigned char* palpha = resized_font_bitmap + (j - cursor_y) * fontpixelsize;
                unsigned char* p = pixels + stride * j;

                for (int k = cursor_x; k < cursor_x + fontpixelsize; k++) {
                    if (k < 0)
                        continue;

                    if (k >= w)
                        break;

                    unsigned char alpha = palpha[k - cursor_x];

                    p[k * 2 + 0] = (p[k * 2 + 0] * (255 - alpha) + pen_color[0] * alpha) / 255;
                    p[k * 2 + 1] = (p[k * 2 + 1] * (255 - alpha) + pen_color[1] * alpha) / 255;
                }
            }

            cursor_x += fontpixelsize;
        }
    }

    free(resized_font_bitmap);
}
*/

/*
static void draw_text_c3(unsigned char* pixels, int w, int h, const char* text, int x, int y, int fontpixelsize,
                         unsigned int color)
{
    const unsigned char* pen_color = (const unsigned char*)&color;
    int stride = w * 3;

    unsigned char* resized_font_bitmap = malloc(fontpixelsize * fontpixelsize * 2);

    const int n = strlen(text);

    int cursor_x = x;
    int cursor_y = y;
    for (int i = 0; i < n; i++) {
        char ch = text[i];

        if (ch == '\n') {
            // newline
            cursor_x = x;
            cursor_y += fontpixelsize * 2;
        }

        if (isprint(ch) != 0) {
            int font_bitmap_index = ch - ' ';
            const unsigned char* font_bitmap = mono_font_data[font_bitmap_index];

            // draw resized character
            resize_bilinear_c1(font_bitmap, 20, 40, resized_font_bitmap, fontpixelsize, fontpixelsize * 2);

            for (int j = cursor_y; j < cursor_y + fontpixelsize * 2; j++) {
                if (j < 0)
                    continue;

                if (j >= h)
                    break;

                const unsigned char* palpha = resized_font_bitmap + (j - cursor_y) * fontpixelsize;
                unsigned char* p = pixels + stride * j;

                for (int k = cursor_x; k < cursor_x + fontpixelsize; k++) {
                    if (k < 0)
                        continue;

                    if (k >= w)
                        break;

                    unsigned char alpha = palpha[k - cursor_x];

                    p[k * 3 + 0] = (p[k * 3 + 0] * (255 - alpha) + pen_color[0] * alpha) / 255;
                    p[k * 3 + 1] = (p[k * 3 + 1] * (255 - alpha) + pen_color[1] * alpha) / 255;
                    p[k * 3 + 2] = (p[k * 3 + 2] * (255 - alpha) + pen_color[2] * alpha) / 255;
                }
            }

            cursor_x += fontpixelsize;
        }
    }

    free(resized_font_bitmap);
}*/


static void draw_text_yuv420sp(unsigned char* yuv420sp, int w, int h, const char* text, int x, int y, int fontpixelsize,
                               unsigned int color)
{
    // assert w % 2 == 0
    // assert h % 2 == 0
    // assert x % 2 == 0
    // assert y % 2 == 0
    // assert fontpixelsize % 2 == 0

    const unsigned char* pen_color = (const unsigned char*)&color;

    unsigned int v_y;
    unsigned int v_uv;
    unsigned char* pen_color_y = (unsigned char*)&v_y;
    unsigned char* pen_color_uv = (unsigned char*)&v_uv;
    pen_color_y[0] = pen_color[0];
    pen_color_uv[0] = pen_color[1];
    pen_color_uv[1] = pen_color[2];

    unsigned char* Y = yuv420sp;
    draw_text_c1(Y, w, h, text, x, y, fontpixelsize, v_y);

    unsigned char* UV = yuv420sp + w * h;
    draw_text_c2(UV, w / 2, h / 2, text, x / 2, y / 2, max(fontpixelsize / 2, 1), v_uv);
}

static void draw_image_c1(unsigned char* pixels, int w, int h, unsigned char* draw_img, int x, int y, int rw, int rh)
{
    for (int i = 0; i < rh; i++) {
        memcpy(pixels + (y + i) * w + x,  draw_img + i * rw,  rw);
    }
}

static void draw_image_c2(unsigned char* pixels, int w, int h, unsigned char* draw_img, int x, int y, int rw, int rh)
{
    for (int i = 0; i < rh; i++) {
        memcpy(pixels + ((y + i) * w + x) * 2,  draw_img + i * rw * 2,  rw * 2);
    }
}

static void draw_image_c3(unsigned char* pixels, int w, int h, unsigned char* draw_img, int x, int y, int rw, int rh)
{
    printf("draw_image_c3 pixels=%p wxh=%dx%d draw_img=%p pos=(%d %d) rwxrh=%dx%d\n", pixels, w, h, draw_img, x, y, rw, rh);
    for (int i = 0; i < rh; i++) {
        memcpy(pixels + ((y + i) * w + x) * 3,  draw_img + i * rw * 3,  rw * 3);
    }
}

static void draw_image_c4(unsigned char* pixels, int w, int h, unsigned char* draw_img, int x, int y, int rw, int rh)
{
    for (int i = 0; i < rh; i++) {
        memcpy(pixels + ((y + i) * w + x) * 4,  draw_img + i * rw * 4,  rw * 4);
    }
}

static void draw_image_yuv420sp(unsigned char* pixels, int w, int h, unsigned char* draw_img, int x, int y, int rw, int rh)
{
    draw_image_c1(pixels, w, h, draw_img, x, y, rw, rh);
    draw_image_c2(pixels, w, h / 2, draw_img + rw * rh, x, y/2, rw, rh/2);
}

void draw_rectangle(image_buffer_t* image, int rx, int ry, int rw, int rh, unsigned int color,
                      int thickness)
{
    image_format_t format = image->format;
    unsigned char* pixels = image->virt_addr;
    int w = image->width;
    int h = image->height;

    unsigned int draw_color = convert_color(color, format);
    // printf("draw_color=%x\n", draw_color);

    // printf("draw_rectangle format=%d rx=%d ry=%d rw=%d rh=%d color=0x%x thickness=%d\n",
    //     format, rx, ry, rw, rh, color, thickness);
    switch (format)
    {
    case IMAGE_FORMAT_RGB888:
        draw_rectangle_c3(pixels, w, h, rx, ry, rw, rh, draw_color, thickness);
        break;
    case IMAGE_FORMAT_RGBA8888:
        draw_rectangle_c4(pixels, w, h, rx, ry, rw, rh, draw_color, thickness);
        break;
    case IMAGE_FORMAT_YUV420SP_NV12:
    case IMAGE_FORMAT_YUV420SP_NV21:
        draw_rectangle_yuv420sp(pixels, w, h, rx, ry, rw, rh, draw_color, thickness);
        break;
    default:
        printf("no support format %d", format);
        break;
    }
}

void draw_line(image_buffer_t* image, int x0, int y0, int x1, int y1, unsigned int color,
                 int thickness)
{
    image_format_t format = image->format;
    unsigned char* pixels = image->virt_addr;
    int w = image->width;
    int h = image->height;

    unsigned draw_color = convert_color(color, format);

    switch (format)
    {
    case IMAGE_FORMAT_RGB888:
        draw_line_c3(pixels, w, h, x0, y0, x1, y1, draw_color, thickness);
        break;
    case IMAGE_FORMAT_RGBA8888:
        draw_line_c4(pixels, w, h, x0, y0, x1, y1, draw_color, thickness);
        break;
    case IMAGE_FORMAT_YUV420SP_NV12:
    case IMAGE_FORMAT_YUV420SP_NV21:
        draw_line_yuv420sp(pixels, w, h, x0, y0, x1, y1, draw_color, thickness);
        break;
    default:
        printf("no support format %d", format);
        break;
    }
}

void rbbox_to_corners(const float *in_rbbox, int *out_rbbox) {
    // generate clockwise corners and rotate it clockwise
    // 顺时针方向返回角点位置
    float cx = in_rbbox[0] + in_rbbox[2] / 2;
    float cy = in_rbbox[1] + in_rbbox[3] / 2;
    float x_d = in_rbbox[2];
    float y_d = in_rbbox[3];
    float angle = in_rbbox[4];
    float a_cos = cos(angle);
    float a_sin = sin(angle);
    float corners_x[4] = {-x_d / 2, -x_d / 2, x_d / 2, x_d / 2};
    float corners_y[4] = {-y_d / 2, y_d / 2, y_d / 2, -y_d / 2};
    for (int i = 0; i < 4; ++i) {
        out_rbbox[2 * i] = (int)(a_cos * corners_x[i] - a_sin * corners_y[i] + cx);
        out_rbbox[2 * i + 1] = (int)(a_sin * corners_x[i] + a_cos * corners_y[i] + cy);
    }
}

void draw_obb_rectangle(image_buffer_t *image, int rx, int ry, int rw, int rh, float angle, unsigned int color,
                        int thickness) {
    float in_bbox[5] = {(float)(rx), (float)(ry), (float)(rw), (float)(rh), angle};
    int out_box_corners[8];
    rbbox_to_corners(in_bbox, out_box_corners);
    for(int i = 0 ; i < 4; i++) {
        int index1 = i;
        int index2 = (i + 1) % 4;
        draw_line(image, out_box_corners[index1 * 2], out_box_corners[index1 * 2 + 1],
                  out_box_corners[index2 * 2], out_box_corners[index2 * 2 + 1], color, thickness);
    }
}

void draw_text(image_buffer_t* image, const char* text, int x, int y, unsigned int color,
                 int fontsize)
{
    image_format_t format = image->format;
    unsigned char* pixels = image->virt_addr;
    int w = image->width;
    int h = image->height;
    unsigned draw_color = convert_color(color, format);

    switch (format)
    {
    case IMAGE_FORMAT_RGB888:
        draw_text_c3(pixels, w, h, text, x, y, fontsize, draw_color);
        break;
    case IMAGE_FORMAT_RGBA8888:
        {
            // 提取真实的宽度步幅，如果没有设置则默认使用 width
            // RGBA8888 每个像素占 4 字节，因此 stride_bytes = 像素步幅 * 4
            int stride_bytes = (image->width_stride > 0) ? (image->width_stride * 4) : (w * 4);
            draw_text_c4(pixels, w, h, stride_bytes, text, x, y, fontsize, draw_color);
        }
        break;
        break;
    case IMAGE_FORMAT_YUV420SP_NV12:
    case IMAGE_FORMAT_YUV420SP_NV21:
        draw_text_yuv420sp(pixels, w, h, text, x, y, fontsize, draw_color);
        break;
    default:
        printf("no support format %d", format);
        break;
    }
}

void draw_circle(image_buffer_t* image, int cx, int cy, int radius, unsigned int color,
                 int thickness)
{
    image_format_t format = image->format;
    unsigned char* pixels = image->virt_addr;
    int w = image->width;
    int h = image->height;
    unsigned draw_color = convert_color(color, format);

    switch (format)
    {
    case IMAGE_FORMAT_RGB888:
        draw_circle_c3(pixels, w, h, cx, cy, radius, draw_color, thickness);
        break;
    case IMAGE_FORMAT_RGBA8888:
        draw_circle_c4(pixels, w, h, cx, cy, radius, draw_color, thickness);
        break;
    case IMAGE_FORMAT_YUV420SP_NV12:
    case IMAGE_FORMAT_YUV420SP_NV21:
        draw_circle_yuv420sp(pixels, w, h, cx, cy, radius, draw_color, thickness);
        break;
    default:
        printf("no support format %d", format);
        break;
    }
}

void draw_image(image_buffer_t* image, unsigned char* draw_img, int x, int y, int rw, int rh)
{
    image_format_t format = image->format;
    unsigned char* pixels = image->virt_addr;
    int w = image->width;
    int h = image->height;

    switch (format)
    {
    case IMAGE_FORMAT_RGB888:
        draw_image_c3(pixels, w, h, draw_img, x, y, rw, rh);
        break;
    case IMAGE_FORMAT_RGBA8888:
        draw_image_c4(pixels, w, h, draw_img, x, y, rw, rh);
        break;
    case IMAGE_FORMAT_YUV420SP_NV12:
    case IMAGE_FORMAT_YUV420SP_NV21:
        draw_image_yuv420sp(pixels, w, h, draw_img, x, y, rw, rh);
        break;
    default:
        printf("no support format %d", format);
        break;
    }
}
