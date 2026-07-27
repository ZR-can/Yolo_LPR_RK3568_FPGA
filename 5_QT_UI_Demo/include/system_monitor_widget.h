#ifndef SYSTEM_MONITOR_WIDGET_H_
#define SYSTEM_MONITOR_WIDGET_H_

#include <QColor>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWidget>

class QPaintEvent;

class SystemMonitorWidget : public QWidget {
public:
    explicit SystemMonitorWidget(QWidget* parent = nullptr);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct MetricHistory {
        QString title;
        QString suffix;
        QColor color;
        double maximum;
        QVector<double> samples;
    };

    void SampleMetrics();
    void AppendSample(MetricHistory* metric, double value);
    QString MetricValueText(const MetricHistory& metric) const;
    double ReadCpuTemperature();
    double ReadNpuLoad();
    double ReadMemoryUsage() const;

    MetricHistory cpu_temperature_;
    MetricHistory npu_load_;
    MetricHistory memory_usage_;
    QTimer sample_timer_;
    QString cpu_temperature_path_;
    QString npu_load_path_;
};

#endif  // SYSTEM_MONITOR_WIDGET_H_
