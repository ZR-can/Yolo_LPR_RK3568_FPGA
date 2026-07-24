#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdint>

#if defined(__linux__)
#include <signal.h>
#endif

#include <QApplication>
#include <QAbstractItemView>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
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
#include <QTimer>

#include "pcie_demo_bridge.h"
#include "pcie_qt_ui_helpers.h"
#include "traffic_pcie_bridge.h"
#include "ui_mainwindow.h"

Q_DECLARE_METATYPE(PcieUiStatus)
Q_DECLARE_METATYPE(PcieUiFrame)

namespace {

// Keep only a tiny queued backlog so Qt can absorb short paint jitter without
// letting stale frames pile up behind the live PCIe stream.
const int kMaxPendingUiFrames = 2;

enum class UiMode {
    kVideoRecognition = 0,
    kImageRecognition = 1,
    kPedestrianViolation = 2,
};

UiMode UiModeFromIndex(int index) {
    switch (index) {
        case 1:
            return UiMode::kImageRecognition;
        case 2:
            return UiMode::kPedestrianViolation;
        default:
            return UiMode::kVideoRecognition;
    }
}

#if defined(__linux__)

volatile sig_atomic_t g_terminal_stop_signal = 0;

void HandleTerminalStopSignal(int signal_number) {
    g_terminal_stop_signal = signal_number;
}

bool InstallTerminalStopHandlers() {
    struct sigaction action = {};
    action.sa_handler = HandleTerminalStopSignal;
    sigemptyset(&action.sa_mask);

    bool installed = true;
    const int stop_signals[] = {SIGINT, SIGTERM, SIGHUP, SIGTSTP};
    for (const int signal_number : stop_signals) {
        if (sigaction(signal_number, &action, nullptr) != 0) {
            installed = false;
        }
    }
    return installed;
}

#endif

}  // namespace

class PcieQtWorker : public QThread {
    Q_OBJECT

public:
    PcieQtWorker(const QString& yolov8_model,
                 const QString& video_ppocr_model,
                 const QString& image_ppocr_model,
                 const QString& dictionary,
                 const QString& traffic_model,
                 UiMode mode,
                 QObject* parent = nullptr)
        : QThread(parent),
          yolov8_model_(yolov8_model),
          video_ppocr_model_(video_ppocr_model),
          image_ppocr_model_(image_ppocr_model),
          dictionary_(dictionary),
          traffic_model_(traffic_model),
          mode_(mode),
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

