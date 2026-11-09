# IRPTZ_demo — 红外集成云台目标跟踪上位机

基于红外热像机芯与云台一体化设备的上位机软件，支持 RTSP 视频拉流解码显示、录像、目标自动跟踪与云台闭环伺服控制。

## 技术栈

| 组件 | 版本/说明 |
|---|---|
| Qt | 6.12.0（MinGW 64-bit，C++17） |
| FFmpeg | 8.1（RTSP 拉流、H.264 解码、录像） |
| OpenCV | 4.10（DSST 跟踪特征提取与矩阵运算） |
| SV_NET_SDK | 红外机芯/云台厂商 SDK（设备控制、测温、镜头控制） |

## 主要功能

- **视频链路**：RTSP 拉流 → FFmpeg 多线程解码 → Qt 界面渲染，支持录像（`RecordThread`）
- **目标跟踪**：DSST 相关滤波 + 卡尔曼滤波，输出像素脱靶量
- **云台伺服**：脱靶量经 PID 前馈后通过网络 SDK 下发 PTZ 控制指令，构成跟踪闭环；带 500ms 失联看门狗与指令去重合并
- **设备管理**：设备搜索（SADP）、连接会话管理（`SvSdkSession`）、参数配置（`ConfigManager`）、日志（`LogManager`）

## 模块结构

```
mainwindow.*          主界面与各模块装配
rtspsource.*          RTSP 拉流源
DecodeThread.*        视频解码线程
RecordThread.*        录像线程
videodecoder.*        FFmpeg 解码封装
capturemanager.*      采集管理
imageprocessor.*      图像处理
imagematconvert.*     QImage/Mat 转换
objecttracker.*       DSST 跟踪器
dsstcore.*            DSST 核心算法
trackingcontroller.*  跟踪控制（状态机、模式切换）
netptzcontroller.*    网络 PTZ 控制器（IPtzController 实现）
lensmanager.*         镜头（变焦/调焦）控制
infraredservice.*     红外机芯服务（测温等）
svsdksession.*        SV_NET_SDK 会话封装
devicemanager.*       设备管理
configmanager.*       配置管理
datarecorder.*        数据记录
logmanager.*          日志管理
ThreadSafeQueue.h     线程安全队列
```

## 构建与运行

### 一键构建

```bat
build_project.bat
```

脚本调用 Qt 6.12 (MinGW) 的 qmake + mingw32-make，完成后自动部署 Qt 运行库并输出到 `release\` 目录。

### 手动构建

用 Qt Creator 打开 [IRPTZ_demo.pro](IRPTZ_demo.pro)，选择 Desktop Qt 6.12.0 MinGW 64-bit 套件构建；`.pro` 中配置了构建后自动拷贝 FFmpeg/OpenCV/SDK 的运行时 DLL。

### 运行

- 双击 `release\IRPTZ_demo.exe`（独立运行目录），或
- 双击根目录 `双击启动红外集成云台上位机.bat`

## 第三方依赖说明

以下目录**不在本仓库内**，编译前需自行放置到项目根目录：

| 目录 | 内容 |
|---|---|
| `sdk/` | SV_NET_SDK 开发包（编译依赖 `sdk/include` 与 `sdk/lib/x64`） |
| `ffmpeg-8.1-full_build-shared/` | FFmpeg 8.1 开发包（include/lib/bin） |
| `opencv/` | OpenCV 4.10 MinGW 编译版（`opencv/install`） |

根目录的 19 个运行必需 DLL 已随仓库提供，新环境克隆后配齐上述依赖即可编译运行。

## 文档

详细设计文档见 [docs/](docs/)：

1. [重构与移植主方案](docs/01_红外集成云台上位机重构与移植主方案.md)
2. [三种软件架构对比与选型指南](docs/02_三种软件架构对比与选型指南.md)
3. [接口映射与代码移植实战手册](docs/03_接口映射与代码移植实战手册.md)
4. [使用与硬件联调指南](docs/04_红外集成云台上位机使用与硬件联调指南.md)
