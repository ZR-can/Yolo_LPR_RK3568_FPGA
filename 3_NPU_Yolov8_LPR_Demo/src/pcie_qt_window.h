#ifndef PCIE_QT_WINDOW_H_
#define PCIE_QT_WINDOW_H_

#include <cstdint>

#include <QMainWindow>
#include <QMetaType>
#include <QSet>
#include <QString>

#include "ui_mainwindow.h"

class QCloseEvent;
class QImage;
class PcieQtWorker;

struct UiStatusSnapshot {
    bool worker_alive = false;
    bool capturing = false;
    bool pcie_open = false;
    int frame_id = 0;
    double fps = 0.0;
    uint64_t start_ms = 0;
    uint64_t captured_frames = 0;
    uint64_t frame_pool_drops = 0;
    uint64_t displayed_frames = 0;
    uint64_t display_queue_drops = 0;
    uint64_t display_failures = 0;
    uint64_t overlay_failures = 0;
    uint64_t inference_jobs = 0;
    uint64_t inference_queue_drops = 0;
    uint64_t inference_failures = 0;
    uint64_t plate_results = 0;
    uint64_t zero_status_retries = 0;
    uint64_t permission_retries = 0;
    uint64_t interrupted_retries = 0;
    uint64_t other_errors = 0;
    double inference_ms = 0.0;
    double display_convert_ms = 0.0;
    double overlay_ms = 0.0;
    double present_ms = 0.0;
    double end_to_end_ms = 0.0;
    unsigned int vendor_id = 0;
    unsigned int device_id = 0;
    unsigned int link_speed = 0;
    unsigned int link_width = 0;
    unsigned int max_payload_size = 0;
    QString plate_text;
    QString message;
};

Q_DECLARE_METATYPE(UiStatusSnapshot)

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(const QString& yolov8_model,
               const QString& lprnet7_model,
               const QString& lprnet8_model,
               QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void ToggleCapture();
    void OnFrameReady(const QImage& image, const UiStatusSnapshot& status);
    void OnWorkerFinished(const UiStatusSnapshot& status);
    void OnModeChanged(const QString& mode);

private:
    void StartWorker();
    void ShutdownWorker();
    void ApplyStatus(const UiStatusSnapshot& status);

    Ui::MainWindow ui_;
    QString yolov8_model_;
    QString lprnet7_model_;
    QString lprnet8_model_;
    PcieQtWorker* worker_;
    bool capture_enabled_;
    QSet<QString> known_plates_;
    bool result_placeholder_visible_;
};

#endif  // PCIE_QT_WINDOW_H_
