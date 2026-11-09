#ifndef LOGMANAGER_H
#define LOGMANAGER_H

#include <QObject>
#include <QString>
#include <QFile>
#include <QTextStream>
#include <QMutex>
#include <QQueue>
#include <QWaitCondition>
#include <QThread>
#include <atomic>
#include <thread>

class LogManager : public QObject
{
    Q_OBJECT
public:
    static LogManager& instance();

    void startLogging(const QString& logDir = QStringLiteral("日志"));
    void stopLogging();

    QString currentLogFile() const;

private:
    LogManager();
    ~LogManager();
    Q_DISABLE_COPY(LogManager)

    struct LogEntry {
        QString fileLine;
        QString consoleLine;
    };

    static void messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg);
    void handleMessage(QtMsgType type, const QMessageLogContext& context, const QString& msg);

    void writerLoop();                       // 写盘线程：批量出队、一次 flush
    bool openLogFile();                      // 按天命名，超限自动轮转
    bool openFileLocked();                   // 调用者需持有 m_fileMutex
    void writeConsole(const QString& text);  // 控制台输出（Windows 走 WriteConsoleW）
    void flushFatal();                       // 致命日志：同步落盘

    static constexpr qint64 MAX_FILE_SIZE = 50LL * 1024 * 1024;  // 单文件上限 50MB
    static constexpr int MAX_QUEUE_SIZE = 10000;                 // 队列上限，防止慢盘拖垮内存

    QFile m_logFile;
    QTextStream m_logStream;
    mutable QMutex m_fileMutex;  // 保护文件对象（写线程 / 致命同步写）
    mutable QMutex m_queueMutex;
    QWaitCondition m_queueCond;
    QQueue<LogEntry> m_logQueue;
    std::thread m_writerThread;
    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_running{false};
    int m_droppedLines = 0;
    int m_rotateIndex = 0;
    QString m_logDir;
    QString m_currentLogFile;
};

#endif // LOGMANAGER_H