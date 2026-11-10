#include "taskboardwindow.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QTime>
#include <QVBoxLayout>

TaskBoardWindow::TaskBoardWindow(QWidget *parent)
    : QWidget(parent)
{
    QSettings settings("RGB_PTZ", "RGB_PTZ_Integrated");
    m_onTop = settings.value("taskboard/onTop", true).toBool();

    Qt::WindowFlags flags = Qt::Tool | Qt::FramelessWindowHint;
    if (m_onTop) {
        flags |= Qt::WindowStaysOnTopHint;
    }
    setWindowFlags(flags);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle("长任务看板");

    setStyleSheet(
        "QFrame#boardContainer { background-color: rgba(18, 22, 30, 242);"
        " border: 1px solid rgba(255, 255, 255, 45); border-radius: 10px; }"
        "QFrame#boardColumn { background-color: rgba(255, 255, 255, 10);"
        " border: 1px solid rgba(255, 255, 255, 25); border-radius: 8px; }"
        "QFrame#taskCard { background-color: rgba(255, 255, 255, 18);"
        " border: 1px solid rgba(255, 255, 255, 35); border-radius: 6px; }"
        "QPushButton { color: #DCE5F2; background-color: rgba(255, 255, 255, 22);"
        " border: 1px solid rgba(255, 255, 255, 40); border-radius: 4px;"
        " padding: 2px 8px; font-size: 12px; }"
        "QPushButton:hover { background-color: rgba(255, 255, 255, 45); }");

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto *container = new QFrame(this);
    container->setObjectName("boardContainer");
    outer->addWidget(container);

    auto *root = new QVBoxLayout(container);
    root->setContentsMargins(12, 10, 12, 12);
    root->setSpacing(10);

    // 标题栏（同时作为拖拽区域）
    auto *titleBar = new QHBoxLayout;
    titleBar->setSpacing(8);

    auto *title = new QLabel("长任务看板", container);
    title->setStyleSheet("color: #EAF2FF; font-size: 16px; font-weight: bold;");
    titleBar->addWidget(title);

    m_pathLabel = new QLabel(container);
    m_pathLabel->setStyleSheet("color: #7F8A99; font-size: 11px;");
    m_pathLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_pathLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    titleBar->addWidget(m_pathLabel, 1);

    auto *refreshBtn = new QPushButton("刷新", container);
    refreshBtn->setCursor(Qt::PointingHandCursor);
    connect(refreshBtn, &QPushButton::clicked, this, &TaskBoardWindow::refresh);
    titleBar->addWidget(refreshBtn);

    m_pinButton = new QPushButton(m_onTop ? "置顶" : "非置顶", container);
    m_pinButton->setCursor(Qt::PointingHandCursor);
    connect(m_pinButton, &QPushButton::clicked, this, &TaskBoardWindow::toggleOnTop);
    titleBar->addWidget(m_pinButton);

    auto *closeBtn = new QPushButton("×", container);
    closeBtn->setCursor(Qt::PointingHandCursor);
    connect(closeBtn, &QPushButton::clicked, this, &QWidget::close);
    titleBar->addWidget(closeBtn);

    root->addLayout(titleBar);

    // 三列看板
    auto *body = new QHBoxLayout;
    body->setSpacing(10);
    body->addWidget(createColumn("待执行", "#FFC857", {}));
    body->addWidget(createColumn("进行中", "#5AA9FF", {}));
    body->addWidget(createColumn("已完成", "#5FD08A", {}));
    root->addLayout(body, 1);

    const QByteArray geometry = settings.value("taskboard/geometry").toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    } else {
        resize(1040, 580);
        move(120, 80);
    }

    refresh();
}

