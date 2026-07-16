#include <cstdio>

#include <QApplication>
#include <QString>

#include "pcie_qt_ui_helpers.h"
#include "pcie_qt_window.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    pcie_qt_ui::LoadChineseFont(&app);

    if (argc != 4) {
        fprintf(stderr,
                "用法: %s <yolov8模型> <7位车牌模型> <8位车牌模型>\n",
                argv[0]);
        return 1;
    }

    MainWindow window(QString::fromLocal8Bit(argv[1]),
                      QString::fromLocal8Bit(argv[2]),
                      QString::fromLocal8Bit(argv[3]));
    window.show();
    return app.exec();
}
