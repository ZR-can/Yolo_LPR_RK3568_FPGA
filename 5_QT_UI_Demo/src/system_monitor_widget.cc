#include "system_monitor_widget.h"

#include <algorithm>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QPainter>
#include <QPaintEvent>
#include <QRegularExpression>
#include <QStringList>

namespace {

const int kHistorySampleCount = 60;
const int kSampleIntervalMs = 1000;

QString ReadTextFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    return QString::fromLocal8Bit(file.readAll()).trimmed();
}

double ParsePlainNumber(const QString& text, bool* ok) {
    const double value = text.trimmed().toDouble(ok);
    return *ok ? value : -1.0;
}

double ParseNpuPercentage(const QString& text) {
    static const QRegularExpression percentage_pattern(
        QStringLiteral("(\\d+(?:\\.\\d+)?)\\s*%"));
    QRegularExpressionMatchIterator matches =
        percentage_pattern.globalMatch(text);
    double maximum = -1.0;
    while (matches.hasNext()) {
        bool ok = false;
        const double value = matches.next().captured(1).toDouble(&ok);
        if (ok) {
            maximum = std::max(maximum, value);
        }
    }
    if (maximum >= 0.0) {
        return std::min(100.0, maximum);
    }

    bool ok = false;
    const double plain_value = ParsePlainNumber(text, &ok);
    if (ok && plain_value >= 0.0 && plain_value <= 100.0) {
        return plain_value;
    }

    static const QRegularExpression ratio_pattern(
        QStringLiteral("^\\s*(\\d+)\\s+(\\d+)\\s*$"));
    const QRegularExpressionMatch ratio_match = ratio_pattern.match(text);
    if (!ratio_match.hasMatch()) {
        return -1.0;
    }
    const double busy = ratio_match.captured(1).toDouble();
    const double total = ratio_match.captured(2).toDouble();
    if (total <= 0.0 || busy < 0.0 || busy > total) {
        return -1.0;
    }
    return busy * 100.0 / total;
}

quint64 ReadMeminfoValue(const QList<QByteArray>& lines,
                         const QByteArray& key) {
    for (const QByteArray& line : lines) {
        if (!line.startsWith(key)) {
            continue;
        }
        const QList<QByteArray> fields = line.simplified().split(' ');
        if (fields.size() >= 2) {
            bool ok = false;
            const quint64 value = fields.at(1).toULongLong(&ok);
            if (ok) {
                return value;
            }
        }
    }
    return 0;
}

}  // namespace

SystemMonitorWidget::SystemMonitorWidget(QWidget* parent)
    : QWidget(parent),
      cpu_temperature_{QString::fromUtf8("CPU温度"),
                       QStringLiteral("°C"),
                       QColor(QStringLiteral("#f5b94c")),
                       100.0,
                       QVector<double>()},
      npu_load_{QString::fromUtf8("NPU负载"),
                QStringLiteral("%"),
                QColor(QStringLiteral("#35d690")),
                100.0,
                QVector<double>()},
      memory_usage_{QString::fromUtf8("内存占用"),
                    QStringLiteral("%"),
                    QColor(QStringLiteral("#4aa3ff")),
                    100.0,
                    QVector<double>()},
      sample_timer_(this) {
    setMinimumHeight(110);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent);

    sample_timer_.setInterval(kSampleIntervalMs);
    sample_timer_.setTimerType(Qt::CoarseTimer);
    connect(&sample_timer_, &QTimer::timeout, this, [this]() {
        SampleMetrics();
    });
    SampleMetrics();
    sample_timer_.start();
}

QSize SystemMonitorWidget::sizeHint() const {
    return QSize(1200, 130);
}

void SystemMonitorWidget::AppendSample(MetricHistory* metric, double value) {
    if (metric == nullptr) {
        return;
    }
    metric->samples.append(value);
    if (metric->samples.size() > kHistorySampleCount) {
        metric->samples.remove(0);
    }
}

QString SystemMonitorWidget::MetricValueText(
    const MetricHistory& metric) const {
    if (metric.samples.isEmpty() || metric.samples.last() < 0.0) {
        return QStringLiteral("--");
    }
    const int precision = metric.suffix == QStringLiteral("°C") ? 1 : 0;
    return QStringLiteral("%1 %2")
        .arg(metric.samples.last(), 0, 'f', precision)
        .arg(metric.suffix);
}

void SystemMonitorWidget::SampleMetrics() {
    AppendSample(&cpu_temperature_, ReadCpuTemperature());
    AppendSample(&npu_load_, ReadNpuLoad());
    AppendSample(&memory_usage_, ReadMemoryUsage());
    update();
}

double SystemMonitorWidget::ReadCpuTemperature() {
    if (!cpu_temperature_path_.isEmpty()) {
        bool ok = false;
        double value =
            ParsePlainNumber(ReadTextFile(cpu_temperature_path_), &ok);
        if (ok) {
            if (value > 1000.0) {
                value /= 1000.0;
            }
            if (value >= 0.0 && value <= 150.0) {
                return value;
            }
        }
        cpu_temperature_path_.clear();
    }

    QStringList candidates;
    candidates << QStringLiteral("/sys/class/thermal/thermal_zone0/temp");

    const QDir thermal_root(QStringLiteral("/sys/class/thermal"));
    const QStringList zones = thermal_root.entryList(
        QStringList() << QStringLiteral("thermal_zone*"),
        QDir::Dirs | QDir::NoDotAndDotDot,
        QDir::Name);
    for (const QString& zone : zones) {
        candidates << thermal_root.filePath(zone + QStringLiteral("/temp"));
    }
    candidates.removeDuplicates();

    for (const QString& path : candidates) {
        bool ok = false;
        double value = ParsePlainNumber(ReadTextFile(path), &ok);
        if (!ok) {
            continue;
        }
        if (value > 1000.0) {
            value /= 1000.0;
        }
        if (value >= 0.0 && value <= 150.0) {
            cpu_temperature_path_ = path;
            return value;
        }
    }
    cpu_temperature_path_.clear();
    return -1.0;
}

