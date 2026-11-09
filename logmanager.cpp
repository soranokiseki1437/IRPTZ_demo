#include "logmanager.h"
#include <QDir>
#include <QDateTime>
#include <QCoreApplication>
#include <QMutexLocker>
#include <QDebug>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>

namespace {

// 是否运行在 Qt 主线程（qInstallMessageHandler 非线程安全，只允许主线程调用）
bool isMainThread()
{
    QCoreApplication* app = QCoreApplication::instance();
    return app && QThread::currentThread() == app->thread();
}

const char* typeString(QtMsgType type)
{
    switch (type) {
        case QtDebugMsg:    return "DEBUG";
        case QtInfoMsg:     return "INFO";
        case QtWarningMsg:  return "WARN";
        case QtCriticalMsg: return "ERROR";
        case QtFatalMsg:    return "FATAL";
        default:            return "?????";
    }
}

// 简化文件名（仅保留文件名，不含路径）
QString shortFileName(const QMessageLogContext& context)
{
    if (context.file == nullptr) return QString();
    QString file = QString::fromUtf8(context.file);
    int idx = file.lastIndexOf('/');
    if (idx < 0) idx = file.lastIndexOf('\\');
    return (idx >= 0) ? file.mid(idx + 1) : file;
}

QString formatFileLine(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
    const QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    const QString shortFile = shortFileName(context);
    if (!shortFile.isEmpty() && context.line > 0) {
        return QString("[%1] [%2] [%3:%4] %5")
            .arg(timestamp, QString::fromLatin1(typeString(type)), shortFile)
            .arg(context.line).arg(msg);
    }
    return QString("[%1] [%2] %3")
        .arg(timestamp, QString::fromLatin1(typeString(type)), msg);
}

QString formatConsoleLine(QtMsgType type, const QMessageLogContext& /*context*/, const QString& msg)
{
    const QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    return QString("[%1] [%2] %3")
        .arg(timestamp, QString::fromLatin1(typeString(type)), msg);
}

} // namespace

LogManager::LogManager()
{
}

LogManager::~LogManager()
{
    stopLogging();
}

LogManager& LogManager::instance()
{
    static LogManager inst;
    return inst;
}

void LogManager::startLogging(const QString& logDir)
{
    Q_ASSERT(isMainThread());

    if (m_initialized.load()) return;

    m_logDir = logDir;

    // 确保日志目录存在
    QDir dir;
    if (!dir.exists(m_logDir)) {
        if (!dir.mkpath(m_logDir)) {
            std::fprintf(stderr, "LogManager: 无法创建日志目录: %s\n",
                         m_logDir.toLocal8Bit().constData());
            // 退回到应用程序所在目录
            m_logDir = QCoreApplication::applicationDirPath();
        }
    }

    m_rotateIndex = 0;
    if (!openLogFile()) {
        return;
    }

    m_initialized = true;
    {
        QMutexLocker locker(&m_queueMutex);
        m_running = true;
        m_logQueue.clear();
        m_droppedLines = 0;
    }
    m_writerThread = std::thread(&LogManager::writerLoop, this);

    // 安装全局消息处理
    qInstallMessageHandler(LogManager::messageHandler);

    // 首条日志
    const QString appInfo = QString("%1 %2").arg(QCoreApplication::applicationName(),
                                                 QCoreApplication::applicationVersion());
    const QString startMsg = QString("==== 启动日志 ==== %1 ==== 应用: %2 ==== 文件: %3")
                                 .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"),
                                      appInfo,
                                      QDir::toNativeSeparators(m_currentLogFile));
    qInfo().noquote() << startMsg;
}

void LogManager::stopLogging()
{
    // 析构发生在静态回收阶段时 QCoreApplication 可能已销毁，此时不做线程断言
    if (QCoreApplication::instance()) {
        Q_ASSERT(isMainThread());
    }

    if (!m_initialized.load()) return;

    // 先摘掉全局处理器，避免写线程退出后仍有新日志进入队列
    qInstallMessageHandler(nullptr);
    {
        QMutexLocker locker(&m_queueMutex);
        m_running = false;
        m_queueCond.wakeAll();
    }

    if (m_writerThread.joinable()) {
        m_writerThread.join();  // 写线程退出前会把队列剩余日志全部落盘
    }

    QMutexLocker fileLock(&m_fileMutex);
    m_logStream.flush();
    if (m_logFile.isOpen()) {
        m_logFile.close();
    }
    m_initialized = false;
}

QString LogManager::currentLogFile() const
{
    QMutexLocker fileLock(&m_fileMutex);
    return m_currentLogFile;
}

void LogManager::messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
    LogManager::instance().handleMessage(type, context, msg);
}