        int ret = -1;
        if (mode_ == UiMode::kVideoRecognition) {
            ret = RunPpocrPcieDemo(yolov8_model_.toLocal8Bit().constData(),
                                   video_ppocr_model_.toLocal8Bit().constData(),
                                   dictionary_.toLocal8Bit().constData(),
                                   &callbacks);
        } else if (mode_ == UiMode::kImageRecognition) {
            ret = RunPpocrPcieImageDemo(
                yolov8_model_.toLocal8Bit().constData(),
                image_ppocr_model_.toLocal8Bit().constData(),
                dictionary_.toLocal8Bit().constData(),
                &callbacks);
        } else if (mode_ == UiMode::kPedestrianViolation) {
            ret = RunTrafficPcieQtDemo(
                traffic_model_.toLocal8Bit().constData(), &callbacks);
        }

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
    QString video_ppocr_model_;
    QString image_ppocr_model_;
    QString dictionary_;
    QString traffic_model_;
    UiMode mode_;
    std::atomic<bool> stop_requested_;
    std::atomic<bool> capture_enabled_;
    std::atomic<int> pending_frame_events_;
    std::atomic<uint64_t> last_status_emit_ms_;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(const QString& yolov8_model,
               const QString& video_ppocr_model,
               const QString& image_ppocr_model,
               const QString& dictionary,
               const QString& traffic_model,
               QWidget* parent = nullptr)
        : QMainWindow(parent),
          yolov8_model_(yolov8_model),
          video_ppocr_model_(video_ppocr_model),
          image_ppocr_model_(image_ppocr_model),
          dictionary_(dictionary),
          traffic_model_(traffic_model),
          worker_(nullptr),
          capture_enabled_(false),
          image_result_initialized_(false),
          image_result_inference_jobs_(0),
          pcie_fps_(0.0),
          display_fps_(0.0),
          inference_fps_(0.0),
          latest_frame_width_(0),
          latest_frame_height_(0),
          latest_frame_stride_(0),
          latest_frame_id_(-1),
          ui_painted_frames_(0),
          ui_statistics_printed_(false),
          worker_generation_(0),
          last_fps_elapsed_ms_(0),
          last_fps_captured_frames_(0),
          last_fps_ui_painted_frames_(0),
          last_fps_inference_jobs_(0) {
        ui_.setupUi(this);
        setWindowTitle(pcie_qt_ui::Zh("智能交通视觉分析系统"));
        ui_.titleLabel->setText(pcie_qt_ui::Zh("视频车牌识别"));
        ui_.fpsKeyLabel->setText(pcie_qt_ui::Zh("PCIe采集"));
        ui_.capturedKeyLabel->setText(pcie_qt_ui::Zh("屏幕显示"));
        ui_.inferenceKeyLabel->setText(pcie_qt_ui::Zh("模型推理"));
        ui_.rootLayout->setStretch(0, 1);
        ui_.rootLayout->setStretch(1, 0);
        ui_.videoLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        ui_.modeComboBox->setCurrentIndex(0);
        ui_.modeBadgeLabel->setText(
            pcie_qt_ui::ModeBadgeText(pcie_qt_ui::Zh("视频识别")));
        ui_.resultGroup->setTitle(pcie_qt_ui::Zh("车牌识别结果"));
        ConfigureResultTable(UiMode::kVideoRecognition);
        pcie_qt_ui::ApplyTrafficStyle(this);
        connect(ui_.startButton, &QPushButton::clicked, this, &MainWindow::ToggleCapture);
        connect(ui_.saveButton, &QPushButton::clicked, this, &MainWindow::SaveCurrentImage);
        connect(ui_.modeComboBox, &QComboBox::currentTextChanged,
                this, &MainWindow::OnModeChanged);
        ui_.startButton->setText(pcie_qt_ui::Zh("开始"));
        ui_.saveButton->setText(pcie_qt_ui::Zh("保存图片"));
        ui_.saveButton->setEnabled(false);
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
        ui_.modeComboBox->setEnabled(!capture_enabled_);
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
        latest_frame_pixels_ = frame.pixels;
        latest_frame_width_ = frame.width;
        latest_frame_height_ = frame.height;
        latest_frame_stride_ = frame.stride;
        latest_frame_id_ = frame.frame_id;
        ui_.saveButton->setEnabled(true);
        ++ui_painted_frames_;
        if (worker_ != nullptr) {
            worker_->MarkFrameEventConsumed();
        }
    }

    void OnWorkerFinished(const PcieUiStatus& status) {
        ui_.startButton->setText(pcie_qt_ui::Zh("开始"));
        ui_.startButton->setEnabled(true);
        ui_.modeComboBox->setEnabled(true);
        ui_.stateValueLabel->setText(pcie_qt_ui::Zh("空闲"));
        ui_.videoLabel->clear();
        ui_.videoLabel->setText(pcie_qt_ui::Zh("等待图像"));
        ui_.saveButton->setEnabled(latest_frame_pixels_ != nullptr &&
                                   !latest_frame_pixels_->empty());
        PrintUiStatistics();
        worker_ = nullptr;
        capture_enabled_ = false;
        if (!status.message.empty()) {
            statusBar()->showMessage(QString::fromUtf8(status.message.c_str()));
        }
    }

