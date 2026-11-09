#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QMessageBox>
#include <QDateTime>
#include <QDebug>
#include <QTimer>
#include <QFileDialog>
#include <QDir>
#include <QMouseEvent>
#include <QPainter>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_ptzController(nullptr)
    , m_ptzThread(nullptr)
    , m_ptzConnected(false)
    , m_tempPollTimer(new QTimer(this))
    , m_ptzManualRepeatTimer(new QTimer(this))
    , m_ptzManualCmd2(0)
    , m_ptzManualHSpeed(0)
    , m_ptzManualVSpeed(0)
    , m_deviceManager(new DeviceManager(this))
    , m_captureManager(new CaptureManager(this))
    , m_imageProcessor(new ImageProcessor(this))
    , m_configManager(new ConfigManager(this))
    , m_rgbConnected(false)
    , m_timedCaptureEnabled(false)
    , m_recordTimer(new QTimer(this))
    , m_recordSeconds(0)
    , m_trackingController(new TrackingController(this))
    , m_isSelectingTarget(false)
    , m_trackBoxVisible(false)
    , m_trackBoxOccluded(false)
{
    ui->setupUi(this);
    initUI();
    
    // 设置管理器之间的关联
    m_captureManager->setDeviceManager(m_deviceManager);
    m_captureManager->setImageProcessor(m_imageProcessor);
    
    // 红外测温与挡片校准信号
    connect(m_tempPollTimer, &QTimer::timeout, this, &MainWindow::onTempPollTimeout);
    connect(m_deviceManager->getInfraredService(), &InfraredService::globalTempUpdated, this, &MainWindow::onGlobalTempUpdated);
    connect(m_deviceManager->getInfraredService(), &InfraredService::ffcTriggered, m_trackingController, &TrackingController::pauseServo);

    // 连接信号槽
    connect(m_deviceManager, &DeviceManager::connectionStatusChanged, this, &MainWindow::onConnectionStatusChanged);
    connect(m_deviceManager, &DeviceManager::errorOccurred, this, &MainWindow::onErrorOccurred);
    connect(m_deviceManager, &DeviceManager::frameReceived, this, &MainWindow::onFrameReceived);
    connect(m_deviceManager, &DeviceManager::streamStarted, this, [this](bool ok){
        if (!ok) ui->statusbar->showMessage("开启视频预览失败", 3000);
    });
    connect(m_deviceManager, &DeviceManager::recordStarted, this, &MainWindow::onRecordStarted);
    connect(m_deviceManager, &DeviceManager::recordStopped, this, &MainWindow::onRecordStopped);
    connect(m_captureManager, &CaptureManager::captureFinished, this, &MainWindow::onCaptureFinished);
    connect(m_captureManager, &CaptureManager::errorOccurred, this, &MainWindow::onErrorOccurred);
    connect(m_imageProcessor, &ImageProcessor::errorOccurred, this, &MainWindow::onErrorOccurred);

    // P8：退出时保存配置（ConfigManager 析构不再写盘，QApplication 收尾阶段 QSettings 不可靠）
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this](){
        m_configManager->saveConfig();
        if (m_deviceManager && m_deviceManager->getDataRecorder()) {
            m_deviceManager->getDataRecorder()->stopRecording();
        }
    });

    // 连接目标跟踪信号槽
    connect(m_trackingController, &TrackingController::stateChanged, this, &MainWindow::onTrackStateChanged);
    connect(m_trackingController, &TrackingController::trackingResult, this, &MainWindow::onTrackResult);
    connect(m_trackingController, &TrackingController::drawTrackingBox, this, &MainWindow::onTrackBoxDraw);
    connect(m_trackingController, &TrackingController::statusMessage, this, &MainWindow::onTrackStatusMessage);
    connect(m_trackingController, &TrackingController::ptzControlDelta, this, &MainWindow::onPtzControlDelta);
    
    // 录像定时器
    m_recordTimer->setInterval(1000);
    connect(m_recordTimer, &QTimer::timeout, this, &MainWindow::onRecordTimerTimeout);
    
    // 初始化 PTZ 方向盘控制
    // 按住期间以 200ms 周期重发方向命令，维持云台平滑转动
    ui->left_up->setAutoRepeat(false);
    ui->up->setAutoRepeat(false);
    ui->right_up->setAutoRepeat(false);
    ui->left->setAutoRepeat(false);
    ui->right->setAutoRepeat(false);
    ui->left_down->setAutoRepeat(false);
    ui->down->setAutoRepeat(false);
    ui->right_down->setAutoRepeat(false);

    m_ptzManualRepeatTimer->setInterval(200);
    connect(m_ptzManualRepeatTimer, &QTimer::timeout, this, [this]() {
        emit ptzMoveDirection(m_ptzManualCmd2, m_ptzManualHSpeed, m_ptzManualVSpeed);
    });

    const auto bindDirection = [this](QPushButton *btn, uint8_t cmd2, int dirX, int dirY) {
        connect(btn, &QPushButton::pressed, this, [this, cmd2, dirX, dirY]() {
            const uint8_t spd = static_cast<uint8_t>(ui->spinSpeed->value() * 63);
            startManualMove(cmd2,
                            static_cast<uint8_t>(dirX ? spd : 0),
                            static_cast<uint8_t>(dirY ? spd : 0));
        });
        connect(btn, &QPushButton::released, this, &MainWindow::stopManualMove);
    };

    bindDirection(ui->up,         0x08, 0, 1);  // 上
    bindDirection(ui->down,       0x10, 0, 1);  // 下
    bindDirection(ui->left,       0x04, 1, 0);  // 左
    bindDirection(ui->right,      0x02, 1, 0);  // 右
    bindDirection(ui->left_up,    0x0C, 1, 1);  // 左上
    bindDirection(ui->right_up,   0x0A, 1, 1);  // 右上
    bindDirection(ui->left_down,  0x14, 1, 1);  // 左下
    bindDirection(ui->right_down, 0x12, 1, 1);  // 右下
    
    // === 镜头控制按钮绑定 ===
    // 【关键】镜头控制按钮禁用 autoRepeat！
    // 因为只需要一次 pressed(start) + 一次 released(stop)，不需要重复发送！
    if (ui->btnZoomIn) ui->btnZoomIn->setAutoRepeat(false);
    if (ui->btnZoomOut) ui->btnZoomOut->setAutoRepeat(false);
    if (ui->btnFocusNear) ui->btnFocusNear->setAutoRepeat(false);
    if (ui->btnFocusFar) ui->btnFocusFar->setAutoRepeat(false);
    if (ui->btnIrisOpen) ui->btnIrisOpen->setAutoRepeat(false);
    if (ui->btnIrisClose) ui->btnIrisClose->setAutoRepeat(false);
    
    // 变倍+
    if (ui->btnZoomIn) {
        connect(ui->btnZoomIn, &QPushButton::pressed, this, [this]() {
            uint8_t speed = static_cast<uint8_t>(ui->spinLensSpeed->value());
            m_deviceManager->getLensManager()->zoomIn(speed, false);
        });
        connect(ui->btnZoomIn, &QPushButton::released, this, [this]() {
            uint8_t speed = static_cast<uint8_t>(ui->spinLensSpeed->value());
            m_deviceManager->getLensManager()->zoomIn(speed, true);
        });
    }
    
    // 变倍-
    if (ui->btnZoomOut) {
        connect(ui->btnZoomOut, &QPushButton::pressed, this, [this]() {
            uint8_t speed = static_cast<uint8_t>(ui->spinLensSpeed->value());
            m_deviceManager->getLensManager()->zoomOut(speed, false);
        });
        connect(ui->btnZoomOut, &QPushButton::released, this, [this]() {
            uint8_t speed = static_cast<uint8_t>(ui->spinLensSpeed->value());
            m_deviceManager->getLensManager()->zoomOut(speed, true);
        });
    }
    
    // 聚焦近
    if (ui->btnFocusNear) {
        connect(ui->btnFocusNear, &QPushButton::pressed, this, [this]() {
            uint8_t speed = static_cast<uint8_t>(ui->spinLensSpeed->value());
            m_deviceManager->getLensManager()->focusNear(speed, false);
        });
        connect(ui->btnFocusNear, &QPushButton::released, this, [this]() {
            uint8_t speed = static_cast<uint8_t>(ui->spinLensSpeed->value());
            m_deviceManager->getLensManager()->focusNear(speed, true);
        });
    }
    
    // 聚焦远
    if (ui->btnFocusFar) {
        connect(ui->btnFocusFar, &QPushButton::pressed, this, [this]() {
            uint8_t speed = static_cast<uint8_t>(ui->spinLensSpeed->value());
            m_deviceManager->getLensManager()->focusFar(speed, false);
        });
        connect(ui->btnFocusFar, &QPushButton::released, this, [this]() {
            uint8_t speed = static_cast<uint8_t>(ui->spinLensSpeed->value());
            m_deviceManager->getLensManager()->focusFar(speed, true);
        });
    }
    
    // 光圈大
    if (ui->btnIrisOpen) {
        connect(ui->btnIrisOpen, &QPushButton::pressed, this, [this]() {
            uint8_t speed = static_cast<uint8_t>(ui->spinLensSpeed->value());
            m_deviceManager->getLensManager()->irisOpen(speed, false);
        });
        connect(ui->btnIrisOpen, &QPushButton::released, this, [this]() {
            uint8_t speed = static_cast<uint8_t>(ui->spinLensSpeed->value());
            m_deviceManager->getLensManager()->irisOpen(speed, true);
        });
    }
    
    // 光圈小
    if (ui->btnIrisClose) {
        connect(ui->btnIrisClose, &QPushButton::pressed, this, [this]() {
            uint8_t speed = static_cast<uint8_t>(ui->spinLensSpeed->value());
            m_deviceManager->getLensManager()->irisClose(speed, false);
        });
        connect(ui->btnIrisClose, &QPushButton::released, this, [this]() {
            uint8_t speed = static_cast<uint8_t>(ui->spinLensSpeed->value());
            m_deviceManager->getLensManager()->irisClose(speed, true);
        });
    }
    
    // 聚焦模式切换
    if (ui->checkBoxManualFocus) {
        connect(ui->checkBoxManualFocus, &QCheckBox::clicked, this, &MainWindow::on_checkBoxManualFocus_clicked);
        connect(m_deviceManager->getLensManager(), &LensManager::focusModeChanged, this, &MainWindow::onFocusModeChanged);
    }
    
    // 光圈模式切换
    if (ui->checkBoxManualIris) {
        connect(ui->checkBoxManualIris, &QCheckBox::clicked, this, &MainWindow::on_checkBoxManualIris_clicked);
        connect(m_deviceManager->getLensManager(), &LensManager::irisModeChanged, this, &MainWindow::onIrisModeChanged);
    }
}

