#ifndef LENSMANAGER_H
#define LENSMANAGER_H

#include <QObject>
#include <QString>
#include <QMutex>
#include <QThread>
#include "svsdksession.h"

class LensWorker : public QObject
{
    Q_OBJECT

public:
    enum LensCommand {
        CmdZoomIn = 0,
        CmdZoomOut,
        CmdFocusNear,
        CmdFocusFar
    };
    Q_ENUM(LensCommand)

    explicit LensWorker(QObject *parent = nullptr);
    ~LensWorker() override;

public slots:
    void setSession(SvSdkSession *session);
    void executeCommand(LensCommand command, uint8_t speed, uint8_t stop);

private:
    SvSdkSession *m_session = nullptr;
};

class LensManager : public QObject
{
    Q_OBJECT

public:
    explicit LensManager(QObject *parent = nullptr);
    ~LensManager() override;

    void setSession(SvSdkSession *session);
    bool isReady() const;

public slots:
    void zoomIn(uint8_t speed, bool stop = false);
    void zoomOut(uint8_t speed, bool stop = false);
    void focusNear(uint8_t speed, bool stop = false);
    void focusFar(uint8_t speed, bool stop = false);

signals:
    void errorOccurred(const QString &error);
    void commandQueued(LensWorker::LensCommand command, uint8_t speed, uint8_t stop);

private:
    SvSdkSession *m_session = nullptr;
    bool m_ready = false;
    mutable QMutex m_mutex;

    QThread *m_workerThread = nullptr;
    LensWorker *m_worker = nullptr;
};

#endif // LENSMANAGER_H
