#include "pcie_qt_ui_helpers.h"

#include <cstdint>

#include <QApplication>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QListWidget>
#include <QSet>
#include <QStringList>
#include <QWidget>

namespace pcie_qt_ui {

QString Zh(const char* text) {
    return QString::fromUtf8(text);
}

void LoadChineseFont(QApplication* app) {
    const QString app_dir = QApplication::applicationDirPath();
    const QStringList font_paths = {
        app_dir + "/simhei.ttf",
        app_dir + "/model/simhei.ttf",
    };

    for (const QString& path : font_paths) {
        if (!QFileInfo::exists(path)) {
            continue;
        }
        const int font_id = QFontDatabase::addApplicationFont(path);
        const QStringList families = QFontDatabase::applicationFontFamilies(font_id);
        if (font_id >= 0 && !families.isEmpty()) {
            app->setFont(QFont(families.first(), 14));
            return;
        }
    }

    app->setFont(QFont(Zh("黑体"), 14));
}

QImage Bgr565ToRgb888(const std::vector<unsigned char>& pixels, int width, int height) {
    QImage image(width, height, QImage::Format_RGB888);
    for (int y = 0; y < height; ++y) {
        unsigned char* dst = image.scanLine(y);
        const unsigned char* src = pixels.data() + static_cast<size_t>(y) * width * 2U;
        for (int x = 0; x < width; ++x) {
            const uint16_t value = static_cast<uint16_t>(src[0]) |
                                   (static_cast<uint16_t>(src[1]) << 8);
            const unsigned int r5 = value & 0x1fU;
            const unsigned int g6 = (value >> 5) & 0x3fU;
            const unsigned int b5 = (value >> 11) & 0x1fU;
            dst[0] = static_cast<unsigned char>((r5 << 3) | (r5 >> 2));
            dst[1] = static_cast<unsigned char>((g6 << 2) | (g6 >> 4));
            dst[2] = static_cast<unsigned char>((b5 << 3) | (b5 >> 2));
            dst += 3;
            src += 2;
        }
    }
    return image;
}

void ApplyTrafficStyle(QWidget* widget) {
    widget->setStyleSheet(
        "QMainWindow, QWidget { background: #11151b; color: #e9eef2;"
        " font-family: SimHei, \"WenQuanYi Micro Hei\", \"Noto Sans CJK SC\", sans-serif;"
        " font-size: 16px; }"
        "#videoLabel { background: #05080c; border: 1px solid #2a3440; }"
        "#titleLabel { font-size: 28px; font-weight: 700; color: #f7fbff; }"
        "#modeBadgeLabel { background: #16202a; border: 1px solid #2c4253;"
        " color: #35d690; padding: 6px; font-weight: 700; }"
        "#resultListWidget { background: #0b1016; border: 1px solid #2a3440;"
        " color: #f4f7fa; font-size: 22px; font-weight: 700; }"
        "QComboBox { background: #18222c; border: 1px solid #304356;"
        " color: #f4f7fa; padding: 4px 8px; }"
        "QGroupBox { border: 1px solid #2a3440; margin-top: 12px; padding: 12px 8px 8px 8px;"
        " font-weight: 700; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }"
        "#fpsKeyLabel, #capturedKeyLabel, #inferenceKeyLabel,"
        " #deviceKeyLabel, #linkKeyLabel, #payloadKeyLabel, #stateKeyLabel { color: #94a4b5; }"
        "#fpsValueLabel, #capturedValueLabel, #inferenceValueLabel,"
        " #deviceValueLabel, #linkValueLabel, #payloadValueLabel,"
        " #stateValueLabel { color: #f4f7fa; font-weight: 600; }"
        "QPushButton { background: #1c8b6c; color: white; border: 0; font-weight: 700; }"
        "QPushButton:pressed { background: #156f57; }"
        "QStatusBar { background: #11151b; color: #94a4b5; }");
}

QString ModeBadgeText(const QString& mode) {
    return Zh("当前模式：%1").arg(mode);
}

void AddUniqueRecognitionResult(QListWidget* list,
                                QSet<QString>* known_results,
                                bool* placeholder_visible,
                                const QString& text) {
    const QString normalized = text.trimmed();
    if (normalized.isEmpty() || known_results->contains(normalized)) {
        return;
    }

    if (*placeholder_visible) {
        list->clear();
        *placeholder_visible = false;
    }

    known_results->insert(normalized);
    list->addItem(Zh("%1. %2").arg(list->count() + 1).arg(normalized));
    list->scrollToBottom();
}

}  // namespace pcie_qt_ui