MainWindow::~MainWindow()
{
    if (m_ptzThread && m_ptzController) {
        // 在 PTZ 线程中准备析构，避免跨线程释放
        QMetaObject::invokeMethod(m_ptzController, "prepareForDestruction",
                                  Qt::BlockingQueuedConnection);
    }
    if (m_ptzThread) {
        disconnect(m_ptzThread, &QThread::finished, m_ptzController, &QObject::deleteLater);
        m_ptzThread->quit();
        m_ptzThread->wait();
    }
    delete m_ptzController;
    m_ptzController = nullptr;
    delete m_ptzThread;
    m_ptzThread = nullptr;
    delete ui;
}

void MainWindow::initUI()
{
    // 初始化 PTZ 模块
    initPTZModule();
    
    // 初始化 RGB 模块
    initRGBModule();

    // 初始化目标跟踪模块
    initTrackingModule();

    // 图像调节与校准控件连接
    connect(ui->checkOSD, &QCheckBox::checkStateChanged, this, [this](Qt::CheckState state){
        m_tempOsdEnabled = (state == Qt::Checked);
    });

    connect(ui->btnCaptureTemp, &QPushButton::clicked, this, [this](){
        if (!m_deviceManager || !m_deviceManager->isConnected()) {
            QMessageBox::warning(this, "提示", "请先连接设备！");
            return;
        }
        QString saveDir = m_configManager->getSavePath();
        QString fileName = QString("%1/temp_%2.raw")
                           .arg(saveDir, QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
        if (m_deviceManager->getInfraredService()->captureTempMatrix(fileName)) {
            ui->textLog->append(QString("[系统] 温度矩阵已导出: %1").arg(fileName));
            ui->statusbar->showMessage("温度矩阵导出成功", 3000);
        } else {
            ui->statusbar->showMessage("温度矩阵导出失败", 3000);
        }
    });

    // 更新 UI 状态
    updatePTZUIState();
    updateRGBUIState();
    updateTrackingUIState();
}

void MainWindow::initPTZModule()
{
    updatePTZUIState();
}

void MainWindow::initRGBModule()
{
    // 加载配置
    ui->lineEditIP->setText(m_configManager->getDeviceIP());
    ui->lineEditUsername->setText(m_configManager->getDeviceUsername());
    ui->lineEditPassword->setText(m_configManager->getDevicePassword());
    ui->spinBoxInterval->setValue(m_configManager->getCaptureInterval());
    ui->spinBoxQuality->setValue(m_configManager->getCaptureQuality());
    ui->lineEditSavePath->setText(m_configManager->getSavePath());

    // 设置保存路径
    m_captureManager->setSavePath(m_configManager->getSavePath());
    m_deviceManager->setRtspPath(m_configManager->getRtspPath());

    // 初始化镜头控制按钮
    if (ui->btnZoomIn) ui->btnZoomIn->setEnabled(false);
    if (ui->btnZoomOut) ui->btnZoomOut->setEnabled(false);
    if (ui->btnFocusNear) ui->btnFocusNear->setEnabled(false);
    if (ui->btnFocusFar) ui->btnFocusFar->setEnabled(false);
    if (ui->btnIrisOpen) ui->btnIrisOpen->setEnabled(false);
    if (ui->btnIrisClose) ui->btnIrisClose->setEnabled(false);
    if (ui->spinLensSpeed) ui->spinLensSpeed->setEnabled(false);
}

void MainWindow::initTrackingModule()
{
    // UI 控件已在 mainwindow.ui 中添加，直接使用 ui-> 访问
    ui->btnTrackStart->setEnabled(false);
    ui->btnTrackStop->setEnabled(false);

    // 从 ConfigManager 读取默认值并同步到 UI 控件
    ui->spinTrackDeadZone->setValue(m_configManager->getTrackDeadZone());
    ui->spinTrackMaxSpeed->setValue(m_configManager->getTrackMaxSpeed());
    ui->spinTrackKp->setValue(qRound(m_configManager->getTrackKp() * 100.0f));
    ui->spinTrackKi->setValue(qRound(m_configManager->getTrackKi() * 100.0f));
    ui->spinTrackKd->setValue(qRound(m_configManager->getTrackKd() * 100.0f));
    ui->spinTrackKff->setValue(qRound(m_configManager->getTrackKff() * 100.0f));
    ui->spinTrackPredict->setValue(m_configManager->getTrackTPredict());

    // 同步到 TrackingController
    m_trackingController->setDeadZonePixels(m_configManager->getTrackDeadZone());
    m_trackingController->setMaxSpeed(m_configManager->getTrackMaxSpeed());
    m_trackingController->setProportionalGain(m_configManager->getTrackKp());
    m_trackingController->setIntegralGain(m_configManager->getTrackKi());
    m_trackingController->setDerivativeGain(m_configManager->getTrackKd());
    m_trackingController->setFeedforwardGain(m_configManager->getTrackKff());
    m_trackingController->setPredictHorizon(m_configManager->getTrackTPredict());

    // 连接跟踪按钮
    connect(ui->btnTrackSelect, &QPushButton::clicked, this, &MainWindow::on_btnTrackSelect_clicked);
    connect(ui->btnTrackStart, &QPushButton::clicked, this, &MainWindow::on_btnTrackStart_clicked);
    connect(ui->btnTrackStop, &QPushButton::clicked, this, &MainWindow::on_btnTrackStop_clicked);
    connect(ui->btnTrackResetParams, &QPushButton::clicked, this, &MainWindow::on_btnTrackResetParams_clicked);
}

void MainWindow::updateTrackingUIState()
{
    bool canTrack = m_rgbConnected;
    TrackingController::TrackingState state = m_trackingController->state();

    ui->btnTrackSelect->setEnabled(canTrack && (state == TrackingController::Idle));
    ui->btnTrackStart->setEnabled(canTrack && (state == TrackingController::Selecting));
    ui->btnTrackStop->setEnabled(state == TrackingController::Initializing ||
                                 state == TrackingController::Tracking ||
                                 state == TrackingController::Lost ||
                                 state == TrackingController::Paused);

    // 更新状态标签
    QString statusText;
    switch (state) {
    case TrackingController::Idle: statusText = "空闲"; break;
    case TrackingController::Selecting: statusText = "等待框选..."; break;
    case TrackingController::Initializing: statusText = "初始化中..."; break;
    case TrackingController::Tracking: statusText = "跟踪中"; break;
    case TrackingController::Lost: statusText = "目标丢失"; break;
    case TrackingController::Paused: statusText = "已暂停"; break;
    }

    if (!canTrack) statusText = "相机未连接";
    ui->labelTrackStatus->setText(statusText);
}

void MainWindow::updatePTZUIState()
{
    ui->btnMove->setEnabled(m_ptzConnected);
    ui->btnStop->setEnabled(m_ptzConnected);
    ui->btnQuery->setEnabled(m_ptzConnected);
    ui->btnReset->setEnabled(m_ptzConnected);
    ui->spinPan->setEnabled(m_ptzConnected);
    ui->spinTilt->setEnabled(m_ptzConnected);
    ui->spinSpeed->setEnabled(m_ptzConnected);
    ui->groupBox_2->setEnabled(m_ptzConnected);
}

void MainWindow::updateRGBUIState()
{
    if (m_rgbConnected) {
        ui->rgbBtnConnect->setText("断开");
        ui->rgbLabelStatus->setText("状态: 已连接");
        ui->rgbLabelStatus->setStyleSheet("color: green;");
        
        // 启用抓图控制
        ui->rgbBtnCaptureOnce->setEnabled(true);
        ui->rgbCheckBoxTimedCapture->setEnabled(true);
        ui->spinBoxInterval->setEnabled(true);
        ui->spinBoxQuality->setEnabled(true);
        // 启用录像按钮
        ui->rgbBtnRecord->setEnabled(true);
        ui->lineEditVideoSavePath->setEnabled(true);
        ui->rgbBtnSelectVideoPath->setEnabled(true);
        
        // 启用镜头控制按钮（变焦始终启用）
        if (ui->btnZoomIn) ui->btnZoomIn->setEnabled(true);
        if (ui->btnZoomOut) ui->btnZoomOut->setEnabled(true);
        if (ui->spinLensSpeed) ui->spinLensSpeed->setEnabled(true);
        if (ui->checkBoxManualFocus) ui->checkBoxManualFocus->setEnabled(true);
        if (ui->checkBoxManualIris) ui->checkBoxManualIris->setEnabled(true);
        
        // 聚焦按钮和光圈按钮的状态由相应的信号控制，这里不需要设置
        // 因为登录成功后会自动触发 getFocusModeFromDevice() 和 getIrisModeFromDevice()，然后触发信号更新
    } else {
        ui->rgbBtnConnect->setText("连接");
        ui->rgbLabelStatus->setText("状态: 未连接");
        ui->rgbLabelStatus->setStyleSheet("color: red;");
        
        // 禁用抓图控制
        ui->rgbBtnCaptureOnce->setEnabled(false);
        ui->rgbCheckBoxTimedCapture->setEnabled(false);
        ui->rgbCheckBoxTimedCapture->setChecked(false);
        m_timedCaptureEnabled = false;
        m_captureManager->stopTimedCapture();
        ui->spinBoxInterval->setEnabled(false);
        ui->spinBoxQuality->setEnabled(false);
        // 禁用录像按钮
        ui->rgbBtnRecord->setEnabled(false);
        ui->rgbBtnRecord->setChecked(false);
        ui->rgbBtnRecord->setText("开始录像");
        ui->rgbLabelRecordStatus->setText("就绪");
        ui->rgbLabelRecordTime->setText("00:00:00");
        ui->lineEditVideoSavePath->setEnabled(false);
        ui->rgbBtnSelectVideoPath->setEnabled(false);
        
        // 禁用所有镜头控制按钮
        if (ui->btnZoomIn) ui->btnZoomIn->setEnabled(false);
        if (ui->btnZoomOut) ui->btnZoomOut->setEnabled(false);
        if (ui->btnFocusNear) ui->btnFocusNear->setEnabled(false);
        if (ui->btnFocusFar) ui->btnFocusFar->setEnabled(false);
        if (ui->btnIrisOpen) ui->btnIrisOpen->setEnabled(false);
        if (ui->btnIrisClose) ui->btnIrisClose->setEnabled(false);
        if (ui->spinLensSpeed) ui->spinLensSpeed->setEnabled(false);
        if (ui->checkBoxManualFocus) ui->checkBoxManualFocus->setEnabled(false);
    }
}

void MainWindow::displayImage(const QImage &image)
{
    if (image.isNull()) {
        return;
    }

    // 保存最后一帧（用于框选初始化）
    m_lastFrame = image.copy();

    // 显示图片（C8：缩放结果缓存到 m_scaledPixmap，跟踪框重绘时复用）
    QPixmap pixmap = QPixmap::fromImage(image);
    QSize labelSize = ui->rgbLabelImage->size();
    m_scaledPixmap = pixmap.scaled(labelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_scaledFrameSize = image.size();

    // 现场排障：确认显示控件尺寸有效（label 尺寸为 0 时 scaled 结果为空，界面只会显示占位文字）
    static bool s_firstDisplayLogged = false;
    if (!s_firstDisplayLogged) {
        s_firstDisplayLogged = true;
        qInfo() << "[MainWindow] 首次显示: label 尺寸 =" << labelSize
                << " 可见 =" << ui->rgbLabelImage->isVisible()
                << " 缩放后 =" << m_scaledPixmap.size();
    }

    ui->labelVideoResolution->setText(QString("分辨率: %1×%2").arg(image.width()).arg(image.height()));
    ui->rgbLabelImage->setPixmap(drawTrackBoxOnPixmap(m_scaledPixmap));
}

// 在缩放 pixmap 上叠画跟踪框与测温信息
QPixmap MainWindow::drawTrackBoxOnPixmap(const QPixmap &base) const
{
    QPixmap pm = base;
    if (pm.isNull()) return pm;

    QPainter painter(&pm);
    painter.setRenderHint(QPainter::Antialiasing);

    // 绘制全屏最高/最低温 HUD
    if (m_tempOsdEnabled && (m_globalMaxTemp != 0.0f || m_globalMinTemp != 0.0f)) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 140));
        QRect hudRect(pm.width() - 170, 10, 160, 48);
        painter.drawRoundedRect(hudRect, 5, 5);

        painter.setPen(QColor(255, 90, 90));
        painter.drawText(hudRect.adjusted(8, 5, -8, -25), Qt::AlignLeft | Qt::AlignVCenter,
                         QString("最高温: %1 ℃").arg(m_globalMaxTemp, 0, 'f', 1));

        painter.setPen(QColor(90, 180, 255));
        painter.drawText(hudRect.adjusted(8, 25, -8, -5), Qt::AlignLeft | Qt::AlignVCenter,
                         QString("最低温: %1 ℃").arg(m_globalMinTemp, 0, 'f', 1));
    }

    // 绘制目标跟踪框
    if (m_trackBoxVisible && !m_trackBox.isEmpty() && m_scaledFrameSize.isValid()) {
        const float scaleX = static_cast<float>(pm.width()) / m_scaledFrameSize.width();
        const float scaleY = static_cast<float>(pm.height()) / m_scaledFrameSize.height();

        const QRectF scaledBox(
            m_trackBox.x() * scaleX,
            m_trackBox.y() * scaleY,
            m_trackBox.width() * scaleX,
            m_trackBox.height() * scaleY
        );

        if (m_trackBoxOccluded) {
            painter.setPen(QPen(Qt::red, 2, Qt::DashLine));
        } else {
            painter.setPen(QPen(Qt::green, 2, Qt::SolidLine));
        }
        painter.drawRect(scaledBox);

        // 绘制中心十字
        const QPointF center = scaledBox.center();
        painter.drawLine(QPointF(center.x() - 8, center.y()), QPointF(center.x() + 8, center.y()));
        painter.drawLine(QPointF(center.x(), center.y() - 8), QPointF(center.x(), center.y() + 8));
    }

    painter.end();
    return pm;
}

