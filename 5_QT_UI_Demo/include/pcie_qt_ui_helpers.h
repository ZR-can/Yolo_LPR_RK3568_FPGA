#ifndef PCIE_QT_UI_HELPERS_H_
#define PCIE_QT_UI_HELPERS_H_

#include <QString>

class QApplication;
class QWidget;

namespace pcie_qt_ui {

QString Zh(const char* text);
void LoadChineseFont(QApplication* app);
void ApplyTrafficStyle(QWidget* widget);

}  // namespace pcie_qt_ui

#endif  // PCIE_QT_UI_HELPERS_H_
