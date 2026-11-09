#ifndef INFRAREDSERVICE_H
#define INFRAREDSERVICE_H

#include <QObject>
#include <QPoint>
#include <QString>
#include "svsdksession.h"

class InfraredService : public QObject
{
    Q_OBJECT
public:
    explicit InfraredService(SvSdkSession *session, QObject *parent = nullptr);

    int currentPalette() const { return m_currentPalette; }

public slots:
    bool triggerShutterFFC();          // 快门打挡片
    bool triggerBackgroundFFC();       // 背景校准
    bool setFFCMode(bool autoMode);    // true=自动, false=手动
    bool setPalette(int paletteIndex); // 1=白热, 2=黑热, 3=铁红, 4=彩虹, 5=红热, 6=冰雨...
    void lockTrackingPalette(bool trackingActive); // 跟踪期间锁定白热并暂停自动FFC
    bool queryGlobalMaxMinTemp(float &maxTemp, float &minTemp);
    bool queryPointTemp(const QPoint &pt, float &tempOut);
    bool captureTempMatrix(const QString &rawFilePath, LONG channel = 0);

signals:
    void ffcTriggered(int freezeServoMs); // 通知伺服闭环短时间冻结
    void globalTempUpdated(float maxTemp, float minTemp);
    void paletteChanged(int paletteIndex);

private:
    SvSdkSession *m_session = nullptr;
    int m_currentPalette = 1; // 默认 1=白热
    int m_savedPaletteBeforeTrack = 1;
};

#endif // INFRAREDSERVICE_H