QString TaskBoardWindow::locateBoardFile() const
{
    QDir dir(QCoreApplication::applicationDirPath());
    for (int depth = 0; depth <= 4; ++depth) {
        const QString candidate = dir.absoluteFilePath("待执行方案/长任务看板.md");
        if (QFileInfo::exists(candidate)) {
            return QDir::cleanPath(candidate);
        }
        if (!dir.cdUp()) {
            break;
        }
    }

    const QString fallback = QDir::cleanPath(QDir::currentPath() + "/待执行方案/长任务看板.md");
    return QFileInfo::exists(fallback) ? fallback : QString();
}

QList<TaskBoardWindow::TaskItem> TaskBoardWindow::parseSection(const QStringList &lines,
                                                               const QString &section) const
{
    QList<TaskItem> result;
    bool inSection = false;

    for (const QString &raw : lines) {
        const QString line = raw.trimmed();

        if (line.startsWith("## ")) {
            inSection = (line.mid(3).trimmed() == section);
            continue;
        }
        if (!inSection || !line.startsWith('|')) {
            continue;
        }

        QStringList cells = line.split('|');
        if (!cells.isEmpty() && cells.first().trimmed().isEmpty()) cells.removeFirst();
        if (!cells.isEmpty() && cells.last().trimmed().isEmpty()) cells.removeLast();
        if (cells.size() < 5) {
            continue;
        }

        const QString id = cells.at(0).trimmed();
        // 跳过表头与 | --- | 分隔行
        if (id.compare("ID", Qt::CaseInsensitive) == 0) {
            continue;
        }
        QString idKey = id;
        idKey.remove('-');
        idKey.remove(':');
        idKey.remove(' ');
        if (idKey.isEmpty()) {
            continue;
        }

        TaskItem item;
        item.id = id;
        item.title = cells.at(1).trimmed();
        item.priority = cells.at(2).trimmed();
        item.acceptance = cells.at(3).trimmed();
        item.plan = cells.at(4).trimmed();
        if (item.title.isEmpty()) {
            continue;
        }
        result.append(item);
    }

    return result;
}

QWidget *TaskBoardWindow::createColumn(const QString &title, const QString &accent,
                                       const QList<TaskItem> &items)
{
    auto *column = new QFrame(this);
    column->setObjectName("boardColumn");
    auto *outer = new QVBoxLayout(column);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(8);

    auto *header = new QLabel(title, column);
    header->setStyleSheet(QString("color: %1; font-size: 14px; font-weight: bold;").arg(accent));
    outer->addWidget(header);

    auto *scroll = new QScrollArea(column);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet(
        "QScrollArea { background: transparent; border: none; }"
        "QScrollBar:vertical { background: rgba(255,255,255,20); width: 8px; border-radius: 4px; }"
        "QScrollBar::handle:vertical { background: rgba(255,255,255,80);"
        " border-radius: 4px; min-height: 24px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");

    auto *content = new QWidget(scroll);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addStretch(1);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);

    if (title == QStringLiteral("待执行")) {
        m_pendingLayout = layout;
    } else if (title == QStringLiteral("进行中")) {
        m_doingLayout = layout;
    } else {
        m_doneLayout = layout;
    }

    if (items.isEmpty()) {
        auto *empty = new QLabel("暂无任务", content);
        empty->setStyleSheet("color: #6E7A8A; font-size: 12px;");
        layout->insertWidget(layout->count() - 1, empty);
    } else {
        for (const TaskItem &item : items) {
            layout->insertWidget(layout->count() - 1, createCard(item));
        }
    }

    return column;
}