    void OnModeChanged(const QString& mode) {
        if (worker_ != nullptr && worker_->isRunning()) {
            if (capture_enabled_) {
                return;
            }
            ShutdownWorker();
            ui_.startButton->setText(pcie_qt_ui::Zh("开始"));
            ui_.stateValueLabel->setText(pcie_qt_ui::Zh("空闲"));
        }

        ui_.modeBadgeLabel->setText(pcie_qt_ui::ModeBadgeText(mode));
        const UiMode selected_mode = CurrentMode();
        ConfigureResultTable(selected_mode);
        ResetPreview();
        if (selected_mode == UiMode::kVideoRecognition) {
            ui_.titleLabel->setText(pcie_qt_ui::Zh("视频车牌识别"));
            ui_.resultGroup->setTitle(pcie_qt_ui::Zh("车牌识别结果"));
            ui_.startButton->setEnabled(true);
            statusBar()->showMessage(
                pcie_qt_ui::Zh("视频识别：YOLOv8 + PP-OCR"));
        } else if (selected_mode == UiMode::kImageRecognition) {
            ui_.titleLabel->setText(pcie_qt_ui::Zh("图片车牌识别"));
            ui_.resultGroup->setTitle(pcie_qt_ui::Zh("当前图片识别结果"));
            ui_.videoLabel->setText(pcie_qt_ui::Zh("等待PCIe静态图片"));
            ui_.startButton->setEnabled(true);
            statusBar()->showMessage(
                pcie_qt_ui::Zh("图片识别：FP16 PP-OCR，单次推理结果"));
        } else {
            ui_.titleLabel->setText(pcie_qt_ui::Zh("行人违法检测"));
            ui_.resultGroup->setTitle(pcie_qt_ui::Zh("违法检测结果"));
            ui_.startButton->setEnabled(true);
            statusBar()->showMessage(
                pcie_qt_ui::Zh("行人违法检测：YOLOv8 Traffic"));
        }
    }

    void SaveCurrentImage() {
        if (latest_frame_pixels_ == nullptr || latest_frame_pixels_->empty() ||
            latest_frame_width_ <= 0 || latest_frame_height_ <= 0 ||
            latest_frame_stride_ <= 0) {
            statusBar()->showMessage(pcie_qt_ui::Zh("暂无可保存图片"));
            return;
        }

        QDir save_dir(QApplication::applicationDirPath() + "/saved_images");
        if (!save_dir.exists() && !save_dir.mkpath(".")) {
            statusBar()->showMessage(pcie_qt_ui::Zh("保存目录创建失败"));
            return;
        }

        const QString timestamp =
            QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz");
        const QString basename = pcie_qt_ui::Zh("pcie_overlay_%1_frame%2")
                                     .arg(timestamp)
                                     .arg(latest_frame_id_);
        QString output_path = save_dir.filePath(basename + ".png");
        const QImage image(latest_frame_pixels_->data(),
                           latest_frame_width_,
                           latest_frame_height_,
                           latest_frame_stride_ * 4,
                           QImage::Format_RGBA8888);
        bool saved = image.save(output_path, "PNG");
        if (!saved) {
            output_path = save_dir.filePath(basename + ".bmp");
            saved = image.save(output_path, "BMP");
        }
        if (!saved) {
            statusBar()->showMessage(pcie_qt_ui::Zh("图片保存失败"));
            return;
        }

        statusBar()->showMessage(pcie_qt_ui::Zh("图片已保存：%1").arg(output_path));
        printf("Qt saved image: %s\n", output_path.toUtf8().constData());
    }

private:
    UiMode CurrentMode() const {
        return UiModeFromIndex(ui_.modeComboBox->currentIndex());
    }