double SystemMonitorWidget::ReadNpuLoad() {
    if (!npu_load_path_.isEmpty()) {
        const double value =
            ParseNpuPercentage(ReadTextFile(npu_load_path_));
        if (value >= 0.0) {
            return value;
        }
        npu_load_path_.clear();
    }

    QStringList candidates;
    candidates
        << QStringLiteral("/sys/kernel/debug/rknpu/load")
        << QStringLiteral("/sys/class/devfreq/fde40000.npu/load")
        << QStringLiteral("/sys/class/devfreq/fde40000.npu/device/load");
    candidates.removeDuplicates();

    for (const QString& path : candidates) {
        const QString text = ReadTextFile(path);
        if (text.isEmpty()) {
            continue;
        }
        const double value = ParseNpuPercentage(text);
        if (value >= 0.0) {
            npu_load_path_ = path;
            return value;
        }
    }
    npu_load_path_.clear();
    return -1.0;
}

double SystemMonitorWidget::ReadMemoryUsage() const {
    QFile file(QStringLiteral("/proc/meminfo"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return -1.0;
    }
    const QList<QByteArray> lines = file.readAll().split('\n');
    const quint64 total = ReadMeminfoValue(lines, QByteArrayLiteral("MemTotal:"));
    quint64 available =
        ReadMeminfoValue(lines, QByteArrayLiteral("MemAvailable:"));
    if (available == 0) {
        available = ReadMeminfoValue(lines, QByteArrayLiteral("MemFree:")) +
                    ReadMeminfoValue(lines, QByteArrayLiteral("Buffers:")) +
                    ReadMeminfoValue(lines, QByteArrayLiteral("Cached:"));
    }
    if (total == 0 || available > total) {
        return -1.0;
    }
    return (total - available) * 100.0 / total;
}

void SystemMonitorWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), QColor(QStringLiteral("#0b1016")));
    painter.setRenderHint(QPainter::Antialiasing, false);

    QFont text_font = painter.font();
    text_font.setPixelSize(18);
    text_font.setBold(true);
    painter.setFont(text_font);

    const MetricHistory* metrics[] = {
        &cpu_temperature_,
        &npu_load_,
        &memory_usage_,
    };
    const int metric_count = sizeof(metrics) / sizeof(metrics[0]);
    for (int metric_index = 0; metric_index < metric_count; ++metric_index) {
        const int left = width() * metric_index / metric_count;
        const int right = width() * (metric_index + 1) / metric_count;
        QRect metric_rect(left, 0, right - left, height());
        metric_rect.adjust(10, 6, -10, -8);

        const MetricHistory& metric = *metrics[metric_index];
        painter.setPen(QColor(QStringLiteral("#94a4b5")));
        painter.drawText(
            metric_rect.adjusted(0, 0, 0, -metric_rect.height() + 30),
            Qt::AlignLeft | Qt::AlignVCenter,
            metric.title);
        painter.setPen(metric.color);
        painter.drawText(
            metric_rect.adjusted(0, 0, 0, -metric_rect.height() + 30),
            Qt::AlignRight | Qt::AlignVCenter,
            MetricValueText(metric));

        QRect chart_rect = metric_rect.adjusted(0, 36, 0, 0);
        painter.fillRect(chart_rect, QColor(QStringLiteral("#0e151d")));
        painter.setPen(QColor(QStringLiteral("#1d2a35")));
        for (int grid = 1; grid < 4; ++grid) {
            const int y = chart_rect.top() +
                          chart_rect.height() * grid / 4;
            painter.drawLine(chart_rect.left(), y, chart_rect.right(), y);
        }

        const int visible_samples =
            std::min(metric.samples.size(), kHistorySampleCount);
        const double slot_width =
            chart_rect.width() / static_cast<double>(kHistorySampleCount);
        for (int sample_index = 0;
             sample_index < visible_samples;
             ++sample_index) {
            const double value = metric.samples.at(
                metric.samples.size() - visible_samples + sample_index);
            if (value < 0.0) {
                continue;
            }
            const int x = chart_rect.right() + 1 -
                          qRound((visible_samples - sample_index) * slot_width);
            const int next_x = chart_rect.right() + 1 -
                               qRound((visible_samples - sample_index - 1) *
                                      slot_width);
            const int bar_width = std::max(1, next_x - x - 1);
            const double normalized =
                std::max(0.0, std::min(1.0, value / metric.maximum));
            const int bar_height = qRound(chart_rect.height() * normalized);
            painter.fillRect(
                QRect(x,
                      chart_rect.bottom() - bar_height + 1,
                      bar_width,
                      bar_height),
                metric.color);
        }

        painter.setPen(QColor(QStringLiteral("#2a3440")));
        painter.drawRect(chart_rect.adjusted(0, 0, -1, -1));
        if (metric_index + 1 < metric_count) {
            painter.drawLine(right, 4, right, height() - 4);
        }
    }
}