void MainWindow::addToHistory(const QImage & /*image*/, const QString &filePath)
{
    // 获取文件名
    QString fileName = QFileInfo(filePath).fileName();
    
    // 创建列表项
    QListWidgetItem *item = new QListWidgetItem(fileName);
    ui->rgbListWidgetHistory->insertItem(0, item);
    
    // 限制历史记录数量
    if (ui->rgbListWidgetHistory->count() > 10) {
        delete ui->rgbListWidgetHistory->takeItem(ui->rgbListWidgetHistory->count() - 1);
    }
}

// PTZ 控制相关槽
void MainWindow::on_btnMove_clicked()
{
    if (!m_ptzController) return;
    
    // 获取数值
    float pan = static_cast<float>(ui->spinPan->value());
    float tilt = static_cast<float>(ui->spinTilt->value());
    float speed = static_cast<float>(ui->spinSpeed->value());
    
    // 通过信号调用，避免跨线程直接调用
    emit ptzMoveTo(pan, tilt, speed);
}

void MainWindow::on_btnStop_clicked()
{
    if (!m_ptzController) return;
    emit ptzStop();
}

void MainWindow::on_btnQuery_clicked()
{
    if (!m_ptzController) return;
    emit ptzQueryAngle();
}

void MainWindow::on_btnReset_clicked()
{
    if (!m_ptzController) return;
    
    qDebug() << "[复位] 云台回到初始位置 (0, 0)";
    ui->textLog->append("云台复位 -> 回到初始位置 (0°, 0°)");
    
    // 从界面速度控件读取速度（与方向按钮保持一致），不再使用固定 0.5
    float speed = static_cast<float>(ui->spinSpeed->value());
    qDebug() << "[复位] 使用界面速度:" << speed;
    emit ptzMoveTo(0.0f, 0.0f, speed);
}

