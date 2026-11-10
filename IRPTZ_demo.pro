QT       += core gui concurrent

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17
TARGET = IRPTZ_demo

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    netptzcontroller.cpp \
    svsdksession.cpp \
    rtspsource.cpp \
    infraredservice.cpp \
    devicemanager.cpp \
    capturemanager.cpp \
    imageprocessor.cpp \
    configmanager.cpp \
    videodecoder.cpp \
    DecodeThread.cpp \
    RecordThread.cpp \
    lensmanager.cpp \
    datarecorder.cpp \
    objecttracker.cpp \
    trackingcontroller.cpp \
    dsstcore.cpp \
    logmanager.cpp \
    logging_categories.cpp \
    imagematconvert.cpp \
    taskboardwindow.cpp

HEADERS += \
    mainwindow.h \
    svsdk.h \
    iptzcontroller.h \
    netptzcontroller.h \
    svsdksession.h \
    rtspsource.h \
    infraredservice.h \
    devicemanager.h \
    capturemanager.h \
    imageprocessor.h \
    configmanager.h \
    videodecoder.h \
    DecodeThread.h \
    ThreadSafeQueue.h \
    RecordThread.h \
    lensmanager.h \
    datatypes.h \
    datarecorder.h \
    objecttracker.h \
    trackingcontroller.h \
    dsstcore.h \
    logmanager.h \
    logging_categories.h \
    imagematconvert.h \
    taskboardwindow.h

FORMS += \
    mainwindow.ui

# 红外一体化网络 SDK
INCLUDEPATH += $$PWD
INCLUDEPATH += $$PWD/sdk/include
LIBS += -L$$PWD/sdk/lib/x64 -lSV_NET_SDK -lws2_32

# FFmpeg 8.1 依赖（解码与拉流）
FFMPEG_DIR = $$PWD/ffmpeg-8.1-full_build-shared
INCLUDEPATH += $$FFMPEG_DIR/include
LIBS += -L$$FFMPEG_DIR/lib \
        -lavcodec \
        -lavformat \
        -lavutil \
        -lswscale \
        -lswresample

# OpenCV 4.10 依赖（DSST 特征提取与矩阵运算）
OPENCV_DIR = $$PWD/opencv/install
INCLUDEPATH += $$OPENCV_DIR/include

win32: LIBS += -L$$OPENCV_DIR/x64/mingw/lib \
    -lopencv_core4100 \
    -lopencv_imgproc4100 \
    -lopencv_imgcodecs4100
unix: LIBS += -L$$OPENCV_DIR/lib -lopencv_core -lopencv_imgproc

DEFINES += _CRT_SECURE_NO_WARNINGS
DEFINES += PROJECT_VERSION=\\\"2.0.0\\\"
DEFINES += PROJECT_NAME=\\\"IR_PTZ_Integrated\\\"

CONFIG(debug, debug|release) {
    DEFINES += QT_MESSAGELOGCONTEXT
}

# 构建后自动复制所有运行时 DLL
win32 {
    CONFIG(debug, debug|release) {
        DESTDIR_WIN = $$shell_path($$OUT_PWD/debug)
    } else {
        DESTDIR_WIN = $$shell_path($$OUT_PWD/release)
    }

    # FFmpeg 8.1 DLLs
    FFMPEG_BIN = $$shell_path($$FFMPEG_DIR/bin)
    QMAKE_POST_LINK += copy /Y $$quote($$FFMPEG_BIN\\avcodec-62.dll)   $$quote($$DESTDIR_WIN) &
    QMAKE_POST_LINK += copy /Y $$quote($$FFMPEG_BIN\\avformat-62.dll)  $$quote($$DESTDIR_WIN) &
    QMAKE_POST_LINK += copy /Y $$quote($$FFMPEG_BIN\\avutil-60.dll)    $$quote($$DESTDIR_WIN) &
    QMAKE_POST_LINK += copy /Y $$quote($$FFMPEG_BIN\\swscale-9.dll)    $$quote($$DESTDIR_WIN) &
    QMAKE_POST_LINK += copy /Y $$quote($$FFMPEG_BIN\\swresample-6.dll) $$quote($$DESTDIR_WIN) &

    # OpenCV DLLs
    OPENCV_BIN = $$shell_path($$OPENCV_DIR/x64/mingw/bin)
    QMAKE_POST_LINK += copy /Y $$quote($$OPENCV_BIN\\libopencv_core4100.dll)     $$quote($$DESTDIR_WIN) &
    QMAKE_POST_LINK += copy /Y $$quote($$OPENCV_BIN\\libopencv_imgproc4100.dll)  $$quote($$DESTDIR_WIN) &
    QMAKE_POST_LINK += copy /Y $$quote($$OPENCV_BIN\\libopencv_imgcodecs4100.dll) $$quote($$DESTDIR_WIN) &

    # 红外相机及集成云台 SDK DLLs
    IR_SDK_BIN = $$shell_path($$PWD/sdk/lib/x64)
    QMAKE_POST_LINK += copy /Y $$quote($$IR_SDK_BIN\\SV_NET_SDK.dll)      $$quote($$DESTDIR_WIN) &
    QMAKE_POST_LINK += copy /Y $$quote($$IR_SDK_BIN\\SV_ISDP.dll)         $$quote($$DESTDIR_WIN) &
    QMAKE_POST_LINK += copy /Y $$quote($$IR_SDK_BIN\\PlaySRenderCtrl.dll) $$quote($$DESTDIR_WIN) &
    QMAKE_POST_LINK += copy /Y $$quote($$IR_SDK_BIN\\pthreadVC2.dll)      $$quote($$DESTDIR_WIN) &
    QMAKE_POST_LINK += copy /Y $$quote($$IR_SDK_BIN\\msvcr120.dll)        $$quote($$DESTDIR_WIN) &
    QMAKE_POST_LINK += copy /Y $$quote($$IR_SDK_BIN\\msvcp120.dll)        $$quote($$DESTDIR_WIN) &
    QMAKE_POST_LINK += copy /Y $$quote($$IR_SDK_BIN\\SDL2.dll)            $$quote($$DESTDIR_WIN) &
    QMAKE_POST_LINK += copy /Y $$quote($$IR_SDK_BIN\\avcodec-58.dll)      $$quote($$DESTDIR_WIN) &
    QMAKE_POST_LINK += copy /Y $$quote($$IR_SDK_BIN\\avformat-58.dll)     $$quote($$DESTDIR_WIN) &
    QMAKE_POST_LINK += copy /Y $$quote($$IR_SDK_BIN\\avutil-56.dll)       $$quote($$DESTDIR_WIN) &
    QMAKE_POST_LINK += copy /Y $$quote($$IR_SDK_BIN\\swscale-5.dll)       $$quote($$DESTDIR_WIN) &
    QMAKE_POST_LINK += copy /Y $$quote($$IR_SDK_BIN\\swresample-3.dll)    $$quote($$DESTDIR_WIN)
}

