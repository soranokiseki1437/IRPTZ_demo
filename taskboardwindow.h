#ifndef TASKBOARDWINDOW_H
#define TASKBOARDWINDOW_H

#include <QWidget>
#include <QPoint>
#include <QList>
#include <QString>

class QLabel;
class QPushButton;
class QVBoxLayout;

// 悬浮长任务看板：读取 待执行方案/长任务看板.md，按三列渲染
class TaskBoardWindow : public QWidget
{
    Q_OBJECT

public:
    explicit TaskBoardWindow(QWidget *parent = nullptr);

public slots:
    void refresh();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    struct TaskItem {
        QString id;
        QString title;
        QString priority;
        QString acceptance;
        QString plan;
    };

    QString locateBoardFile() const;
    QList<TaskItem> parseSection(const QStringList &lines, const QString &section) const;
    QWidget *createColumn(const QString &title, const QString &accent, const QList<TaskItem> &items);
    QWidget *createCard(const TaskItem &item);
    void clearColumn(QVBoxLayout *layout);
    void toggleOnTop();

    QLabel *m_pathLabel = nullptr;
    QPushButton *m_pinButton = nullptr;
    QVBoxLayout *m_pendingLayout = nullptr;
    QVBoxLayout *m_doingLayout = nullptr;
    QVBoxLayout *m_doneLayout = nullptr;

    bool m_dragging = false;
    QPoint m_dragOffset;
    bool m_onTop = true;
};

#endif // TASKBOARDWINDOW_H
