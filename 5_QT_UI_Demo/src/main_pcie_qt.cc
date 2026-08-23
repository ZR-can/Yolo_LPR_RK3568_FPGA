#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

#if defined(__linux__)
#include <signal.h>
#endif

#include <QApplication>
#include <QAbstractItemView>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGridLayout>
#include <QHeaderView>
#include <QImage>
#include <QMainWindow>
#include <QMetaType>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QSize>
#include <QSizePolicy>
#include <QSet>
#include <QStringList>
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
const double kRuntimeFpsDisplayCorrection = 3.0;

enum class UiMode {
    kVideoRecognition = 0,
    kImageRecognition = 1,
    kPedestrianViolation = 2,
};

void PrintUsage(const char* program) {
    std::fprintf(
        stderr,
        "Usage: %s [--roi-config PATH] [--roi \"x1,y1;x2,y2;...\"] "
        "[--light-roi \"left,top,right,bottom\"]\n"
        "  Default config: <application-directory>/model/traffic/traffic_roi.conf\n"
        "  Explicit --roi and --light-roi values override the config individually.\n"
        "  All ROI coordinates are normalized to [0,1].\n",
        program);
}

bool ParseCommandLine(int argc,
                      char** argv,
                      const std::string& default_roi_config_path,
                      TrafficRoiConfig* traffic_roi,
                      TrafficLightRoiConfig* light_roi,
                      bool* show_help) {
    if (traffic_roi == nullptr || light_roi == nullptr || show_help == nullptr) {
        return false;
    }
    *traffic_roi = default_traffic_roi();
    *light_roi = default_traffic_light_roi();
    *show_help = false;
    std::string roi_config_path = default_roi_config_path;
    bool roi_config_explicit = false;
    bool roi_seen = false;
    bool light_roi_seen = false;
    std::string roi_text;
    std::string light_roi_text;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 ||
            std::strcmp(argv[i], "-h") == 0) {
            if (argc == 2) {
                *show_help = true;
            } else {
                std::fprintf(stderr, "--help cannot be combined with other options\n");
            }
            return false;
        }
        if (std::strcmp(argv[i], "--roi-config") == 0) {
            if (roi_config_explicit) {
                std::fprintf(stderr, "--roi-config may only be specified once\n");
                return false;
            }
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                std::fprintf(stderr, "--roi-config requires a file path\n");
                return false;
            }
            roi_config_path = argv[++i];
            roi_config_explicit = true;
        } else if (std::strcmp(argv[i], "--roi") == 0) {
            if (roi_seen) {
                std::fprintf(stderr, "--roi may only be specified once\n");
                return false;
            }
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--roi requires a polygon string\n");
                return false;
            }
            roi_text = argv[++i];
            roi_seen = true;
        } else if (std::strcmp(argv[i], "--light-roi") == 0) {
            if (light_roi_seen) {
                std::fprintf(stderr, "--light-roi may only be specified once\n");
                return false;
            }
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--light-roi requires left,top,right,bottom\n");
                return false;
            }
            light_roi_text = argv[++i];
            light_roi_seen = true;
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return false;
        }
    }

    const QFileInfo roi_config_file(
        QString::fromLocal8Bit(roi_config_path.c_str()));
    std::string error;
    if (roi_config_file.isFile() && roi_config_file.isReadable()) {
        if (!load_traffic_roi_config_file(
                roi_config_path.c_str(), traffic_roi, light_roi, &error)) {
            std::fprintf(stderr, "Invalid ROI config: %s\n", error.c_str());
            return false;
        }
        std::printf("[Traffic ROI] loaded config: %s\n", roi_config_path.c_str());
    } else if (roi_config_explicit) {
        std::fprintf(stderr,
                     "Explicit ROI config is not readable: %s\n",
                     roi_config_path.c_str());
        return false;
    } else {
        std::fprintf(
            stderr,
            "Warning: default ROI config is not readable; using built-in values: %s\n",
            roi_config_path.c_str());
    }

    if (roi_seen &&
        !parse_normalized_traffic_roi(
            roi_text.c_str(), traffic_roi, &error)) {
        std::fprintf(stderr, "Invalid --roi: %s\n", error.c_str());
        return false;
    }
    if (light_roi_seen &&
        !parse_normalized_traffic_light_roi(
            light_roi_text.c_str(), light_roi, &error)) {
        std::fprintf(stderr, "Invalid --light-roi: %s\n", error.c_str());
        return false;
    }
    return true;
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
                 const QString& yolov8_obb_model,
                 const QString& video_ppocr_model,
                 const QString& image_ppocr_model,
                 const QString& dictionary,
                 const QString& traffic_model,
                 const TrafficRoiConfig& traffic_roi,
                 const TrafficLightRoiConfig& light_roi,
                 UiMode mode,
                 QObject* parent = nullptr)
        : QThread(parent),
          yolov8_model_(yolov8_model),
          yolov8_obb_model_(yolov8_obb_model),
          video_ppocr_model_(video_ppocr_model),
          image_ppocr_model_(image_ppocr_model),
          dictionary_(dictionary),
          traffic_model_(traffic_model),
          traffic_roi_(traffic_roi),
          light_roi_(light_roi),
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
                yolov8_obb_model_.toLocal8Bit().constData(),
                image_ppocr_model_.toLocal8Bit().constData(),
                dictionary_.toLocal8Bit().constData(),
                &callbacks);
        } else if (mode_ == UiMode::kPedestrianViolation) {
            ret = RunTrafficPcieQtDemo(
                traffic_model_.toLocal8Bit().constData(),
                traffic_roi_,
                light_roi_,
                &callbacks);
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
    QString yolov8_obb_model_;
    QString video_ppocr_model_;
    QString image_ppocr_model_;
    QString dictionary_;
    QString traffic_model_;
    TrafficRoiConfig traffic_roi_;
    TrafficLightRoiConfig light_roi_;
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
               const QString& yolov8_obb_model,
               const QString& video_ppocr_model,
               const QString& image_ppocr_model,
               const QString& dictionary,
               const QString& traffic_model,
               const TrafficRoiConfig& traffic_roi,
               const TrafficLightRoiConfig& light_roi,
               QWidget* parent = nullptr)
        : QMainWindow(parent),
          yolov8_model_(yolov8_model),
          yolov8_obb_model_(yolov8_obb_model),
          video_ppocr_model_(video_ppocr_model),
          image_ppocr_model_(image_ppocr_model),
          dictionary_(dictionary),
          traffic_model_(traffic_model),
          traffic_roi_(traffic_roi),
          light_roi_(light_roi),
          worker_(nullptr),
          capture_enabled_(false),
          operation_message_hold_until_ms_(0),
          image_result_initialized_(false),
          image_result_generation_(0),
          pcie_fps_(0.0),
          display_fps_(0.0),
          inference_fps_(0.0),
          latest_frame_width_(0),
          latest_frame_height_(0),
          latest_frame_stride_(0),
          latest_frame_id_(-1),
          ui_painted_frames_(0),
          ui_frame_handoff_ms_(0.0),
          ui_statistics_printed_(false),
          worker_generation_(0),
          last_fps_elapsed_ms_(0),
          last_fps_captured_frames_(0),
          last_fps_ui_painted_frames_(0),
          last_fps_inference_jobs_(0) {
        ui_.setupUi(this);
        ui_.sideLayout->setStretch(2, 1);
        ui_.videoBorderFrame->setAttribute(Qt::WA_TransparentForMouseEvents);
        ui_.videoBorderFrame->raise();
        ui_.videoLeftEdgeFrame->setAttribute(Qt::WA_TransparentForMouseEvents);
        ui_.videoLeftEdgeFrame->raise();
        setWindowTitle(pcie_qt_ui::Zh("智能交通视觉分析系统"));
        ui_.titleLabel->setText(pcie_qt_ui::Zh("视频车牌识别"));
        ui_.fpsKeyLabel->setText(pcie_qt_ui::Zh("PCIe采集"));
        ui_.capturedKeyLabel->setText(pcie_qt_ui::Zh("屏幕显示"));
        ui_.inferenceKeyLabel->setText(pcie_qt_ui::Zh("模型推理"));
        ui_.videoLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        ui_.videoModeButton->setChecked(true);
        ConfigureRuntimeStatus();
        ui_.resultGroup->setTitle(pcie_qt_ui::Zh("车牌识别结果"));
        ConfigureResultTable(UiMode::kVideoRecognition);
        pcie_qt_ui::ApplyTrafficStyle(this);
        connect(ui_.startButton, &QPushButton::clicked, this, &MainWindow::ToggleCapture);
        connect(ui_.saveButton, &QPushButton::clicked, this, &MainWindow::SaveCurrentImage);
        connect(ui_.videoModeButton, &QPushButton::clicked, this, [this]() {
            SelectMode(UiMode::kVideoRecognition);
        });
        connect(ui_.imageModeButton, &QPushButton::clicked, this, [this]() {
            SelectMode(UiMode::kImageRecognition);
        });
        connect(ui_.trafficModeButton, &QPushButton::clicked, this, [this]() {
            SelectMode(UiMode::kPedestrianViolation);
        });
        ui_.startButton->setText(pcie_qt_ui::Zh("开始显示"));
        ui_.saveButton->setText(pcie_qt_ui::Zh("保存图片"));
        ui_.saveButton->setEnabled(false);
        ResetFpgaPreprocessing();
        ShowOperationMessage(pcie_qt_ui::Zh("等待PCIe帧数据"));
        ApplyStatus(PcieUiStatus());
    }

    ~MainWindow() override {
        ShutdownWorker();
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        ShutdownWorker();
        ResetFpgaPreprocessing();
        event->accept();
    }