    void ConfigureResultTable(UiMode mode) {
        ui_.resultTableWidget->clear();
        if (mode == UiMode::kVideoRecognition ||
            mode == UiMode::kImageRecognition) {
            ui_.resultTableWidget->setColumnCount(3);
            ui_.resultTableWidget->setHorizontalHeaderLabels(
                QStringList() << pcie_qt_ui::Zh("车牌")
                              << pcie_qt_ui::Zh("类型")
                              << pcie_qt_ui::Zh("置信度"));
            ui_.resultTableWidget->setRowCount(0);
        } else {
            ui_.resultTableWidget->setColumnCount(3);
            ui_.resultTableWidget->setHorizontalHeaderLabels(
                QStringList() << pcie_qt_ui::Zh("项目")
                              << pcie_qt_ui::Zh("当前")
                              << pcie_qt_ui::Zh("累计"));
            const QStringList metrics =
                QStringList() << pcie_qt_ui::Zh("信号灯")
                              << pcie_qt_ui::Zh("行人")
                              << pcie_qt_ui::Zh("斑马线内")
                              << pcie_qt_ui::Zh("违法");
            ui_.resultTableWidget->setRowCount(metrics.size());
            for (int row = 0; row < metrics.size(); ++row) {
                ui_.resultTableWidget->setItem(
                    row, 0, new QTableWidgetItem(metrics[row]));
                ui_.resultTableWidget->setItem(
                    row, 1, new QTableWidgetItem(pcie_qt_ui::Zh("--")));
                ui_.resultTableWidget->setItem(
                    row, 2, new QTableWidgetItem(pcie_qt_ui::Zh("--")));
            }
        }
        ui_.resultTableWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
        ui_.resultTableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
        ui_.resultTableWidget->setSelectionMode(QAbstractItemView::SingleSelection);
        ui_.resultTableWidget->setAlternatingRowColors(true);
        ui_.resultTableWidget->setShowGrid(false);
        ui_.resultTableWidget->verticalHeader()->setVisible(false);
        ui_.resultTableWidget->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        for (int column = 1;
             column < ui_.resultTableWidget->columnCount();
             ++column) {
            ui_.resultTableWidget->horizontalHeader()->setSectionResizeMode(
                column, QHeaderView::ResizeToContents);
        }
    }

    void ResetPreview() {
        known_plates_.clear();
        image_result_initialized_ = false;
        image_result_inference_jobs_ = 0;
        latest_frame_pixels_.reset();
        latest_frame_width_ = 0;
        latest_frame_height_ = 0;
        latest_frame_stride_ = 0;
        latest_frame_id_ = -1;
        ui_.videoLabel->clear();
        ui_.videoLabel->setText(pcie_qt_ui::Zh("等待图像"));
        ui_.saveButton->setEnabled(false);
    }

    void StartWorker() {
        const UiMode mode = CurrentMode();
        if (mode == UiMode::kImageRecognition &&
            !QFileInfo::exists(image_ppocr_model_)) {
            statusBar()->showMessage(
                pcie_qt_ui::Zh("图片识别模型不存在：%1").arg(image_ppocr_model_));
            return;
        }
        if (mode == UiMode::kPedestrianViolation &&
            !QFileInfo::exists(traffic_model_)) {
            statusBar()->showMessage(
                pcie_qt_ui::Zh("交通检测模型不存在：%1").arg(traffic_model_));
            return;
        }

        pcie_fps_ = 0.0;
        display_fps_ = 0.0;
        inference_fps_ = 0.0;
        ui_painted_frames_ = 0;
        ui_statistics_printed_ = false;
        last_fps_elapsed_ms_ = 0;
        last_fps_captured_frames_ = 0;
        last_fps_ui_painted_frames_ = 0;
        last_fps_inference_jobs_ = 0;
        ConfigureResultTable(mode);
        ResetPreview();

        worker_ = new PcieQtWorker(yolov8_model_,
                                   video_ppocr_model_,
                                   image_ppocr_model_,
                                   dictionary_,
                                   traffic_model_,
                                   mode,
                                   this);
        const uint64_t run_generation = ++worker_generation_;
        PcieQtWorker* const started_worker = worker_;
        capture_enabled_ = true;
        connect(worker_,
                &PcieQtWorker::FrameReady,
                this,
                [this, run_generation, started_worker](
                    const PcieUiFrame& frame, const PcieUiStatus& status) {
                    if (run_generation == worker_generation_ &&
                        worker_ == started_worker) {
                        OnFrameReady(frame, status);
                    }
                });
        connect(worker_,
                &PcieQtWorker::StatusReady,
                this,
                [this, run_generation, started_worker](
                    const PcieUiStatus& status) {
                    if (run_generation == worker_generation_ &&
                        worker_ == started_worker) {
                        ApplyStatus(status);
                    }
                });
        connect(worker_,
                &PcieQtWorker::WorkerFinished,
                this,
                [this, run_generation, started_worker](
                    const PcieUiStatus& status) {
                    if (run_generation == worker_generation_ &&
                        worker_ == started_worker) {
                        OnWorkerFinished(status);
                    }
                });
        connect(worker_, &QThread::finished, worker_, &QObject::deleteLater);
        ui_.startButton->setText(pcie_qt_ui::Zh("暂停"));
        ui_.modeComboBox->setEnabled(false);
        statusBar()->showMessage(pcie_qt_ui::Zh("正在启动"));
        ui_run_timer_.restart();
        worker_->start();
    }

