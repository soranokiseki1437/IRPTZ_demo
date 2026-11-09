#ifndef DATARECORDER_H
#define DATARECORDER_H

#include <QObject>
#include <QTimer>
#include <QMutex>
#include <QVector>
#include <QDateTime>
#include <QFile>
#include <QTextStream>
#include <memory>
#include "datatypes.h"

class DataRecorder : public QObject
{
    Q_OBJECT

public:
    explicit DataRecorder(QObject *parent = nullptr);
    ~DataRecorder();

    // 开始/停止记录
    bool startRecording(const QString &baseFilePath);
    void stopRecording();
    bool isRecording() const;

    // 数据录入接口
    void updatePTZData(float pan, float tilt, float panSpeed = 0, float tiltSpeed = 0);
    void updateDetectionData(float targetX, float targetY, float offsetX, float offsetY, float confidence);
    void updateTrackingData(float kfX, float kfY, float kfVx, float kfVy, float psr, int trackState);
    void updatePTZControlData(int cmdX, int cmdY, int speedX, int speedY);
    void recordFrame(int frameIndex);

    // 获取数据
    QVector<DataPoint> getDataPoints() const;
    int getDataPointCount() const;

    // 导出数据
    bool exportToCsv(const QString &filePath);
    bool exportToExcel(const QString &filePath);

signals:
    void recordingStarted(const QString &filePath);
    void recordingStopped(const QString &filePath);
    void errorOccurred(const QString &error);
    void dataPointAdded(const DataPoint &point);

private slots:
    void onRecordTimer();

private:
    mutable QMutex m_mutex;
    QTimer *m_recordTimer;
    QVector<DataPoint> m_dataPoints;
    
    std::unique_ptr<QTextStream> m_csvStream;  // R-05: 文本流
    std::unique_ptr<QFile> m_csvFile;          // R-05: 持久化文件句柄
    int m_writeCounter = 0;              // R-05: flush 计数器
    static constexpr int kMaxMemoryPoints = 3600;  // R-05: 内存最多保留6分钟数据

    bool m_recording;
    qint64 m_startTimeMs;
    QString m_baseFilePath;
    QString m_csvFilePath;
    
    // 当前状态缓存
    PTZData m_currentPTZ;
    DetectionData m_currentDetection;
    int m_frameCounter;
    int m_recordIntervalMs;  // 记录间隔（毫秒），默认100ms(10Hz)

    // M4: 扩展数据缓存
    float m_currentKfX = 0, m_currentKfY = 0;
    float m_currentKfVx = 0, m_currentKfVy = 0;
    float m_currentPsr = 0;
    int m_currentTrackState = 0;
    int m_currentPtzCmdX = 0, m_currentPtzCmdY = 0;
    int m_currentPtzSpeedX = 0, m_currentPtzSpeedY = 0;
};

#endif // DATARECORDER_H
