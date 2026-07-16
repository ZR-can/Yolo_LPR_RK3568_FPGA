#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QImage>
#include <QMainWindow>
#include <QPixmap>
#include <QPushButton>
#include <QSet>
#include <QStatusBar>
#include <QThread>

#include "image_utils.h"
#include "pcie_frame_source.h"
#include "pcie_qt_ui_helpers.h"
#include "simple_tracker.h"
#include "ui_mainwindow.h"
#include "yolo_lpr_pipeline.h"

namespace {

const int kInferenceInterval = 2;

using pcie_qt_ui::Zh;

uint64_t NowMilliseconds() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

double ElapsedMilliseconds(const std::chrono::steady_clock::time_point& begin) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - begin)
        .count();
}

image_buffer_t MakeBgr565Image(std::vector<unsigned char>* pixels) {
    image_buffer_t image;
    std::memset(&image, 0, sizeof(image));
    image.width = PcieFrameSource::kFrameWidth;
    image.height = PcieFrameSource::kFrameHeight;
    image.width_stride = PcieFrameSource::kFrameWidth;
    image.height_stride = PcieFrameSource::kFrameHeight;
    image.format = IMAGE_FORMAT_BGR565;
    image.virt_addr = pixels->data();
    image.size = static_cast<int>(pixels->size());
    image.fd = -1;
    return image;
}

std::vector<PipelineResult> ClampResults(const std::vector<PipelineResult>& results) {
    std::vector<PipelineResult> clamped;
    clamped.reserve(results.size());
    for (PipelineResult result : results) {
        result.left = std::max(0, std::min(result.left, PcieFrameSource::kFrameWidth - 1));
        result.top = std::max(0, std::min(result.top, PcieFrameSource::kFrameHeight - 1));
        result.right = std::max(0, std::min(result.right, PcieFrameSource::kFrameWidth - 1));
        result.bottom = std::max(0, std::min(result.bottom, PcieFrameSource::kFrameHeight - 1));
        if (result.right > result.left && result.bottom > result.top) {
            clamped.push_back(result);
        }
    }
    return clamped;
}

void DrawExistingOverlay(image_buffer_t* image, const std::vector<PipelineResult>& results) {
    for (const PipelineResult& result : results) {
        draw_pipeline_result_overlay(image, result);
    }
}

}  // namespace

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
          exit_requested_(false),
          capture_enabled_(true) {}

    void SetCaptureEnabled(bool enabled) {
        capture_enabled_.store(enabled);
    }

    void RequestExit() {
        exit_requested_.store(true);
        capture_enabled_.store(false);
    }

signals:
    void FrameReady(const QImage& image, const UiStatusSnapshot& status);
    void StatusReady(const UiStatusSnapshot& status);
    void WorkerFinished(const UiStatusSnapshot& status);