void LogManager::handleMessage(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
    if (!m_initialized.load(std::memory_order_relaxed)) return;

    // 只做格式化 + 入队（轻量）：格式化与写盘分离，避免所有线程被磁盘 IO 串行化
    LogEntry entry;
    entry.fileLine = formatFileLine(type, context, msg);
    entry.consoleLine = formatConsoleLine(type, context, msg);

    {
        QMutexLocker locker(&m_queueMutex);
        if (m_logQueue.size() >= MAX_QUEUE_SIZE) {
            // 队列积压（磁盘慢）：丢弃最旧日志并计数，防止内存无限增长
            m_logQueue.dequeue();
            ++m_droppedLines;
        }
        m_logQueue.enqueue(entry);
        m_queueCond.wakeOne();
    }

    if (type == QtFatalMsg) {
        flushFatal();
        std::abort();
    }
}

// 写盘线程：批量出队（最多 64 行或 100ms 一攒），一次 flush
void LogManager::writerLoop()
{
    QMutexLocker locker(&m_queueMutex);

    while (true) {
        if (m_logQueue.isEmpty()) {
            if (!m_running.load()) break;   // 收尾：队列已空且要求退出
            m_queueCond.wait(&m_queueMutex, 100);
            if (m_logQueue.isEmpty()) continue;
        }

        QString fileBatch;
        QString consoleBatch;
        int count = 0;
        while (!m_logQueue.isEmpty() && count < 64) {
            const LogEntry entry = m_logQueue.dequeue();
            fileBatch += entry.fileLine + '\n';
            consoleBatch += entry.consoleLine + '\n';
            ++count;
        }
        int dropped = 0;
        if (m_droppedLines > 0) {
            dropped = m_droppedLines;
            m_droppedLines = 0;
        }
        locker.unlock();

        if (dropped > 0) {
            fileBatch += QString("[LogManager] 队列积压，丢弃了 %1 行日志\n").arg(dropped);
        }
        {
            QMutexLocker fileLock(&m_fileMutex);
            if (m_logFile.size() >= MAX_FILE_SIZE) {
                // 超过 50MB：轮转为 yyyyMMdd_1.log、yyyyMMdd_2.log ...
                m_logStream.flush();
                m_logFile.close();
                ++m_rotateIndex;
                openFileLocked();
            }
            if (m_logFile.isOpen()) {
                m_logStream << fileBatch;
                m_logStream.flush();
            }
        }
        writeConsole(consoleBatch);

        locker.relock();
    }
}

bool LogManager::openLogFile()
{
    QMutexLocker fileLock(&m_fileMutex);
    return openFileLocked();
}

bool LogManager::openFileLocked()
{
    // 按天命名：yyyyMMdd.log（轮转文件名带序号）
    const QString date = QDateTime::currentDateTime().toString("yyyyMMdd");
    const QString fileName = (m_rotateIndex == 0)
        ? QString("%1.log").arg(date)
        : QString("%1_%2.log").arg(date).arg(m_rotateIndex);
    m_currentLogFile = QDir(m_logDir).filePath(fileName);

    m_logFile.setFileName(m_currentLogFile);
    if (!m_logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        std::fprintf(stderr, "LogManager: 无法打开日志文件: %s (error=%d)\n",
                     m_currentLogFile.toLocal8Bit().constData(),
                     static_cast<int>(m_logFile.error()));
        return false;
    }

    // 关联到 QTextStream（Qt 6 默认 UTF-8）
    m_logStream.setDevice(&m_logFile);

    // 写入 UTF-8 BOM，使 Windows 记事本正确识别（只在新文件开头写一次）
    if (m_logFile.size() == 0) {
        const char bom[] = { '\xEF', '\xBB', '\xBF' };
        m_logFile.write(bom, 3);
    }
    return true;
}

void LogManager::writeConsole(const QString& text)
{
    if (text.isEmpty()) return;

#ifdef Q_OS_WIN
    HANDLE hStdErr = GetStdHandle(STD_ERROR_HANDLE);
    if (hStdErr != INVALID_HANDLE_VALUE && hStdErr != nullptr) {
        DWORD mode = 0;
        if (GetConsoleMode(hStdErr, &mode)) {
            // 真正的控制台窗口 —— 用 Unicode 输出
            DWORD written = 0;
            WriteConsoleW(hStdErr, text.utf16(),
                          static_cast<DWORD>(text.size()),
                          &written, nullptr);
            return;
        }
    }
#endif
    // IDE 输出面板或重定向到文件 —— 按系统本地编码（中文 Windows 为 GBK/CP936）输出。
    // 若这里写 UTF-8，IDE 面板按本地编码解码，中文会显示成"鍚姩鏃ュ織"这类乱码。
    std::fprintf(stderr, "%s", text.toLocal8Bit().constData());
}

// 致命错误：与写线程抢占文件锁，把剩余队列同步落盘后调用方 abort
void LogManager::flushFatal()
{
    QString batch;
    {
        QMutexLocker queueLock(&m_queueMutex);
        while (!m_logQueue.isEmpty()) {
            batch += m_logQueue.dequeue().fileLine + '\n';
        }
    }

    QMutexLocker fileLock(&m_fileMutex);
    if (m_logFile.isOpen()) {
        if (!batch.isEmpty()) {
            m_logStream << batch;
        }
        m_logStream.flush();
        m_logFile.close();
    }
}