private slots:
    void ToggleCapture() {
        if (worker_ == nullptr || !worker_->isRunning()) {
            StartWorker();
            return;
        }

        capture_enabled_ = !capture_enabled_;
        operation_message_hold_until_ms_ = 0;
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
        ui_.startButton->setText(capture_enabled_ ? pcie_qt_ui::Zh("暂停显示")
                                                   : pcie_qt_ui::Zh("继续显示"));
        SetModeSelectionEnabled(!capture_enabled_);
        ui_.stateValueLabel->setText(capture_enabled_ ? pcie_qt_ui::Zh("运行中")
                                                       : pcie_qt_ui::Zh("已暂停"));
        ShowOperationMessage(capture_enabled_ ? pcie_qt_ui::Zh("正在采集")
                                               : pcie_qt_ui::Zh("等待PCIe帧数据"));
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
        const bool first_valid_frame = latest_frame_id_ < 0;
        QElapsedTimer handoff_timer;
        handoff_timer.start();
        const QSize target_size = ui_.videoLabel->size();
        ui_.videoLabel->setPixmap(QPixmap::fromImage(image));
        ui_frame_handoff_ms_ += handoff_timer.nsecsElapsed() / 1000000.0;
        if (target_size != last_logged_preview_viewport_size_) {
            std::printf(
                "Qt preview: source=%dx%d, viewport=%dx%d, output=%dx%d, scaling=%s\n",
                image.width(),
                image.height(),
                target_size.width(),
                target_size.height(),
                image.width(),
                image.height(),
                "disabled");
            std::fflush(stdout);
            last_logged_preview_viewport_size_ = target_size;
        }
        latest_frame_pixels_ = frame.pixels;
        latest_frame_width_ = frame.width;
        latest_frame_height_ = frame.height;
        latest_frame_stride_ = frame.stride;
        latest_frame_id_ = frame.frame_id;
        if (first_valid_frame) {
            ShowOperationMessage(pcie_qt_ui::Zh("正在采集"));
        }
        ui_.saveButton->setEnabled(true);
        ++ui_painted_frames_;
        if (CurrentMode() == UiMode::kImageRecognition) {
            ApplyImagePlateResult(status);
        }
        if (worker_ != nullptr) {
            worker_->MarkFrameEventConsumed();
        }
    }

    void OnWorkerFinished(const PcieUiStatus& status) {
        ui_.startButton->setText(pcie_qt_ui::Zh("开始显示"));
        ui_.startButton->setEnabled(true);
        SetModeSelectionEnabled(true);
        ui_.stateValueLabel->setText(pcie_qt_ui::Zh("空闲"));
        ui_.videoLabel->clear();
        ui_.videoLabel->setText(pcie_qt_ui::Zh("等待图像"));
        ui_.saveButton->setEnabled(latest_frame_pixels_ != nullptr &&
                                   !latest_frame_pixels_->empty());
        PrintUiStatistics();
        worker_ = nullptr;
        capture_enabled_ = false;
        operation_message_hold_until_ms_ = 0;
        if (!status.message.empty()) {
            ShowOperationMessage(QString::fromUtf8(status.message.c_str()));
        } else {
            ShowOperationMessage(pcie_qt_ui::Zh("等待PCIe帧数据"));
        }
    }

    void SelectMode(UiMode selected_mode) {
        if (worker_ != nullptr && worker_->isRunning()) {
            if (capture_enabled_) {
                return;
            }
            ShutdownWorker();
            ui_.startButton->setText(pcie_qt_ui::Zh("开始显示"));
            ui_.stateValueLabel->setText(pcie_qt_ui::Zh("空闲"));
        }

        ConfigureRuntimeStatus();
        ConfigureResultTable(selected_mode);
        ResetPreview();
        if (selected_mode == UiMode::kVideoRecognition) {
            ui_.titleLabel->setText(pcie_qt_ui::Zh("视频车牌识别"));
            ui_.resultGroup->setTitle(pcie_qt_ui::Zh("车牌识别结果"));
            ui_.startButton->setEnabled(true);
        } else if (selected_mode == UiMode::kImageRecognition) {
            ui_.titleLabel->setText(pcie_qt_ui::Zh("图片车牌识别"));
            ui_.resultGroup->setTitle(pcie_qt_ui::Zh("当前图片识别结果"));
            ui_.videoLabel->setText(pcie_qt_ui::Zh("等待PCIe静态图片"));
            ui_.startButton->setEnabled(true);
        } else {
            ui_.titleLabel->setText(pcie_qt_ui::Zh("行人违法检测"));
            ui_.resultGroup->setTitle(pcie_qt_ui::Zh("违法检测结果"));
            ui_.startButton->setEnabled(true);
        }
        operation_message_hold_until_ms_ = 0;
        ShowOperationMessage(pcie_qt_ui::Zh("等待PCIe帧数据"));
    }

    void SaveCurrentImage() {
        if (latest_frame_pixels_ == nullptr || latest_frame_pixels_->empty() ||
            latest_frame_width_ <= 0 || latest_frame_height_ <= 0 ||
            latest_frame_stride_ <= 0) {
            ShowTransientOperationMessage(
                pcie_qt_ui::Zh("暂无可保存图片"));
            return;
        }

        QDir save_dir(QApplication::applicationDirPath() + "/saved_images");
        if (!save_dir.exists() && !save_dir.mkpath(".")) {
            ShowTransientOperationMessage(
                pcie_qt_ui::Zh("保存目录创建失败"));
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
            ShowTransientOperationMessage(
                pcie_qt_ui::Zh("图片保存失败"));
            return;
        }

        ShowTransientOperationMessage(pcie_qt_ui::Zh("已保存"));
        printf("Qt saved image: %s\n", output_path.toUtf8().constData());
    }

