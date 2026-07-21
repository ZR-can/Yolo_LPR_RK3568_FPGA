#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdint>

#include <QApplication>
#include <QAbstractItemView>
#include <QCloseEvent>
#include <QComboBox>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QImage>
#include <QMainWindow>
#include <QMetaType>
#include <QPixmap>
#include <QPushButton>
#include <QSize>
#include <QSizePolicy>
#include <QSet>
#include <QStringList>
#include <QStatusBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>

#include "pcie_demo_bridge.h"
#include "pcie_qt_ui_helpers.h"
#include "ui_mainwindow.h"

Q_DECLARE_METATYPE(PcieUiStatus)
Q_DECLARE_METATYPE(PcieUiFrame)

namespace {

// Keep only a tiny queued backlog so Qt can absorb short paint jitter without
// letting stale frames pile up behind the live PCIe stream.
const int kMaxPendingUiFrames = 2;

}  // namespace

class PcieQtWorker : public QThread {
    Q_OBJECT

public:
    PcieQtWorker(const QString& yolov8_model,
                 const QString& lprnet7_model,
                 const QString& lprnet8_model,
                 QObject* parent = nullptr)
        : QThread(parent),
          yolov8_model_(yolov8_model),
          lprnet7_model_(lprnet7_model),
          lprnet8_model_(lprnet8_model),
          stop_requested_(false),
          capture_enabled_(true),
          pending_frame_events_(0),
          last_status_emit_ms_(0) {}

    void SetCaptureEnabled(bool enabled) {
        capture_enabled_.store(enabled);
    }

    void RequestExit() {
        stop_requested_.store(true);
        capture_enabled_.store(false);
    }

    void MarkFrameEventConsumed() {
        int pending = pending_frame_events_.load();
        while (pending > 0 &&
               !pending_frame_events_.compare_exchange_weak(pending, pending - 1)) {
        }
    }

signals:
    void FrameReady(const PcieUiFrame& frame, const PcieUiStatus& status);
    void StatusReady(const PcieUiStatus& status);
    void WorkerFinished(const PcieUiStatus& status);

protected:
    void run() override {
        PcieUiCallbacks callbacks;
        callbacks.should_stop = [this]() { return stop_requested_.load(); };
        callbacks.capture_enabled = [this]() { return capture_enabled_.load(); };
        callbacks.can_accept_frame = [this]() {
            return pending_frame_events_.load() < kMaxPendingUiFrames;
        };
        callbacks.on_frame = [this](const PcieUiFrame& frame, const PcieUiStatus& status) {
            if (frame.pixels == nullptr || frame.pixels->empty()) {
                return;
            }
            if (!TryReserveFrameEvent()) {
                return;
            }
            emit FrameReady(frame, status);
        };
        callbacks.on_status = [this](const PcieUiStatus& status) {
            const bool has_message = !status.message.empty();
            const uint64_t elapsed_ms = status.elapsed_ms;
            const uint64_t last_ms = last_status_emit_ms_.load();
            if (!has_message && elapsed_ms >= last_ms && elapsed_ms - last_ms < 250U) {
                return;
            }
            last_status_emit_ms_.store(elapsed_ms);
            emit StatusReady(status);
        };

        const int ret = RunPcieDemo(yolov8_model_.toLocal8Bit().constData(),
                                    lprnet7_model_.toLocal8Bit().constData(),
                                    lprnet8_model_.toLocal8Bit().constData(),
                                    &callbacks);

        PcieUiStatus status;
        status.message = (ret == 0) ? "已停止" : "已停止（异常）";
        emit WorkerFinished(status);
    }

private:
    bool TryReserveFrameEvent() {
        int pending = pending_frame_events_.load();
        while (pending < kMaxPendingUiFrames) {
            if (pending_frame_events_.compare_exchange_weak(pending, pending + 1)) {
                return true;
            }
        }
        return false;
    }

