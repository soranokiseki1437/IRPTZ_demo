@echo off
chcp 65001 >nul
title 红外集成云台目标跟踪系统 (IRPTZ_demo)

set SCRIPT_DIR=%~dp0
set APP_DIR=%SCRIPT_DIR%release
set EXE_PATH=%APP_DIR%\IRPTZ_demo.exe

if not exist "%EXE_PATH%" (
    set EXE_PATH=%APP_DIR%\IR_PTZ_Integrated.exe
)

if not exist "%EXE_PATH%" (
    set APP_DIR=%SCRIPT_DIR%build\Desktop_Qt_6_12_0_MinGW_64_bit_Release\release
    set EXE_PATH=%APP_DIR%\IRPTZ_demo.exe
)

if not exist "%EXE_PATH%" (
    set EXE_PATH=%APP_DIR%\IR_PTZ_Integrated.exe
)

if not exist "%EXE_PATH%" (
    echo [错误] 未找到上位机可执行文件：%EXE_PATH%
    echo 请使用 Qt Creator 打开 IRPTZ_demo.pro 完成编译。
    pause
    exit /b 1
)

echo ====================================================
echo   红外集成云台目标跟踪上位机系统 (全以太网化 DSST+KF+PID)
echo ====================================================
echo 正在启动上位机程序...
cd /d "%APP_DIR%"
start "" "%EXE_PATH%"
