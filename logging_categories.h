#ifndef LOGGING_CATEGORIES_H
#define LOGGING_CATEGORIES_H

#include <QLoggingCategory>

// 分类日志：把高频/逐帧日志从 qDebug 分流到独立类别，
// 运行时可用 QT_LOGGING_RULES 或 QLoggingCategory::setFilterRules 单独开关（见 main.cpp）。
Q_DECLARE_LOGGING_CATEGORY(trackerLog)   // 跟踪：特征提取、滤波、响应、遮挡决策
Q_DECLARE_LOGGING_CATEGORY(ptzCtrlLog)   // 云台：指令交互与运动控制
Q_DECLARE_LOGGING_CATEGORY(streamLog)    // 流媒体：预览、解码、录像

#endif // LOGGING_CATEGORIES_H