    void ShutdownWorker() {
        if (worker_ == nullptr) {
            return;
        }
        PcieQtWorker* const stopping_worker = worker_;
        ++worker_generation_;
        worker_ = nullptr;
        stopping_worker->RequestExit();
        stopping_worker->wait();
        PrintUiStatistics();
        capture_enabled_ = false;
    }

    void PrintUiStatistics() {
        if (ui_statistics_printed_ || !ui_run_timer_.isValid()) {
            return;
        }
        const double elapsed_seconds =
            std::max(0.001, ui_run_timer_.elapsed() / 1000.0);
        printf("Qt UI painted: %llu (%.2f fps)\n",
               (unsigned long long)ui_painted_frames_,
               ui_painted_frames_ / elapsed_seconds);
        fflush(stdout);
        ui_statistics_printed_ = true;
    }

    void SetResultCellText(int row, int column, const QString& text) {
        QTableWidgetItem* item = ui_.resultTableWidget->item(row, column);
        if (item == nullptr) {
            item = new QTableWidgetItem();
            ui_.resultTableWidget->setItem(row, column, item);
        }
        item->setText(text);
        item->setTextAlignment(column == 0
                                   ? Qt::AlignLeft | Qt::AlignVCenter
                                   : Qt::AlignRight | Qt::AlignVCenter);
    }

    QString TrafficLightText(const std::string& state) const {
        if (state == "red") {
            return pcie_qt_ui::Zh("红灯");
        }
        if (state == "green") {
            return pcie_qt_ui::Zh("绿灯");
        }
        return pcie_qt_ui::Zh("未知");
    }

    void ApplyTrafficResult(const PcieUiStatus& status) {
        SetResultCellText(
            0, 1, TrafficLightText(status.traffic_light_state));
        SetResultCellText(0, 2, pcie_qt_ui::Zh("--"));
        SetResultCellText(
            1, 1, QString::number(status.traffic_person_count));
        SetResultCellText(
            1, 2, QString::number(status.traffic_person_id_total));
        SetResultCellText(
            2, 1, QString::number(status.traffic_persons_in_crosswalk));
        SetResultCellText(2, 2, pcie_qt_ui::Zh("--"));
        SetResultCellText(
            3, 1, QString::number(status.traffic_violation_count));
        SetResultCellText(
            3, 2, QString::number(status.traffic_violation_event_total));
    }

    void InsertPlateResultRow(const PcieUiStatus& status) {
        const QString plate =
            QString::fromUtf8(status.plate_text.c_str()).trimmed();
        if (plate.isEmpty()) {
            return;
        }
        const QString plate_type = status.plate_type.empty()
                                       ? pcie_qt_ui::Zh("--")
                                       : QString::fromUtf8(
                                             status.plate_type.c_str());
        const int row = ui_.resultTableWidget->rowCount();
        ui_.resultTableWidget->insertRow(row);

        QTableWidgetItem* plate_item = new QTableWidgetItem(plate);
        QTableWidgetItem* type_item = new QTableWidgetItem(plate_type);
        QTableWidgetItem* confidence_item = new QTableWidgetItem(
            pcie_qt_ui::Zh("%1%").arg(
                status.plate_confidence * 100.0f, 0, 'f', 1));
        plate_item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        type_item->setTextAlignment(Qt::AlignCenter);
        confidence_item->setTextAlignment(
            Qt::AlignRight | Qt::AlignVCenter);

        ui_.resultTableWidget->setItem(row, 0, plate_item);
        ui_.resultTableWidget->setItem(row, 1, type_item);
        ui_.resultTableWidget->setItem(row, 2, confidence_item);
    }