protected:
    void run() override {
        exit_requested_.store(false);

        UiStatusSnapshot status;
        status.worker_alive = true;
        status.capturing = true;
        status.start_ms = NowMilliseconds();
        status.message = Zh("正在初始化 RKNN 推理管线");
        emit StatusReady(status);

        YOLOLPRPipelineContext pipeline;
        std::memset(&pipeline, 0, sizeof(pipeline));
        if (init_pipeline(yolov8_model_.toLocal8Bit().constData(),
                          lprnet7_model_.toLocal8Bit().constData(),
                          lprnet8_model_.toLocal8Bit().constData(),
                          &pipeline) != 0) {
            status.worker_alive = false;
            status.capturing = false;
            status.message = Zh("推理管线初始化失败");
            emit WorkerFinished(status);
            return;
        }

        PcieFrameSource source;
        status.message = Zh("正在打开 PCIe 图像源");
        emit StatusReady(status);
        if (source.Open() != 0) {
            release_pipeline(&pipeline);
            status.worker_alive = false;
            status.capturing = false;
            status.message = Zh("PCIe 打开失败");
            emit WorkerFinished(status);
            return;
        }

        status.pcie_open = true;
        status.vendor_id = source.DeviceInfo().vendor_id;
        status.device_id = source.DeviceInfo().device_id;
        status.link_speed = source.DeviceInfo().link_speed;
        status.link_width = source.DeviceInfo().link_width;
        status.max_payload_size = source.DeviceInfo().max_payload_size;
        status.message = Zh("采集中");
        emit StatusReady(status);

        std::vector<unsigned char> frame(PcieFrameSource::kFrameBytes);
        SimplePlateTracker tracker;
        uint64_t fps_window_ms = NowMilliseconds();
        int fps_window_frames = 0;
        int frame_id = 0;
        bool last_enabled = true;

        while (!exit_requested_.load()) {
            if (!capture_enabled_.load()) {
                if (last_enabled) {
                    status.capturing = false;
                    status.message = Zh("已暂停，PCIe 保持打开");
                    emit StatusReady(status);
                    last_enabled = false;
                }
                msleep(50);
                continue;
            }

            if (!last_enabled) {
                status.capturing = true;
                status.message = Zh("采集中");
                fps_window_ms = NowMilliseconds();
                fps_window_frames = 0;
                emit StatusReady(status);
                last_enabled = true;
            }

            const PcieFrameReadResult read_result =
                source.ReadFrame(frame.data(), frame.size());
            if (read_result == PCIE_FRAME_RETRY) {
                UpdateDriverStats(source, &status);
                msleep(1);
                continue;
            }
            if (read_result == PCIE_FRAME_FATAL) {
                UpdateDriverStats(source, &status);
                status.message = Zh("PCIe 读帧失败");
                break;
            }

            const uint64_t frame_ready_ms = NowMilliseconds();
            ++status.captured_frames;
            ++fps_window_frames;
            status.frame_id = frame_id;

            image_buffer_t image = MakeBgr565Image(&frame);
            if ((frame_id % kInferenceInterval) == 0) {
                std::vector<PipelineResult> inference_results;
                const std::chrono::steady_clock::time_point begin =
                    std::chrono::steady_clock::now();
                const int ret = process_pipeline(&pipeline, &image, inference_results, false);
                status.inference_ms += ElapsedMilliseconds(begin);
                if (ret == 0) {
                    tracker.update(inference_results, frame_id);
                    ++status.inference_jobs;
                    status.plate_results += inference_results.size();
                } else {
                    ++status.inference_failures;
                }
            }

            std::vector<PipelineResult> display_results;
            tracker.predict(frame_id, display_results);
            display_results = ClampResults(display_results);

            std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
            DrawExistingOverlay(&image, display_results);
            status.overlay_ms += ElapsedMilliseconds(begin);

            for (const PipelineResult& result : display_results) {
                if (result.has_valid_plate_text && !result.plate_name.empty()) {
                    status.plate_text = QString::fromUtf8(result.plate_name.c_str());
                    break;
                }
            }

            begin = std::chrono::steady_clock::now();
            QImage ui_image = pcie_qt_ui::Bgr565ToRgb888(
                frame, PcieFrameSource::kFrameWidth, PcieFrameSource::kFrameHeight);
            status.display_convert_ms += ElapsedMilliseconds(begin);
            if (ui_image.isNull()) {
                ++status.display_failures;
                continue;
            }
            ++status.displayed_frames;
            status.end_to_end_ms += (double)(NowMilliseconds() - frame_ready_ms);

            const uint64_t now_ms = NowMilliseconds();
            if (now_ms - fps_window_ms >= 1000U) {
                status.fps = fps_window_frames * 1000.0 / (now_ms - fps_window_ms);
                fps_window_frames = 0;
                fps_window_ms = now_ms;
            }

            UpdateDriverStats(source, &status);
            emit FrameReady(ui_image, status);
            ++frame_id;
        }

        source.Close();
        release_pipeline(&pipeline);
        status.worker_alive = false;
        status.capturing = false;
        status.pcie_open = false;
        if (status.message == Zh("采集中") || status.message == Zh("已暂停，PCIe 保持打开")) {
            status.message = Zh("已停止");
        }
        emit WorkerFinished(status);
    }