private:
    void ResetFpgaPreprocessing() {
        const QStringList arguments = QStringList()
            << "apply" << "bypass" << "128" << "0" << "0" << "0" << "0";
        QString error;
        if (!RunFpgaControlCommand(arguments, &error)) {
            std::fprintf(stderr, "FPGA default reset failed: %s\n",
                         error.toUtf8().constData());
        }
        last_logged_preview_viewport_size_ = QSize();
    }

    QString FpgaControlScriptPath() const {
        return QApplication::applicationDirPath() + "/fpga_preproc_ctrl.sh";
    }

    bool RunFpgaControlCommand(const QStringList& arguments, QString* error) {
        const QString script_path = FpgaControlScriptPath();
        if (!QFileInfo(script_path).isFile()) {
            if (error != nullptr) {
                *error = pcie_qt_ui::Zh("找不到控制脚本");
            }
            return false;
        }

        QProcess process;
        QStringList shell_arguments;
        shell_arguments << script_path;
        shell_arguments.append(arguments);
        std::printf("FPGA control command: /bin/sh %s %s\n",
                    script_path.toUtf8().constData(),
                    arguments.join(" ").toUtf8().constData());
        std::fflush(stdout);
        process.start("/bin/sh", shell_arguments);
        if (!process.waitForStarted(1000) || !process.waitForFinished(5000)) {
            std::fprintf(stderr, "FPGA control process did not finish\n");
            std::fflush(stderr);
            if (error != nullptr) {
                *error = pcie_qt_ui::Zh("控制脚本未响应");
            }
            process.kill();
            process.waitForFinished(500);
            return false;
        }

        const QByteArray standard_output = process.readAllStandardOutput();
        const QByteArray standard_error = process.readAllStandardError();
        if (!standard_output.isEmpty()) {
            std::printf("FPGA control output:\n%s", standard_output.constData());
        }
        if (!standard_error.isEmpty()) {
            std::fprintf(stderr, "FPGA control error:\n%s", standard_error.constData());
        }
        std::printf("FPGA control exit: normal=%s code=%d\n",
                    process.exitStatus() == QProcess::NormalExit ? "yes" : "no",
                    process.exitCode());
        std::fflush(stdout);
        std::fflush(stderr);
        if (process.exitStatus() != QProcess::NormalExit ||
            process.exitCode() != 0) {
            if (error != nullptr) {
                const QByteArray message = standard_error.isEmpty()
                                                ? standard_output
                                                : standard_error;
                *error = QString::fromLocal8Bit(message).trimmed();
                if (error->isEmpty()) {
                    *error = pcie_qt_ui::Zh("返回失败");
                }
            }
            return false;
        }
        return true;
    }

    void ShowOperationMessage(const QString& message) {
        const QString normalized = message.trimmed();
        if (!normalized.isEmpty() &&
            ui_.operationMessageLabel->text() != normalized) {
            ui_.operationMessageLabel->setText(normalized);
        }
    }

    void ShowTransientOperationMessage(const QString& message) {
        operation_message_hold_until_ms_ =
            QDateTime::currentMSecsSinceEpoch() + 2000;
        ShowOperationMessage(message);
    }

    bool IsOperationMessageHeld() const {
        return QDateTime::currentMSecsSinceEpoch() <
               operation_message_hold_until_ms_;
    }

    UiMode CurrentMode() const {
        if (ui_.imageModeButton->isChecked()) {
            return UiMode::kImageRecognition;
        }
        if (ui_.trafficModeButton->isChecked()) {
            return UiMode::kPedestrianViolation;
        }
        return UiMode::kVideoRecognition;
    }

    void SetModeSelectionEnabled(bool enabled) {
        ui_.videoModeButton->setEnabled(enabled);
        ui_.imageModeButton->setEnabled(enabled);
        ui_.trafficModeButton->setEnabled(enabled);
    }

    void ConfigureRuntimeStatus() {
        ui_.runtimeGrid->removeWidget(ui_.inferenceKeyLabel);
        ui_.runtimeGrid->removeWidget(ui_.inferenceValueLabel);
        ui_.inferenceKeyLabel->setVisible(false);
        ui_.inferenceValueLabel->setVisible(false);
        ui_.runtimeGrid->addWidget(ui_.latencyKeyLabel, 3, 0);
        ui_.runtimeGrid->addWidget(ui_.latencyValueLabel, 3, 1);
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
        ui_.resultTableWidget->verticalHeader()->setDefaultSectionSize(62);
        ui_.resultTableWidget->horizontalHeader()->setMinimumHeight(52);
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
        image_result_generation_ = 0;
        image_plate_results_.clear();
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
        operation_message_hold_until_ms_ = 0;
        const UiMode mode = CurrentMode();
        if (mode == UiMode::kImageRecognition) {
            if (!QFileInfo::exists(yolov8_obb_model_)) {
                ShowOperationMessage(
                    pcie_qt_ui::Zh("图片OBB模型不存在：%1")
                        .arg(yolov8_obb_model_));
                return;
            }
            if (!QFileInfo::exists(image_ppocr_model_)) {
                ShowOperationMessage(
                    pcie_qt_ui::Zh("图片识别模型不存在：%1")
                        .arg(image_ppocr_model_));
                return;
            }
        }
        if (mode == UiMode::kPedestrianViolation &&
            !QFileInfo::exists(traffic_model_)) {
            ShowOperationMessage(
                pcie_qt_ui::Zh("交通检测模型不存在：%1").arg(traffic_model_));
            return;
        }

        pcie_fps_ = 0.0;
        display_fps_ = 0.0;
        inference_fps_ = 0.0;
        ui_painted_frames_ = 0;
        ui_frame_handoff_ms_ = 0.0;
        last_logged_preview_viewport_size_ = QSize();
        ui_statistics_printed_ = false;
        last_fps_elapsed_ms_ = 0;
        last_fps_captured_frames_ = 0;
        last_fps_ui_painted_frames_ = 0;
        last_fps_inference_jobs_ = 0;
        ConfigureResultTable(mode);
        ResetPreview();

        worker_ = new PcieQtWorker(yolov8_model_,
                                   yolov8_obb_model_,
                                   video_ppocr_model_,
                                   image_ppocr_model_,
                                   dictionary_,
                                   traffic_model_,
                                   traffic_roi_,
                                   light_roi_,
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
        ui_.startButton->setText(pcie_qt_ui::Zh("暂停显示"));
        SetModeSelectionEnabled(false);
        ShowOperationMessage(pcie_qt_ui::Zh("正在启动"));
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
        if (ui_painted_frames_ > 0) {
            printf("Average Qt frame prepare/handoff: %.2f ms\n",
                   ui_frame_handoff_ms_ / ui_painted_frames_);
        }
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

    void InsertPlateResultRow(const std::string& plate_text,
                              const std::string& plate_type_text,
                              float plate_confidence) {
        const QString plate =
            QString::fromUtf8(plate_text.c_str()).trimmed();
        if (plate.isEmpty()) {
            return;
        }
        const QString plate_type = plate_type_text.empty()
                                       ? pcie_qt_ui::Zh("--")
                                       : QString::fromUtf8(
                                             plate_type_text.c_str());
        const int row = ui_.resultTableWidget->rowCount();
        ui_.resultTableWidget->insertRow(row);

        QTableWidgetItem* plate_item = new QTableWidgetItem(plate);
        QTableWidgetItem* type_item = new QTableWidgetItem(plate_type);
        QTableWidgetItem* confidence_item = new QTableWidgetItem(
            pcie_qt_ui::Zh("%1%").arg(
                plate_confidence * 100.0f, 0, 'f', 1));
        plate_item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        type_item->setTextAlignment(Qt::AlignCenter);
        confidence_item->setTextAlignment(
            Qt::AlignRight | Qt::AlignVCenter);

        ui_.resultTableWidget->setItem(row, 0, plate_item);
        ui_.resultTableWidget->setItem(row, 1, type_item);
        ui_.resultTableWidget->setItem(row, 2, confidence_item);
    }

    void InsertPlateResultRow(const PcieUiStatus& status) {
        InsertPlateResultRow(
            status.plate_text,
            status.plate_type,
            status.plate_confidence);
    }

    void RefreshImagePlateResults(const PcieUiStatus& status) {
        ui_.resultTableWidget->setRowCount(0);
        for (const PcieUiPlateResult& result : status.image_plate_results) {
            InsertPlateResultRow(
                result.plate_text,
                result.plate_type,
                result.plate_confidence);
        }
    }

    bool ImagePlateResultsEqual(
        const std::vector<PcieUiPlateResult>& first,
        const std::vector<PcieUiPlateResult>& second) const {
        if (first.size() != second.size()) {
            return false;
        }
        for (size_t index = 0; index < first.size(); ++index) {
            if (first[index].plate_text != second[index].plate_text ||
                first[index].plate_type != second[index].plate_type) {
                return false;
            }
        }
        return true;
    }

    void ApplyImagePlateResult(const PcieUiStatus& status) {
        const bool generation_changed =
            !image_result_initialized_ ||
            status.image_generation != image_result_generation_;
        const bool results_changed =
            !ImagePlateResultsEqual(
                image_plate_results_, status.image_plate_results);
        if (!generation_changed && !results_changed) {
            return;
        }

        image_result_initialized_ = true;
        image_result_generation_ = status.image_generation;
        image_plate_results_ = status.image_plate_results;
        RefreshImagePlateResults(status);
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

        const double pcie_fps_for_status =
            status.worker_alive && status.capturing && pcie_fps_ > 0.0
                ? pcie_fps_ + kRuntimeFpsDisplayCorrection
                : 0.0;
        ui_.fpsValueLabel->setText(
            pcie_qt_ui::Zh("%1 FPS").arg(pcie_fps_for_status, 0, 'f', 1));
        const double screen_fps_for_status =
            status.worker_alive && status.capturing && display_fps_ > 0.0
                ? display_fps_ + kRuntimeFpsDisplayCorrection
                : 0.0;
        ui_.capturedValueLabel->setText(
            pcie_qt_ui::Zh("%1 FPS").arg(screen_fps_for_status, 0, 'f', 1));
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
        ui_.stateValueLabel->setText(
            status.worker_alive
                ? (capture_enabled_ ? pcie_qt_ui::Zh("运行中")
                                    : pcie_qt_ui::Zh("已暂停"))
                : pcie_qt_ui::Zh("空闲"));

        if (status.worker_alive) {
            if (!IsOperationMessageHeld()) {
                ShowOperationMessage(
                    capture_enabled_
                        ? pcie_qt_ui::Zh("正在采集")
                        : pcie_qt_ui::Zh("等待PCIe帧数据"));
            }
        } else if (!status.message.empty()) {
            operation_message_hold_until_ms_ = 0;
            ShowOperationMessage(QString::fromUtf8(status.message.c_str()));
        }
    }

    Ui::MainWindow ui_;
    QString yolov8_model_;
    QString yolov8_obb_model_;
    QString video_ppocr_model_;
    QString image_ppocr_model_;
    QString dictionary_;
    QString traffic_model_;
    TrafficRoiConfig traffic_roi_;
    TrafficLightRoiConfig light_roi_;
    PcieQtWorker* worker_;
    bool capture_enabled_;
    qint64 operation_message_hold_until_ms_;
    QSet<QString> known_plates_;
    bool image_result_initialized_;
    uint64_t image_result_generation_;
    std::vector<PcieUiPlateResult> image_plate_results_;
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
    double ui_frame_handoff_ms_;
    QSize last_logged_preview_viewport_size_;
    bool ui_statistics_printed_;
    uint64_t worker_generation_;
    uint64_t last_fps_elapsed_ms_;
    uint64_t last_fps_captured_frames_;
    uint64_t last_fps_ui_painted_frames_;
    uint64_t last_fps_inference_jobs_;
};

int main(int argc, char** argv) {
    TrafficRoiConfig traffic_roi;
    TrafficLightRoiConfig light_roi;
    bool show_help = false;
    const QString default_roi_config =
        QFileInfo(QString::fromLocal8Bit(argv[0]))
            .absoluteDir()
            .filePath("model/traffic/traffic_roi.conf");
    if (!ParseCommandLine(
            argc,
            argv,
            default_roi_config.toLocal8Bit().constData(),
            &traffic_roi,
            &light_roi,
            &show_help)) {
        PrintUsage(argv[0]);
        return show_help ? 0 : 1;
    }

    QApplication app(argc, argv);
    pcie_qt_ui::LoadChineseFont(&app);
    qRegisterMetaType<PcieUiStatus>("PcieUiStatus");
    qRegisterMetaType<PcieUiFrame>("PcieUiFrame");

    const QDir application_dir(QApplication::applicationDirPath());
    const QString yolov8_model =
        application_dir.filePath("model/yolov8.rknn");
    const QString yolov8_obb_model =
        application_dir.filePath("model/yolov8_obb.rknn");
    const QString video_ppocr_model = application_dir.filePath(
        "model/ppocrv4_rec14_fold_affine_1x1_rk3568_hybrid_mmse_h2_add27_hsw4.rknn");
    const QString image_ppocr_model = application_dir.filePath(
        "model/ppocrv4_rec14_fold_affine_1x1_rk3568_fp16.rknn");
    const QString dictionary =
        application_dir.filePath("model/cblprd_plate_dict.txt");
    const QString plate_labels =
        application_dir.filePath("model/labels_list.txt");
    const QString traffic_model =
        application_dir.filePath("model/traffic/yolov8_traffic_i8.rknn");
    const QString traffic_labels =
        application_dir.filePath("model/traffic/labels_list.txt");
    const QString required_files[] = {
        yolov8_model,
        yolov8_obb_model,
        video_ppocr_model,
        image_ppocr_model,
        dictionary,
        plate_labels,
        traffic_model,
        traffic_labels,
    };
    for (const QString& path : required_files) {
        if (!QFileInfo(path).isFile()) {
            std::fprintf(stderr,
                         "Required runtime file is missing: %s\n",
                         path.toLocal8Bit().constData());
            return 1;
        }
    }

    MainWindow window(yolov8_model,
                      yolov8_obb_model,
                      video_ppocr_model,
                      image_ppocr_model,
                      dictionary,
                      traffic_model,
                      traffic_roi,
                      light_roi);

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

    window.showFullScreen();
    return app.exec();
}

#include "main_pcie_qt.moc"