    void ApplyImagePlateResult(const PcieUiStatus& status) {
        if (image_result_initialized_ &&
            status.inference_jobs == image_result_inference_jobs_) {
            return;
        }
        image_result_initialized_ = true;
        image_result_inference_jobs_ = status.inference_jobs;
        ui_.resultTableWidget->setRowCount(0);
        InsertPlateResultRow(status);
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

        if (CurrentMode() == UiMode::kVideoRecognition &&
            !status.plate_text.empty()) {
            const QString plate = QString::fromUtf8(status.plate_text.c_str()).trimmed();
            if (!plate.isEmpty() && !known_plates_.contains(plate)) {
                known_plates_.insert(plate);
                InsertPlateResultRow(status);
                ui_.resultTableWidget->scrollToBottom();
            }
        } else if (CurrentMode() == UiMode::kImageRecognition) {
            ApplyImagePlateResult(status);
        } else if (CurrentMode() == UiMode::kPedestrianViolation) {
            ApplyTrafficResult(status);
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
    QString video_ppocr_model_;
    QString image_ppocr_model_;
    QString dictionary_;
    QString traffic_model_;
    PcieQtWorker* worker_;
    bool capture_enabled_;
    QSet<QString> known_plates_;
    bool image_result_initialized_;
    uint64_t image_result_inference_jobs_;
    double pcie_fps_;
    double display_fps_;
    double inference_fps_;
    std::shared_ptr<std::vector<unsigned char> > latest_frame_pixels_;
    int latest_frame_width_;
    int latest_frame_height_;
    int latest_frame_stride_;
    int latest_frame_id_;
    QElapsedTimer ui_run_timer_;
    uint64_t ui_painted_frames_;
    bool ui_statistics_printed_;
    uint64_t worker_generation_;
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

    if (argc < 4 || argc > 6) {
        fprintf(stderr,
                "用法: %s <车牌yolov8模型> <视频ppocr模型> <字符字典> "
                "[交通yolov8模型] [图片ppocr模型]\n",
                argv[0]);
        return 1;
    }

    const QString traffic_model =
        argc >= 5
            ? QString::fromLocal8Bit(argv[4])
            : QDir(QApplication::applicationDirPath())
                  .filePath("model/traffic/yolov8_traffic_i8.rknn");
    const QString image_ppocr_model =
        argc >= 6
            ? QString::fromLocal8Bit(argv[5])
            : QDir(QApplication::applicationDirPath())
                  .filePath(
                      "model/ppocrv4_rec14_fold_affine_1x1_rk3568_fp16.rknn");
    MainWindow window(QString::fromLocal8Bit(argv[1]),
                      QString::fromLocal8Bit(argv[2]),
                      image_ppocr_model,
                      QString::fromLocal8Bit(argv[3]),
                      traffic_model);

#if defined(__linux__)
    if (!InstallTerminalStopHandlers()) {
        fprintf(stderr, "Warning: failed to install one or more terminal signal handlers\n");
    }
    QTimer terminal_signal_timer;
    QObject::connect(&terminal_signal_timer, &QTimer::timeout, [&window]() {
        const int signal_number = g_terminal_stop_signal;
        if (signal_number == 0) {
            return;
        }
        g_terminal_stop_signal = 0;
        fprintf(stderr,
                "\nReceived signal %d; closing Qt UI and printing statistics...\n",
                signal_number);
        window.close();
    });
    terminal_signal_timer.start(100);
#endif

    window.show();
    return app.exec();
}

#include "main_pcie_qt.moc"