    QString yolov8_model_;
    QString lprnet7_model_;
    QString lprnet8_model_;
    std::atomic<bool> stop_requested_;
    std::atomic<bool> capture_enabled_;
    std::atomic<int> pending_frame_events_;
    std::atomic<uint64_t> last_status_emit_ms_;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(const QString& yolov8_model,
               const QString& lprnet7_model,
               const QString& lprnet8_model,
               QWidget* parent = nullptr)
        : QMainWindow(parent),
          yolov8_model_(yolov8_model),
          lprnet7_model_(lprnet7_model),
          lprnet8_model_(lprnet8_model),
          worker_(nullptr),
          capture_enabled_(false),
          pcie_fps_(0.0),
          display_fps_(0.0),
          inference_fps_(0.0),
          ui_painted_frames_(0),
          last_fps_elapsed_ms_(0),
          last_fps_captured_frames_(0),
          last_fps_ui_painted_frames_(0),
          last_fps_inference_jobs_(0) {
        ui_.setupUi(this);
        setWindowTitle(pcie_qt_ui::Zh("交通识别系统"));
        ui_.titleLabel->setText(pcie_qt_ui::Zh("交通识别系统"));
        ui_.fpsKeyLabel->setText(pcie_qt_ui::Zh("PCIe采集"));
        ui_.capturedKeyLabel->setText(pcie_qt_ui::Zh("屏幕显示"));
        ui_.inferenceKeyLabel->setText(pcie_qt_ui::Zh("模型推理"));
        ui_.rootLayout->setStretch(0, 1);
        ui_.rootLayout->setStretch(1, 0);
        ui_.videoLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        ui_.modeComboBox->setCurrentIndex(0);
        ui_.modeBadgeLabel->setText(pcie_qt_ui::ModeBadgeText(pcie_qt_ui::Zh("车牌识别")));
        ConfigureResultTable();
        pcie_qt_ui::ApplyTrafficStyle(this);
        connect(ui_.startButton, &QPushButton::clicked, this, &MainWindow::ToggleCapture);
        connect(ui_.modeComboBox, &QComboBox::currentTextChanged,
                this, &MainWindow::OnModeChanged);
        ui_.startButton->setText(pcie_qt_ui::Zh("开始"));
        ApplyStatus(PcieUiStatus());
    }

    ~MainWindow() override {
        ShutdownWorker();
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        ShutdownWorker();
        event->accept();
    }

private slots:
    void ToggleCapture() {
        if (worker_ == nullptr || !worker_->isRunning()) {
            StartWorker();
            return;
        }

        capture_enabled_ = !capture_enabled_;
        worker_->SetCaptureEnabled(capture_enabled_);
        if (capture_enabled_) {
            last_fps_elapsed_ms_ = 0;
            last_fps_captured_frames_ = 0;
            last_fps_ui_painted_frames_ = ui_painted_frames_;
            last_fps_inference_jobs_ = 0;
        } else {
            pcie_fps_ = 0.0;
            display_fps_ = 0.0;
            inference_fps_ = 0.0;
            ui_.fpsValueLabel->setText(pcie_qt_ui::Zh("0.0 FPS"));
            ui_.capturedValueLabel->setText(pcie_qt_ui::Zh("0.0 FPS"));
            ui_.inferenceValueLabel->setText(pcie_qt_ui::Zh("0.0 FPS"));
        }
        ui_.startButton->setText(capture_enabled_ ? pcie_qt_ui::Zh("暂停")
                                                  : pcie_qt_ui::Zh("继续"));
        ui_.stateValueLabel->setText(capture_enabled_ ? pcie_qt_ui::Zh("运行中")
                                                      : pcie_qt_ui::Zh("已暂停"));
        statusBar()->showMessage(capture_enabled_ ? pcie_qt_ui::Zh("正在采集")
                                                  : pcie_qt_ui::Zh("已暂停"));
    }

    void OnFrameReady(const PcieUiFrame& frame, const PcieUiStatus& status) {
        if (frame.pixels == nullptr || frame.pixels->empty() ||
            frame.width <= 0 || frame.height <= 0 || frame.stride <= 0) {
            if (worker_ != nullptr) {
                worker_->MarkFrameEventConsumed();
            }
            ApplyStatus(status);
            return;
        }
        const QImage image(frame.pixels->data(),
                           frame.width,
                           frame.height,
                           frame.stride * 4,
                           QImage::Format_RGBA8888);
        QSize target_size = ui_.videoLabel->size();
        if (target_size.width() <= 1 || target_size.height() <= 1) {
            target_size = image.size();
        }
        const QPixmap pixmap = QPixmap::fromImage(image).scaled(
            target_size, Qt::KeepAspectRatio, Qt::FastTransformation);
        ui_.videoLabel->setPixmap(pixmap);
        ++ui_painted_frames_;
        if (worker_ != nullptr) {
            worker_->MarkFrameEventConsumed();
        }
    }

    void OnWorkerFinished(const PcieUiStatus& status) {
        ui_.startButton->setText(pcie_qt_ui::Zh("开始"));
        ui_.stateValueLabel->setText(pcie_qt_ui::Zh("空闲"));
        ui_.videoLabel->clear();
        ui_.videoLabel->setText(pcie_qt_ui::Zh("等待图像"));
        const double elapsed_seconds =
            std::max(0.001, ui_run_timer_.isValid() ? ui_run_timer_.elapsed() / 1000.0 : 0.001);
        printf("Qt UI painted: %llu (%.2f fps)\n",
               (unsigned long long)ui_painted_frames_,
               ui_painted_frames_ / elapsed_seconds);
        worker_ = nullptr;
        capture_enabled_ = false;
        if (!status.message.empty()) {
            statusBar()->showMessage(QString::fromUtf8(status.message.c_str()));
        }
    }