// 按住方向键：立即下发一次方向命令，并启动周期重发维持云台持续转动
void MainWindow::startManualMove(uint8_t cmd2, uint8_t hSpeed, uint8_t vSpeed)
{
    m_ptzManualCmd2 = cmd2;
    m_ptzManualHSpeed = hSpeed;
    m_ptzManualVSpeed = vSpeed;
    emit ptzMoveDirection(cmd2, hSpeed, vSpeed);
    m_ptzManualRepeatTimer->start();
}

// 松开方向键：停止周期重发并下发停止指令
void MainWindow::stopManualMove()
{
    m_ptzManualRepeatTimer->stop();
    emit ptzStop();
}

void MainWindow::handleAngleReceived(float pan, float tilt, float panSpeed, float tiltSpeed)
{
    // 更新 DataRecorder 的 PTZ 数据（包含速度）
    if (m_deviceManager && m_deviceManager->getDataRecorder()) {
        m_deviceManager->getDataRecorder()->updatePTZData(pan, tilt, panSpeed, tiltSpeed);
    }
    
    // 更新日志
    ui->textLog->append(QString("云台反馈 -> 水平: %1°  垂直: %2°  水平速度: %3  垂直速度: %4")
                        .arg(pan).arg(tilt).arg(panSpeed).arg(tiltSpeed));
}

