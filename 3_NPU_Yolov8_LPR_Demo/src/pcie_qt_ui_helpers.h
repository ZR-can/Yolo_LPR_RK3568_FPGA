#ifndef PCIE_QT_UI_HELPERS_H_
#define PCIE_QT_UI_HELPERS_H_

#include <vector>

#include <QImage>
#include <QString>
#include <QSet>

class QApplication;
class QListWidget;
class QWidget;

namespace pcie_qt_ui {

QString Zh(const char* text);
void LoadChineseFont(QApplication* app);
QImage Bgr565ToRgb888(const std::vector<unsigned char>& pixels, int width, int height);
void ApplyTrafficStyle(QWidget* widget);
QString ModeBadgeText(const QString& mode);
void AddUniqueRecognitionResult(QListWidget* list,
                                QSet<QString>* known_results,
                                bool* placeholder_visible,
                                const QString& text);

}  // namespace pcie_qt_ui

#endif  // PCIE_QT_UI_HELPERS_H_