    void OnModeChanged(const QString& mode) {
        ui_.modeBadgeLabel->setText(pcie_qt_ui::ModeBadgeText(mode));
        if (mode == pcie_qt_ui::Zh("行人模式")) {
            statusBar()->showMessage(pcie_qt_ui::Zh("行人模式已选择，后续可接入行人模型"));
        } else {
            statusBar()->showMessage(pcie_qt_ui::Zh("车牌识别模式"));
        }
    }

private:
    void ConfigureResultTable() {
        ui_.resultTableWidget->setColumnCount(3);
        ui_.resultTableWidget->setHorizontalHeaderLabels(QStringList()
                                                         << pcie_qt_ui::Zh("车牌")
                                                         << pcie_qt_ui::Zh("类型")
                                                         << pcie_qt_ui::Zh("置信度"));
        ui_.resultTableWidget->setRowCount(0);
        ui_.resultTableWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
        ui_.resultTableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
        ui_.resultTableWidget->setSelectionMode(QAbstractItemView::SingleSelection);
        ui_.resultTableWidget->setAlternatingRowColors(true);
        ui_.resultTableWidget->setShowGrid(false);
        ui_.resultTableWidget->verticalHeader()->setVisible(false);
        ui_.resultTableWidget->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        ui_.resultTableWidget->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        ui_.resultTableWidget->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    }

    void StartWorker() {
        known_plates_.clear();
        pcie_fps_ = 0.0;
        display_fps_ = 0.0;
        inference_fps_ = 0.0;
        ui_painted_frames_ = 0;
        last_fps_elapsed_ms_ = 0;
        last_fps_captured_frames_ = 0;
        last_fps_ui_painted_frames_ = 0;
        last_fps_inference_jobs_ = 0;
        ui_.resultTableWidget->setRowCount(0);
        ui_.videoLabel->clear();
        ui_.videoLabel->setText(pcie_qt_ui::Zh("等待图像"));

        worker_ = new PcieQtWorker(yolov8_model_, lprnet7_model_, lprnet8_model_, this);
        capture_enabled_ = true;
        connect(worker_, &PcieQtWorker::FrameReady, this, &MainWindow::OnFrameReady);
        connect(worker_, &PcieQtWorker::StatusReady, this, &MainWindow::ApplyStatus);
        connect(worker_, &PcieQtWorker::WorkerFinished, this, &MainWindow::OnWorkerFinished);
        connect(worker_, &QThread::finished, worker_, &QObject::deleteLater);
        ui_.startButton->setText(pcie_qt_ui::Zh("暂停"));
        statusBar()->showMessage(pcie_qt_ui::Zh("正在启动"));
        ui_run_timer_.restart();
        worker_->start();
    }

    void ShutdownWorker() {
        if (worker_ == nullptr) {
            return;
        }
        worker_->RequestExit();
        worker_->wait();
        worker_ = nullptr;
        capture_enabled_ = false;
    }