// RGB 图像捕获相关槽
void MainWindow::on_rgbBtnConnect_clicked()
{
    if (m_rgbConnected) {
        // 断开连接
        m_deviceManager->logout();
    } else {
        // 连接设备
        QString ip = ui->lineEditIP->text();
        QString username = ui->lineEditUsername->text();
        QString password = ui->lineEditPassword->text();
        
        // 保存配置
        m_configManager->setDeviceIP(ip);
        m_configManager->setDeviceUsername(username);
        m_configManager->setDevicePassword(password);
        m_configManager->saveConfig();
        
        // 尝试连接
        m_deviceManager->login(ip, username, password);
    }
}

void MainWindow::on_rgbBtnCaptureOnce_clicked()
{
    if (!m_rgbConnected) {
        return;
    }
    
    // 保存配置
    m_configManager->setCaptureQuality(ui->spinBoxQuality->value());
    m_configManager->saveConfig();
    
    // 执行单次抓图
    m_captureManager->captureOnce(ui->spinBoxQuality->value(), 1920, 1080);
}

void MainWindow::on_rgbCheckBoxTimedCapture_stateChanged(int arg1)
{
    if (!m_rgbConnected) {
        ui->rgbCheckBoxTimedCapture->setChecked(false);
        return;
    }
    
    m_timedCaptureEnabled = (arg1 == Qt::Checked);
    
    if (m_timedCaptureEnabled) {
        // 保存配置
        m_configManager->setCaptureInterval(ui->spinBoxInterval->value());
        m_configManager->setCaptureQuality(ui->spinBoxQuality->value());
        m_configManager->saveConfig();
        
        // 开始定时抓图
        m_captureManager->startTimedCapture(ui->spinBoxInterval->value() * 1000, ui->spinBoxQuality->value(), 1920, 1080);
    } else {
        // 停止定时抓图
        m_captureManager->stopTimedCapture();
    }
}

void MainWindow::on_rgbBtnSelectSavePath_clicked()
{
    QString path = QFileDialog::getExistingDirectory(this, "选择保存路径", m_configManager->getSavePath());
    if (!path.isEmpty()) {
        ui->lineEditSavePath->setText(path);
        m_configManager->setSavePath(path);
        m_configManager->saveConfig();
        m_captureManager->setSavePath(path);
    }
}

void MainWindow::on_rgbBtnSelectVideoPath_clicked()
{
    QString path = QFileDialog::getExistingDirectory(this, "选择录像保存路径", ui->lineEditVideoSavePath->text());
    if (!path.isEmpty()) {
        ui->lineEditVideoSavePath->setText(path);
    }
}

void MainWindow::on_rgbBtnRecord_clicked()
{
    if (ui->rgbBtnRecord->isChecked()) {
        // 开始录像
        QString saveDir = ui->lineEditVideoSavePath->text();
        
        // 检查并创建目录
        QDir dir(saveDir);
        if (!dir.exists()) {
            if (!dir.mkpath(".")) {
                QMessageBox::warning(this, "错误", "无法创建保存目录！");
                ui->rgbBtnRecord->setChecked(false);
                return;
            }
        }
        
        // 生成文件名
        QDateTime currentTime = QDateTime::currentDateTime();
        QString fileName = QString("record_%1.mp4").arg(currentTime.toString("yyyyMMdd_hhmmss"));
        QString fullPath = dir.absoluteFilePath(fileName);
        
        // 开始录像
        if (m_deviceManager->startRecord(fullPath)) {
            // 录像开始成功，UI更新会在 onRecordStarted 中处理
        } else {
            QMessageBox::warning(this, "错误", "开始录像失败！");
            ui->rgbBtnRecord->setChecked(false);
        }
    } else {
        // 停止录像
        m_deviceManager->stopRecord();
        // 录像停止，UI更新会在 onRecordStopped 中处理
    }
}

void MainWindow::onRecordTimerTimeout()
{
    m_recordSeconds++;
    ui->rgbLabelRecordTime->setText(formatTime(m_recordSeconds));
}

void MainWindow::onRecordStarted()
{
    m_recordSeconds = 0;
    ui->rgbLabelRecordTime->setText("00:00:00");
    ui->rgbBtnRecord->setText("停止录像");
    ui->rgbLabelRecordStatus->setText("录像中...");
    ui->lineEditVideoSavePath->setEnabled(false);
    ui->rgbBtnSelectVideoPath->setEnabled(false);
    m_recordTimer->start();
    qDebug() << "[MainWindow] 录像已开始";
    
    // 启动 PTZ 自动查询 (10Hz)
    if (m_ptzController && m_ptzConnected) {
        emit ptzStartAutoQuery(100);
        qDebug() << "[MainWindow] PTZ 自动查询已启动 (10Hz)";
    }
}

void MainWindow::onRecordStopped(const QString &filePath)
{
    m_recordTimer->stop();
    m_recordSeconds = 0;
    ui->rgbLabelRecordTime->setText("00:00:00");
    ui->rgbBtnRecord->setChecked(false);
    ui->rgbBtnRecord->setText("开始录像");
    ui->rgbLabelRecordStatus->setText("就绪");
    ui->lineEditVideoSavePath->setEnabled(true);
    ui->rgbBtnSelectVideoPath->setEnabled(true);
    qDebug() << "[MainWindow] 录像已完成，文件:" << filePath;
    
    // 停止 PTZ 自动查询
    if (m_ptzController) {
        emit ptzStopAutoQuery();
        qDebug() << "[MainWindow] PTZ 自动查询已停止";
    }
    
    // 显示数据记录文件信息
    QString csvPath = QFileInfo(filePath).absolutePath() + "/" + QFileInfo(filePath).completeBaseName() + ".csv";
    if (QFile::exists(csvPath)) {
        ui->statusbar->showMessage(QString("录像已保存: %1，数据已保存: %2").arg(filePath, csvPath), 5000);
    } else {
        ui->statusbar->showMessage(QString("录像已保存: %1").arg(filePath), 5000);
    }
}

QString MainWindow::formatTime(int seconds)
{
    int hours = seconds / 3600;
    int minutes = (seconds % 3600) / 60;
    int secs = seconds % 60;
    return QString("%1:%2:%3")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(secs, 2, 10, QChar('0'));
}

