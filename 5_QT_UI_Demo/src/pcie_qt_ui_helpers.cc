#include "pcie_qt_ui_helpers.h"

#include <QApplication>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QStringList>
#include <QWidget>

namespace pcie_qt_ui {

QString Zh(const char* text) {
    return QString::fromUtf8(text);
}

void LoadChineseFont(QApplication* app) {
    const QString app_dir = QApplication::applicationDirPath();
    const QStringList font_paths = {
        app_dir + "/assets/fonts/simhei.ttf",
        app_dir + "/simhei.ttf",
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

void ApplyTrafficStyle(QWidget* widget) {
    widget->setStyleSheet(
        "QMainWindow, QWidget { background: #11151b; color: #e9eef2;"
        " font-family: SimHei, \"WenQuanYi Micro Hei\", \"Noto Sans CJK SC\", sans-serif;"
        " font-size: 16px; }"
        "#videoPanel, #videoLabel { background: #05080c; border: 0; }"
        "#videoBorderFrame { background: transparent; border: 2px solid #4f6b82; }"
        "#videoLeftEdgeFrame { background: transparent; border: 0;"
        " border-left: 8px solid #010204; border-top: 8px solid #010204;"
        " border-bottom: 8px solid #010204; }"
        "#leftTopStatusPanel { background: #0d1218; border-bottom: 1px solid #2a3440; }"
        "#leftSystemPanel { background: #0d1218; border-top: 1px solid #2a3440; }"
        "#sidePanel { background: #111820; border-left: 1px solid #2a3440; }"
        "#titleLabel { font-size: 28px; font-weight: 700; color: #f7fbff; }"
        "#videoModeButton, #imageModeButton, #trafficModeButton {"
        " background: #16202a; border: 1px solid #304356; color: #c8d3dc;"
        " font-size: 18px; font-weight: 700; }"
        "#videoModeButton:checked, #imageModeButton:checked, #trafficModeButton:checked {"
        " background: #1c8b6c; border-color: #35d690; color: #ffffff; }"
        "#videoModeButton:disabled, #imageModeButton:disabled, #trafficModeButton:disabled {"
        " background: #121920; border-color: #25333f; color: #647585; }"
        "#operationMessageLabel { background: #16202a; border: 1px solid #2c4253;"
        " color: #35d690; padding: 8px; font-size: 20px; font-weight: 700; }"
        "#resultTableWidget { background: #0b1016; alternate-background-color: #101821;"
        " border: 1px solid #2a3440; color: #f4f7fa; font-size: 30px; font-weight: 700;"
        " selection-background-color: #1c8b6c; selection-color: #ffffff; }"
        "#resultTableWidget QHeaderView::section { background: #16202a; color: #b8c7d3;"
        " border: 0; border-bottom: 1px solid #2a3440; padding: 8px;"
        " font-size: 24px; font-weight: 700; }"
        "QGroupBox { border: 1px solid #2a3440; margin-top: 12px; padding: 12px 8px 8px 8px;"
        " font-weight: 700; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }"
        "#runtimeGroup, #pcieGroup, #systemMonitorGroup { font-size: 18px; }"
        "#fpsKeyLabel, #capturedKeyLabel, #inferenceKeyLabel, #latencyKeyLabel,"
        " #deviceKeyLabel, #linkKeyLabel, #payloadKeyLabel, #stateKeyLabel {"
        " color: #94a4b5; font-size: 18px; font-weight: 600; }"
        "#fpsValueLabel, #capturedValueLabel, #inferenceValueLabel, #latencyValueLabel,"
        " #deviceValueLabel, #linkValueLabel, #payloadValueLabel,"
        " #stateValueLabel { color: #f4f7fa; font-size: 22px; font-weight: 700; }"
        "QPushButton { background: #1c8b6c; color: white; border: 0;"
        " font-size: 18px; font-weight: 700; }"
        "QPushButton:pressed { background: #156f57; }");
}

}  // namespace pcie_qt_ui