QWidget *TaskBoardWindow::createCard(const TaskItem &item)
{
    auto *card = new QFrame(this);
    card->setObjectName("taskCard");
    card->setToolTip(QString("验收标准：%1\n关联方案：%2").arg(item.acceptance, item.plan));

    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(4);

    auto *topRow = new QHBoxLayout;
    topRow->setSpacing(6);

    auto *idLabel = new QLabel(item.id, card);
    idLabel->setStyleSheet("color: #8E9AAB; font-size: 11px;");
    topRow->addWidget(idLabel);

    QString priorityColor = "#9AA4B2";
    if (item.priority == QStringLiteral("高")) {
        priorityColor = "#FF6B6B";
    } else if (item.priority == QStringLiteral("中")) {
        priorityColor = "#FFC857";
    }
    auto *priorityLabel = new QLabel(item.priority, card);
    priorityLabel->setStyleSheet(
        QString("color: %1; font-size: 11px; font-weight: bold;").arg(priorityColor));
    topRow->addWidget(priorityLabel);
    topRow->addStretch(1);
    layout->addLayout(topRow);

    auto *titleLabel = new QLabel(item.title, card);
    titleLabel->setWordWrap(true);
    titleLabel->setStyleSheet("color: #E8EEF7; font-size: 12px;");
    layout->addWidget(titleLabel);

    return card;
}

void TaskBoardWindow::clearColumn(QVBoxLayout *layout)
{
    if (!layout) {
        return;
    }
    // 最后一个 item 是 addStretch，保留
    while (layout->count() > 1) {
        QLayoutItem *item = layout->takeAt(0);
        if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }
}

void TaskBoardWindow::refresh()
{
    const QString path = locateBoardFile();
    if (path.isEmpty()) {
        clearColumn(m_pendingLayout);
        clearColumn(m_doingLayout);
        clearColumn(m_doneLayout);
        auto *missing = new QLabel("未找到 待执行方案/长任务看板.md", this);
        missing->setWordWrap(true);
        missing->setStyleSheet("color: #FF6B6B; font-size: 12px;");
        if (m_pendingLayout) {
            m_pendingLayout->insertWidget(m_pendingLayout->count() - 1, missing);
        }
        m_pathLabel->setText("文件未找到");
        m_pathLabel->setToolTip(QString());
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_pathLabel->setText("读取失败");
        m_pathLabel->setToolTip(path);
        return;
    }

    const QString text = QString::fromUtf8(file.readAll());
    file.close();
    const QStringList lines = text.split('\n');

    const QList<TaskItem> pending = parseSection(lines, QStringLiteral("待执行"));
    const QList<TaskItem> doing = parseSection(lines, QStringLiteral("进行中"));
    const QList<TaskItem> done = parseSection(lines, QStringLiteral("已完成"));

    const auto fill = [this](QVBoxLayout *layout, const QList<TaskItem> &items) {
        if (!layout) {
            return;
        }
        clearColumn(layout);
        if (items.isEmpty()) {
            auto *empty = new QLabel("暂无任务", this);
            empty->setStyleSheet("color: #6E7A8A; font-size: 12px;");
            layout->insertWidget(layout->count() - 1, empty);
            return;
        }
        for (const TaskItem &item : items) {
            layout->insertWidget(layout->count() - 1, createCard(item));
        }
    };

    fill(m_pendingLayout, pending);
    fill(m_doingLayout, doing);
    fill(m_doneLayout, done);

    m_pathLabel->setText(QString("已同步 %1").arg(QTime::currentTime().toString("HH:mm:ss")));
    m_pathLabel->setToolTip(path);
}

void TaskBoardWindow::toggleOnTop()
{
    m_onTop = !m_onTop;
    setWindowFlag(Qt::WindowStaysOnTopHint, m_onTop);
    m_pinButton->setText(m_onTop ? "置顶" : "非置顶");
    show();
    raise();

    QSettings settings("RGB_PTZ", "RGB_PTZ_Integrated");
    settings.setValue("taskboard/onTop", m_onTop);
}

void TaskBoardWindow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && event->position().y() <= 52) {
        m_dragging = true;
        m_dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void TaskBoardWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - m_dragOffset);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void TaskBoardWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
    }
    QWidget::mouseReleaseEvent(event);
}

void TaskBoardWindow::closeEvent(QCloseEvent *event)
{
    QSettings settings("RGB_PTZ", "RGB_PTZ_Integrated");
    settings.setValue("taskboard/geometry", saveGeometry());
    settings.setValue("taskboard/onTop", m_onTop);
    event->accept();
}