void MainWindow::onConnectionStatusChanged(bool connected)
{
    m_rgbConnected = connected;
    updateRGBUIState();
    updateTrackingUIState();

    if (connected) {
        qInfo() << "[MainWindow] 设备连接成功，开始视频预览与网络云台会话";
        m_deviceManager->requestStartStream();

        // 自动初始化网络集成云台控制器
        if (!m_ptzController && m_deviceManager && m_deviceManager->getSdkSession()) {
            m_ptzThread = new QThread();
            m_ptzController = new NetPtzController(m_deviceManager->getSdkSession(), 0, nullptr);

            connect(m_ptzThread, &QThread::finished, m_ptzController, &QObject::deleteLater);
            connect(m_ptzThread, &QThread::started, m_ptzController, &IPtzController::init);

            connect(m_ptzController, &IPtzController::angleReceived, this, &MainWindow::handleAngleReceived);
            connect(this, &MainWindow::ptzMoveTo, m_ptzController, &IPtzController::moveTo);
            connect(this, &MainWindow::ptzStop, m_ptzController, &IPtzController::stop);
            connect(this, &MainWindow::ptzQueryAngle, m_ptzController, &IPtzController::queryAngle);
            connect(this, &MainWindow::ptzMoveDirection, m_ptzController, &IPtzController::moveDirection);
            connect(this, &MainWindow::ptzStartAutoQuery, m_ptzController, &IPtzController::startAutoQuery);
            connect(this, &MainWindow::ptzStopAutoQuery, m_ptzController, &IPtzController::stopAutoQuery);

            connect(m_ptzController, &IPtzController::angleReceived, m_deviceManager, &DeviceManager::onPTZAngleReceived);

            connect(m_ptzController, &IPtzController::rawDataInout, this, [this](bool isTx, QByteArray data){
                QString hexStr = data.toHex(' ').toUpper();
                if (isTx) {
                    ui->textLog->append(QString("<font color='#0055FF'><b>[TX]</b> %1</font>").arg(hexStr));
                } else {
                    ui->textLog->append(QString("<font color='#008000'><b>[RX]</b> %1</font>").arg(hexStr));
                }
            });

            m_ptzController->moveToThread(m_ptzThread);
            m_ptzThread->start();

            m_ptzConnected = true;
            updatePTZUIState();
            emit ptzStartAutoQuery(100);
            ui->textLog->append("[云台] 以太网一体化网络云台已就绪");
        }

        m_tempPollTimer->start(1000);
    } else {
        qInfo() << "[MainWindow] 设备断开连接，停止视频预览与云台会话";
        m_tempPollTimer->stop();
        // 断开时停止跟踪
        if (m_trackingController->isTracking()) {
            m_trackingController->stopTracking();
        }
        if (m_ptzController) {
            if (m_ptzThread) {
                QMetaObject::invokeMethod(m_ptzController, "prepareForDestruction", Qt::BlockingQueuedConnection);
                disconnect(m_ptzThread, &QThread::finished, m_ptzController, &QObject::deleteLater);
                m_ptzThread->quit();
                m_ptzThread->wait();
            }
            delete m_ptzController;
            m_ptzController = nullptr;
            delete m_ptzThread;
            m_ptzThread = nullptr;
            m_ptzConnected = false;
            updatePTZUIState();
        }
    }
}

void MainWindow::onCaptureFinished(const QImage &image, const QString &filePath)
{
    // 抓图图片不显示在预览区，只添加到历史记录
    // SDK的OSD会自动添加时间戳，直接保存原始图片
    
    // 添加到历史记录
    addToHistory(image, filePath);
    
    // 更新状态
    ui->statusbar->showMessage(QString("抓图成功: %1").arg(QFileInfo(filePath).fileName()), 3000);
}

void MainWindow::onErrorOccurred(const QString &error)
{
    // 显示错误信息
    ui->rgbLabelError->setText(QString("错误: %1").arg(error));
    ui->statusbar->showMessage(error, 5000);
    qDebug() << "错误:" << error;
}

void MainWindow::onFrameReceived(const QImage &frame)
{
    if (frame.isNull()) {
        return;
    }

    // 现场排障：确认解码帧是否真正到达 UI 线程
    static bool s_firstUiFrameLogged = false;
    if (!s_firstUiFrameLogged) {
        s_firstUiFrameLogged = true;
        qInfo() << "[MainWindow] 收到首帧图像，尺寸 =" << frame.width() << "x" << frame.height();
    }

    // 显示图片（SDK的OSD已自动添加时间戳）
    displayImage(frame);

    // 如果正在跟踪，传递帧给跟踪控制器
    if (m_trackingController->isTracking()) {
        m_trackingController->processFrame(frame);
    }
}

void MainWindow::onFocusModeChanged(bool isManualFocus)
{
    if (ui->checkBoxManualFocus) {
        // 使用 blockSignals 避免死循环
        const bool blocked = ui->checkBoxManualFocus->blockSignals(true);
        ui->checkBoxManualFocus->setChecked(isManualFocus);
        ui->checkBoxManualFocus->blockSignals(blocked);
        
        ui->textLog->append(QString("[镜头控制] 聚焦模式已切换为: %1")
                           .arg(isManualFocus ? "手动对焦" : "自动对焦"));
        
        // 【确保】变焦按钮始终启用
        if (ui->btnZoomIn) ui->btnZoomIn->setEnabled(true);
        if (ui->btnZoomOut) ui->btnZoomOut->setEnabled(true);
        
        // 【UI 联动】自动对焦模式下禁用手动对焦按钮
        if (ui->btnFocusNear) {
            ui->btnFocusNear->setEnabled(isManualFocus);
        }
        if (ui->btnFocusFar) {
            ui->btnFocusFar->setEnabled(isManualFocus);
        }
    }
}

void MainWindow::on_checkBoxManualFocus_clicked(bool checked)
{
    if (m_deviceManager && m_deviceManager->getLensManager()) {
        m_deviceManager->getLensManager()->setFocusMode(checked);
    }
}

void MainWindow::onIrisModeChanged(bool isManualIris)
{
    if (ui->checkBoxManualIris) {
        // 使用 blockSignals 避免死循环
        const bool blocked = ui->checkBoxManualIris->blockSignals(true);
        ui->checkBoxManualIris->setChecked(isManualIris);
        ui->checkBoxManualIris->blockSignals(blocked);
        
        ui->textLog->append(QString("[镜头控制] 光圈模式已切换为: %1")
                           .arg(isManualIris ? "手动/光圈优先" : "自动"));
        
        // 【UI 联动】自动光圈模式下禁用手动光圈按钮
        if (ui->btnIrisOpen) {
            ui->btnIrisOpen->setEnabled(isManualIris);
        }
        if (ui->btnIrisClose) {
            ui->btnIrisClose->setEnabled(isManualIris);
        }
    }
}

void MainWindow::on_checkBoxManualIris_clicked(bool checked)
{
    if (m_deviceManager && m_deviceManager->getLensManager()) {
        m_deviceManager->getLensManager()->setIrisMode(checked);
    }
}