    void ApplyStatus(const PcieUiStatus& status) {
        if (!status.worker_alive || !status.capturing) {
            pcie_fps_ = 0.0;
            display_fps_ = 0.0;
            inference_fps_ = 0.0;
            last_fps_elapsed_ms_ = status.elapsed_ms;
            last_fps_captured_frames_ = status.captured_frames;
            last_fps_ui_painted_frames_ = ui_painted_frames_;
            last_fps_inference_jobs_ = status.inference_jobs;
        } else if (last_fps_elapsed_ms_ == 0) {
            last_fps_elapsed_ms_ = status.elapsed_ms;
            last_fps_captured_frames_ = status.captured_frames;
            last_fps_ui_painted_frames_ = ui_painted_frames_;
            last_fps_inference_jobs_ = status.inference_jobs;
        } else if (status.elapsed_ms > last_fps_elapsed_ms_) {
            const uint64_t delta_ms = status.elapsed_ms - last_fps_elapsed_ms_;
            const uint64_t delta_captured = status.captured_frames >= last_fps_captured_frames_
                                                ? status.captured_frames - last_fps_captured_frames_
                                                : 0;
            const uint64_t delta_displayed = ui_painted_frames_ >= last_fps_ui_painted_frames_
                                                ? ui_painted_frames_ - last_fps_ui_painted_frames_
                                                : 0;
            const uint64_t delta_inference = status.inference_jobs >= last_fps_inference_jobs_
                                                ? status.inference_jobs - last_fps_inference_jobs_
                                                : 0;
            if (delta_ms >= 500U) {
                pcie_fps_ = delta_captured * 1000.0 / delta_ms;
                display_fps_ = delta_displayed * 1000.0 / delta_ms;
                inference_fps_ = delta_inference * 1000.0 / delta_ms;
                last_fps_elapsed_ms_ = status.elapsed_ms;
                last_fps_captured_frames_ = status.captured_frames;
                last_fps_ui_painted_frames_ = ui_painted_frames_;
                last_fps_inference_jobs_ = status.inference_jobs;
            }
        }

        ui_.fpsValueLabel->setText(pcie_qt_ui::Zh("%1 FPS").arg(pcie_fps_, 0, 'f', 1));
        ui_.capturedValueLabel->setText(pcie_qt_ui::Zh("%1 FPS").arg(display_fps_, 0, 'f', 1));
        ui_.inferenceValueLabel->setText(pcie_qt_ui::Zh("%1 FPS").arg(inference_fps_, 0, 'f', 1));
        ui_.latencyValueLabel->setText(pcie_qt_ui::Zh("%1 ms").arg(status.avg_end_to_end_ms, 0, 'f', 1));

        if (!status.plate_text.empty()) {
            const QString plate = QString::fromUtf8(status.plate_text.c_str()).trimmed();
            if (!plate.isEmpty() && !known_plates_.contains(plate)) {
                known_plates_.insert(plate);
                const QString plate_type = status.plate_type.empty()
                                               ? pcie_qt_ui::Zh("--")
                                               : QString::fromUtf8(status.plate_type.c_str());
                const int row = ui_.resultTableWidget->rowCount();
                ui_.resultTableWidget->insertRow(row);

                QTableWidgetItem* plate_item = new QTableWidgetItem(plate);
                QTableWidgetItem* type_item = new QTableWidgetItem(plate_type);
                QTableWidgetItem* confidence_item = new QTableWidgetItem(
                    pcie_qt_ui::Zh("%1%").arg(status.plate_confidence * 100.0f, 0, 'f', 1));
                plate_item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
                type_item->setTextAlignment(Qt::AlignCenter);
                confidence_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

                ui_.resultTableWidget->setItem(row, 0, plate_item);
                ui_.resultTableWidget->setItem(row, 1, type_item);
                ui_.resultTableWidget->setItem(row, 2, confidence_item);
                ui_.resultTableWidget->scrollToBottom();
            }
        }

        ui_.deviceValueLabel->setText(status.vendor_id == 0
                                          ? pcie_qt_ui::Zh("--")
                                          : pcie_qt_ui::Zh("0x%1:0x%2")
                                                .arg(status.vendor_id, 4, 16, QLatin1Char('0'))
                                                .arg(status.device_id, 4, 16, QLatin1Char('0')));
        ui_.linkValueLabel->setText(status.link_speed == 0
                                        ? pcie_qt_ui::Zh("--")
                                        : pcie_qt_ui::Zh("Gen%1 x%2")
                                              .arg(status.link_speed)
                                              .arg(status.link_width));
        ui_.payloadValueLabel->setText(status.max_payload_size == 0
                                           ? pcie_qt_ui::Zh("--")
                                           : QString::number(status.max_payload_size));
        ui_.stateValueLabel->setText(status.worker_alive
                                          ? (status.capturing ? pcie_qt_ui::Zh("运行中")
                                                              : pcie_qt_ui::Zh("已暂停"))
                                          : pcie_qt_ui::Zh("空闲"));

        if (!status.message.empty()) {
            statusBar()->showMessage(QString::fromUtf8(status.message.c_str()));
        }
    }

    Ui::MainWindow ui_;
    QString yolov8_model_;
    QString lprnet7_model_;
    QString lprnet8_model_;
    PcieQtWorker* worker_;
    bool capture_enabled_;
    QSet<QString> known_plates_;
    double pcie_fps_;
    double display_fps_;
    double inference_fps_;
    QElapsedTimer ui_run_timer_;
    uint64_t ui_painted_frames_;
    uint64_t last_fps_elapsed_ms_;
    uint64_t last_fps_captured_frames_;
    uint64_t last_fps_ui_painted_frames_;
    uint64_t last_fps_inference_jobs_;
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    pcie_qt_ui::LoadChineseFont(&app);
    qRegisterMetaType<PcieUiStatus>("PcieUiStatus");
    qRegisterMetaType<PcieUiFrame>("PcieUiFrame");

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

#include "main_pcie_qt.moc"