private:
    void UpdateDriverStats(const PcieFrameSource& source, UiStatusSnapshot* status) const {
        const PcieReadStatistics& statistics = source.Statistics();
        status->zero_status_retries = statistics.zero_status_retries;
        status->permission_retries = statistics.permission_retries;
        status->interrupted_retries = statistics.interrupted_retries;
        status->other_errors = statistics.other_errors;
    }

    QString yolov8_model_;
    QString lprnet7_model_;
    QString lprnet8_model_;
    std::atomic<bool> exit_requested_;
    std::atomic<bool> capture_enabled_;
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
          result_placeholder_visible_(true) {
        ui_.setupUi(this);
        setWindowTitle(Zh("RK3568 PCIe 交通目标识别系统"));
        ui_.titleLabel->setText(Zh("交通目标识别"));
        ui_.modeBadgeLabel->setText(pcie_qt_ui::ModeBadgeText(Zh("车牌识别")));
        ui_.resultListWidget->addItem(Zh("暂无识别结果"));
        pcie_qt_ui::ApplyTrafficStyle(this);
        connect(ui_.startButton, &QPushButton::clicked, this, &MainWindow::ToggleCapture);
        connect(ui_.modeComboBox, &QComboBox::currentTextChanged,
                this, &MainWindow::OnModeChanged);
        ApplyStatus(UiStatusSnapshot());
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
        ui_.startButton->setText(capture_enabled_ ? Zh("暂停") : Zh("继续"));
        statusBar()->showMessage(capture_enabled_ ? Zh("采集中") : Zh("已暂停"));
    }

    void OnFrameReady(const QImage& image, const UiStatusSnapshot& status) {
        const QPixmap pixmap = QPixmap::fromImage(image).scaled(
            ui_.videoLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        ui_.videoLabel->setPixmap(pixmap);
        ApplyStatus(status);
    }

    void OnWorkerFinished(const UiStatusSnapshot& status) {
        ApplyStatus(status);
        ui_.startButton->setText(Zh("开始"));
        worker_ = nullptr;
        capture_enabled_ = false;
    }

    void OnModeChanged(const QString& mode) {
        ui_.modeBadgeLabel->setText(pcie_qt_ui::ModeBadgeText(mode));
        if (mode == Zh("行人模式")) {
            statusBar()->showMessage(Zh("行人模式界面已切换，识别模型后续接入"));
        } else {
            statusBar()->showMessage(Zh("车牌识别模式"));
        }
    }

private:
    void StartWorker() {
        worker_ = new PcieQtWorker(yolov8_model_, lprnet7_model_, lprnet8_model_, this);
        capture_enabled_ = true;
        connect(worker_, &PcieQtWorker::FrameReady, this, &MainWindow::OnFrameReady);
        connect(worker_, &PcieQtWorker::StatusReady, this, &MainWindow::ApplyStatus);
        connect(worker_, &PcieQtWorker::WorkerFinished, this, &MainWindow::OnWorkerFinished);
        connect(worker_, &QThread::finished, worker_, &QObject::deleteLater);
        ui_.startButton->setText(Zh("暂停"));
        statusBar()->showMessage(Zh("正在启动"));
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

    void ApplyStatus(const UiStatusSnapshot& status) {
        ui_.fpsValueLabel->setText(QString::number(status.fps, 'f', 1));
        ui_.capturedValueLabel->setText(QString::number(status.captured_frames));
        ui_.inferenceValueLabel->setText(Zh("%1 / 失败 %2")
                                             .arg(status.inference_jobs)
                                             .arg(status.inference_failures));
        if (!status.plate_text.isEmpty()) {
            pcie_qt_ui::AddUniqueRecognitionResult(ui_.resultListWidget,
                                                   &known_plates_,
                                                   &result_placeholder_visible_,
                                                   status.plate_text);
        }
        ui_.deviceValueLabel->setText(status.vendor_id == 0
                                          ? Zh("--")
                                          : Zh("0x%1:0x%2")
                                                .arg(status.vendor_id, 4, 16, QLatin1Char('0'))
                                                .arg(status.device_id, 4, 16, QLatin1Char('0')));
        ui_.linkValueLabel->setText(status.link_speed == 0
                                        ? Zh("--")
                                        : Zh("第%1代 x%2")
                                              .arg(status.link_speed)
                                              .arg(status.link_width));
        ui_.payloadValueLabel->setText(status.max_payload_size == 0
                                           ? Zh("--")
                                           : QString::number(status.max_payload_size));
        ui_.stateValueLabel->setText(status.capturing ? Zh("采集中")
                                                      : status.message);
        if (!status.message.isEmpty()) {
            statusBar()->showMessage(status.message);
        }
    }

    Ui::MainWindow ui_;
    QString yolov8_model_;
    QString lprnet7_model_;
    QString lprnet8_model_;
    PcieQtWorker* worker_;
    bool capture_enabled_;
    QSet<QString> known_plates_;
    bool result_placeholder_visible_;
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    pcie_qt_ui::LoadChineseFont(&app);
    qRegisterMetaType<UiStatusSnapshot>("UiStatusSnapshot");

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
