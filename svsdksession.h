#ifndef SVSDKSESSION_H
#define SVSDKSESSION_H

#include <QObject>
#include <QString>
#include <QMutex>
#include <atomic>
#include "svsdk.h"

class SvSdkSession : public QObject
{
    Q_OBJECT
public:
    explicit SvSdkSession(QObject *parent = nullptr);
    ~SvSdkSession() override;

    bool ensureInit();
    bool login(const QString &ip, uint16_t port, const QString &user, const QString &pwd, QString *errOut = nullptr);
    void logout();

    bool isLoggedIn() const { return m_userId.load() >= 0; }
    LONG userId() const { return m_userId.load(); }
    bool isAdvVer164() const { return m_advVer164.load(); }
    QString firmwareVersion() const { return m_fwVersion; }

signals:
    void networkException(uint32_t dwType);
    void reconnected();

private:
    static void CALLBACK onSdkException(DWORD dwType, LONG lUserID, LONG lHandle, void *pUser);
    static bool parseAdvVer164(const char *softVer);

    bool m_inited = false;
    std::atomic<LONG> m_userId{-1};
    std::atomic<bool> m_advVer164{true};
    QString m_fwVersion;
    mutable QMutex m_mutex;
};

#endif // SVSDKSESSION_H

