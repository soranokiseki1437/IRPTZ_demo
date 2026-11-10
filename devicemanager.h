#ifndef DEVICEMANAGER_H
#define DEVICEMANAGER_H

#include <QObject>
#include <QString>
#include <QMutex>
#include <atomic>
#include "DecodeThread.h"
#include "RecordThread.h"
#include "lensmanager.h"
#include "datarecorder.h"
#include "svsdksession.h"
#include "rtspsource.h"
#include "infraredservice.h"

struct LoginResult {
    bool success;
    uint64_t userID;
    QString errorMsg;
};
Q_DECLARE_METATYPE(LoginResult)

class DeviceManager : public QObject
{
    Q_OBJECT

public:
    explicit DeviceManager(QObject *parent = nullptr);
    ~DeviceManager();

    static DeviceManager *instance;

    bool isConnected() const;
    uint64_t getUserID() const;
    bool isStreaming() const;
    bool isRecording() const;

    SvSdkSession* getSdkSession() { return m_sdkSession; }
    LensManager* getLensManager() { return m_lensManager; }
    DataRecorder* getDataRecorder() { return m_dataRecorder; }
    InfraredService* getInfraredService() { return m_infraredService; }

    void setRtspPath(const QString &path) { m_rtspPath = path; }
    QString getRtspPath() const { return m_rtspPath; }

public slots:
    void login(const QString &ip, const QString &username, const QString &password, uint16_t port = 8000);
    bool logout();
    bool startStream();
    bool stopStream();
    void requestStartStream();
    bool startRecord(const QString& filePath);
    void stopRecord();

    void onPTZAngleReceived(float pan, float tilt, float panSpeed, float tiltSpeed);

signals:
    void connectionStatusChanged(bool connected);
    void errorOccurred(const QString &error);
    void frameReceived(const QImage &frame);
    void streamStarted(bool ok);
    void streamInfo(const QString &info);
    void recordStarted();
    void recordStopped(const QString &filePath);
    void recordFailed(const QString &error);
    void ptzAngleReceived(float pan, float tilt);

private slots:
    void onFrameDecoded(const QImage &frame);
    void onRecordError(const QString &error);
    void onRecordFinished(const QString &filePath);
    void onDataRecordStarted(const QString &filePath);
    void onDataRecordStopped(const QString &filePath);
    void onRtspStreamConnected(int encType);
    void onRtspStreamError(const QString &err);

private:
    SvSdkSession *m_sdkSession = nullptr;
    RtspSource *m_rtspSource = nullptr;
    DecodeThread *m_decodeThread = nullptr;
    RecordThread *m_recordThread = nullptr;
    LensManager *m_lensManager = nullptr;
    DataRecorder *m_dataRecorder = nullptr;
    InfraredService *m_infraredService = nullptr;

    bool m_connected = false;
    bool m_streaming = false;
    std::atomic<bool> m_isRecording{false};

    QString m_ip;
    uint16_t m_port = 8000;
    QString m_username;
    QString m_password;
    QString m_rtspPath = "Streaming/Channels/101";

    mutable QMutex m_mutex;
};

#endif // DEVICEMANAGER_H