// ========================
// 目标跟踪相关槽函数
// ========================

void MainWindow::on_btnTrackSelect_clicked()
{
    m_trackingController->startSelection();
    m_isSelectingTarget = true;
    ui->statusbar->showMessage("请在视频画面上按住鼠标左键框选目标", 5000);
}

void MainWindow::on_btnTrackStart_clicked()
{
    if (!m_isSelectingTarget || m_selectBox.isEmpty()) {
        ui->statusbar->showMessage("请先框选目标", 3000);
        return;
    }

    // 空帧防护
    if (m_lastFrame.isNull()) {
        ui->statusbar->showMessage("尚未收到视频帧", 3000);
        m_isSelectingTarget = false;
        m_selectBox = QRectF();
        return;
    }

    qDebug() << "[TrackStart] === begin ===";
    qDebug() << "[TrackStart] m_selectBox:" << m_selectBox;
    qDebug() << "[TrackStart] frame size:" << m_lastFrame.width() << "x" << m_lastFrame.height();

    // 从 UI 控件读取参数
    m_trackingController->setDeadZonePixels(ui->spinTrackDeadZone->value());
    m_trackingController->setMaxSpeed(ui->spinTrackMaxSpeed->value());
    m_trackingController->setProportionalGain(ui->spinTrackKp->value() / 100.0f);
    m_trackingController->setIntegralGain(ui->spinTrackKi->value() / 100.0f);
    m_trackingController->setDerivativeGain(ui->spinTrackKd->value() / 100.0f);
    m_trackingController->setFeedforwardGain(ui->spinTrackKff->value() / 100.0f);
    m_trackingController->setPredictHorizon(ui->spinTrackPredict->value());

    qDebug() << "[TrackStart] params set, will setTarget...";

    // 设置帧尺寸
    m_trackingController->setFrameSize(m_lastFrame.width(), m_lastFrame.height());

    // 启动跟踪（setTarget 内部会创建子线程并返回；此处加 try/catch 防止直接崩溃）
    try {
        m_trackingController->setTarget(m_lastFrame, m_selectBox);
    } catch (const std::exception &e) {
        qCritical() << "[TrackStart] setTarget 抛出异常:" << e.what();
        ui->statusbar->showMessage(QString("setTarget 异常: %1").arg(e.what()), 5000);
        return;
    } catch (...) {
        qCritical() << "[TrackStart] setTarget 抛出未知异常";
        ui->statusbar->showMessage("setTarget 异常（未知类型）", 5000);
        return;
    }

    qDebug() << "[TrackStart] setTarget 完成，state=" << m_trackingController->state();
    m_isSelectingTarget = false;
    m_selectBox = QRectF();

    if (m_deviceManager && m_deviceManager->getInfraredService()) {
        m_deviceManager->getInfraredService()->lockTrackingPalette(true);
    }
}

void MainWindow::on_btnTrackStop_clicked()
{
    m_trackingController->stopTracking();
    m_isSelectingTarget = false;
    m_selectBox = QRectF();
    m_trackBoxVisible = false;
    updateTrackingUIState();

    if (m_deviceManager && m_deviceManager->getInfraredService()) {
        m_deviceManager->getInfraredService()->lockTrackingPalette(false);
    }
}

void MainWindow::on_btnTrackResetParams_clicked()
{
    const int defaultDeadZone = 15;
    const int defaultMaxSpeed = 20;
    const int defaultPredict = 5;
    const float defaultKp = 0.15f;
    const float defaultKi = 0.01f;
    const float defaultKd = 0.03f;
    const float defaultKff = 0.50f;

    ui->spinTrackDeadZone->setValue(defaultDeadZone);
    ui->spinTrackMaxSpeed->setValue(defaultMaxSpeed);
    ui->spinTrackPredict->setValue(defaultPredict);
    ui->spinTrackKp->setValue(qRound(defaultKp * 100.0f));
    ui->spinTrackKi->setValue(qRound(defaultKi * 100.0f));
    ui->spinTrackKd->setValue(qRound(defaultKd * 100.0f));
    ui->spinTrackKff->setValue(qRound(defaultKff * 100.0f));

    m_trackingController->setDeadZonePixels(defaultDeadZone);
    m_trackingController->setMaxSpeed(defaultMaxSpeed);
    m_trackingController->setPredictHorizon(defaultPredict);
    m_trackingController->setProportionalGain(defaultKp);
    m_trackingController->setIntegralGain(defaultKi);
    m_trackingController->setDerivativeGain(defaultKd);
    m_trackingController->setFeedforwardGain(defaultKff);

    m_configManager->setTrackParams(defaultKp, defaultKi, defaultKd, defaultKff, defaultPredict, defaultDeadZone, defaultMaxSpeed);
    m_configManager->saveConfig();

    ui->statusbar->showMessage("目标跟踪控制参数已恢复默认值并已保存", 3000);
    ui->textLog->append("[系统] 目标跟踪参数已恢复默认值");
}

void MainWindow::onTrackStateChanged(TrackingController::TrackingState state)
{
    updateTrackingUIState();

    QString msg;
    switch (state) {
    case TrackingController::Idle: msg = "跟踪已停止"; break;
    case TrackingController::Selecting: msg = "请框选目标"; break;
    case TrackingController::Initializing: msg = "跟踪器初始化中"; break;
    case TrackingController::Tracking: msg = "跟踪运行中"; break;
    case TrackingController::Lost: msg = "目标丢失，尝试恢复"; break;
    case TrackingController::Paused: msg = "跟踪已暂停"; break;
    }
    ui->statusbar->showMessage(msg, 3000);
}

void MainWindow::onTrackResult(const TrackResult& result)
{
    // 更新数据记录器
    if (m_deviceManager && m_deviceManager->getDataRecorder() &&
        m_deviceManager->getDataRecorder()->isRecording()) {
        float offsetX = result.bbox.center().x() - m_lastFrame.width() / 2.0f;
        float offsetY = result.bbox.center().y() - m_lastFrame.height() / 2.0f;
        m_deviceManager->getDataRecorder()->updateDetectionData(
            result.bbox.center().x(), result.bbox.center().y(),
            offsetX, offsetY, result.confidence);
    }
}

void MainWindow::onTrackBoxDraw(const QRectF& bbox, bool occluded)
{
    m_trackBox = bbox;
    m_trackBoxVisible = !bbox.isEmpty();
    m_trackBoxOccluded = occluded;

    // C8：直接复用缓存的缩放 pixmap 叠画跟踪框，省去 fromImage+scaled 全量重算
    if (!m_scaledPixmap.isNull()) {
        ui->rgbLabelImage->setPixmap(drawTrackBoxOnPixmap(m_scaledPixmap));
    }
}

void MainWindow::onTrackStatusMessage(const QString& msg)
{
    ui->statusbar->showMessage(msg, 3000);
    ui->textLog->append(QString("[跟踪] %1").arg(msg));
}

