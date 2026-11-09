#ifndef LENSMANAGER_H
#define LENSMANAGER_H

#include <QObject>
#include <QString>
#include <QMutex>
#include <QThread>
#include <QTimer>
#include "svsdksession.h"

class LensWorker : public QObject
{
    Q_OBJECT

public:
    enum LensCommand {
        CmdZoomIn = 0,
        CmdZoomOut,
        CmdFocusNear,
        CmdFocusFar,
        CmdIrisOpen,
        CmdIrisClose,
        CmdOnePushFocus
    };
    Q_ENUM(LensCommand)

    explicit LensWorker(QObject *parent = nullptr);
    ~LensWorker() override;

public slots:
    void setSession(SvSdkSession *session);
    void executeCommand(LensCommand command, uint8_t speed, uint8_t stop);
    void executeSetFocusMode(bool isManual);
    void executeSetIrisMode(bool isManual);

signals:
    void focusModeSetResult(bool success, bool isManual);
    void irisModeSetResult(bool success, bool isManual);

private:
    SvSdkSession *m_session = nullptr;
};

class LensManager : public QObject
{
    Q_OBJECT

public:
    enum FocusMode {
        AutoFocus = 0,    // 自动对焦
        ManualFocus = 1,  // 手动对焦
        SemiAutoFocus = 2 // 半自动对焦
    };
    Q_ENUM(FocusMode)

    explicit LensManager(QObject *parent = nullptr);
    ~LensManager() override;

    void setSession(SvSdkSession *session);
    bool isReady() const;

    FocusMode getFocusMode() const;
    void setFocusMode(bool isManual);
    void setIrisMode(bool isManual);

public slots:
    void zoomIn(uint8_t speed, bool stop = false);
    void zoomOut(uint8_t speed, bool stop = false);
    void focusNear(uint8_t speed, bool stop = false);
    void focusFar(uint8_t speed, bool stop = false);
    void irisOpen(uint8_t speed, bool stop = false);
    void irisClose(uint8_t speed, bool stop = false);
    void onePushFocus();

private slots:
    void onFocusModeSetResult(bool success, bool isManual);
    void onIrisModeSetResult(bool success, bool isManual);

signals:
    void errorOccurred(const QString &error);
    void focusModeChanged(bool isManualFocus);
    void irisModeChanged(bool isManualIris);

    void commandQueued(LensWorker::LensCommand command, uint8_t speed, uint8_t stop);
    void setFocusModeRequested(bool isManual);
    void setIrisModeRequested(bool isManual);

private:
    SvSdkSession *m_session = nullptr;
    bool m_ready = false;
    mutable QMutex m_mutex;

    QThread *m_workerThread = nullptr;
    LensWorker *m_worker = nullptr;

    FocusMode m_focusMode = AutoFocus;
    bool m_isManualIris = false;
};

#endif // LENSMANAGER_H