void MainWindow::onPtzControlDelta(int /*deltaX*/, int /*deltaY*/, int speedX, int speedY)
{
    if (!m_ptzConnected || !m_ptzController) {
        return;
    }

    // 速度输出转PTZ方向指令（负反馈闭环）
    // deltaX > 0: 目标在右侧，云台右转 (cmd2 = 0x02)
    // deltaX < 0: 目标在左侧，云台左转 (cmd2 = 0x04)
    // deltaY > 0: 目标在下方，云台下转 (cmd2 = 0x10)
    // deltaY < 0: 目标在上方，云台上转 (cmd2 = 0x08)

    uint8_t cmd2 = 0x00;
    uint8_t hSpeed = 0;
    uint8_t vSpeed = 0;

    // R-03: 根据速度输出的符号（而非脱靶量）决定方向
    if (speedX > 0) {
        cmd2 |= 0x02;  // 右
        hSpeed = static_cast<uint8_t>(qBound(0, speedX, 63));
    } else if (speedX < 0) {
        cmd2 |= 0x04;  // 左
        hSpeed = static_cast<uint8_t>(qBound(0, -speedX, 63));
    }

    if (speedY > 0) {
        cmd2 |= 0x10;  // 下
        vSpeed = static_cast<uint8_t>(qBound(0, speedY, 63));
    } else if (speedY < 0) {
        cmd2 |= 0x08;  // 上
        vSpeed = static_cast<uint8_t>(qBound(0, -speedY, 63));
    }

    if (cmd2 != 0x00) {
        emit ptzMoveDirection(cmd2, hSpeed, vSpeed);
    } else {
        emit ptzStop();
    }
}

// ========================
// 鼠标事件（框选目标）
// ========================

void MainWindow::mousePressEvent(QMouseEvent *event)
{
    if (!m_isSelectingTarget) {
        QMainWindow::mousePressEvent(event);
        return;
    }

    // 检查点击是否在图像标签内
    QPoint labelPos = ui->rgbLabelImage->mapFrom(this, event->pos());
    QRect labelRect = ui->rgbLabelImage->rect();
    if (!labelRect.contains(labelPos)) {
        QMainWindow::mousePressEvent(event);
        return;
    }

    m_selectStart = labelPos;
    m_selectBox = QRectF();
}

void MainWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_isSelectingTarget) {
        QMainWindow::mouseMoveEvent(event);
        return;
    }

    QPoint labelPos = ui->rgbLabelImage->mapFrom(this, event->pos());
    QRect labelRect = ui->rgbLabelImage->rect();
    if (!labelRect.contains(labelPos)) {
        QMainWindow::mouseMoveEvent(event);
        return;
    }

    // 计算框选区域（在标签坐标系）
    int x = std::min(m_selectStart.x(), labelPos.x());
    int y = std::min(m_selectStart.y(), labelPos.y());
    int w = std::abs(labelPos.x() - m_selectStart.x());
    int h = std::abs(labelPos.y() - m_selectStart.y());

    if (w < 5 || h < 5) return;

    // 转换到原始图像坐标（考虑 KeepAspectRatio 留白）
    if (!m_lastFrame.isNull()) {
        QSize labelSize = ui->rgbLabelImage->size();
        float frameW = static_cast<float>(m_lastFrame.width());
        float frameH = static_cast<float>(m_lastFrame.height());

        // 计算实际显示的图像在 label 中的区域（KeepAspectRatio 下可能有黑边）
        float displayScale = std::min(labelSize.width() / frameW, labelSize.height() / frameH);
        float displayW = frameW * displayScale;
        float displayH = frameH * displayScale;
        float offsetX = (labelSize.width() - displayW) / 2.0f;
        float offsetY = (labelSize.height() - displayH) / 2.0f;

        // 标签坐标 → 原始图像坐标
        float imgX = (x - offsetX) * frameW / displayW;
        float imgY = (y - offsetY) * frameH / displayH;
        float imgW = w * frameW / displayW;
        float imgH = h * frameH / displayH;

        m_selectBox = QRectF(imgX, imgY, imgW, imgH);

        // 实时显示框选（在缩放的画布上绘制，坐标需偏移）
        // R-09: 复用已缓存的 m_scaledPixmap，避免每次拖动都缩放 1080P 图像
        QPixmap overlay = m_scaledPixmap.copy();
        QPainter painter(&overlay);
        painter.setPen(QPen(Qt::yellow, 2, Qt::DashLine));
        painter.drawRect(QRectF(x - offsetX, y - offsetY, w, h));
        painter.end();
        ui->rgbLabelImage->setPixmap(overlay);
    }
}

void MainWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_isSelectingTarget) {
        QMainWindow::mouseReleaseEvent(event);
        return;
    }

    if (m_selectBox.width() < 10 || m_selectBox.height() < 10) {
        ui->statusbar->showMessage("框选区域太小，请重新框选", 3000);
        m_selectBox = QRectF();
        return;
    }

    ui->statusbar->showMessage(QString("已框选目标: %1x%2, 点击[开始跟踪]启动")
                               .arg(static_cast<int>(m_selectBox.width()))
                               .arg(static_cast<int>(m_selectBox.height())), 3000);
    updateTrackingUIState();
}

void MainWindow::on_btnFFC_clicked()
{
    if (!m_deviceManager || !m_deviceManager->isConnected()) {
        QMessageBox::warning(this, "提示", "请先连接设备！");
        return;
    }
    ui->statusbar->showMessage("正在执行快门校准...", 2000);
    ui->textLog->append("[系统] 触发快门校准");
    m_deviceManager->getInfraredService()->triggerShutterFFC();
}

void MainWindow::on_btnManualFFC_clicked()
{
    if (!m_deviceManager || !m_deviceManager->isConnected()) {
        QMessageBox::warning(this, "提示", "请先连接设备！");
        return;
    }
    ui->statusbar->showMessage("正在执行背景校准...", 2000);
    ui->textLog->append("[系统] 触发背景校准");
    m_deviceManager->getInfraredService()->triggerBackgroundFFC();
}

void MainWindow::on_comboPalette_currentIndexChanged(int index)
{
    if (!m_deviceManager || !m_deviceManager->isConnected()) return;
    int paletteId = index + 1;
    m_deviceManager->getInfraredService()->setPalette(paletteId);
    ui->textLog->append(QString("[系统] 伪彩色已切换为: %1").arg(paletteId));
}

void MainWindow::onGlobalTempUpdated(float maxTemp, float minTemp)
{
    m_globalMaxTemp = maxTemp;
    m_globalMinTemp = minTemp;
}

void MainWindow::onTempPollTimeout()
{
    if (m_deviceManager && m_deviceManager->isConnected()) {
        float maxT = 0.0f, minT = 0.0f;
        m_deviceManager->getInfraredService()->queryGlobalMaxMinTemp(maxT, minT);
    }
}

