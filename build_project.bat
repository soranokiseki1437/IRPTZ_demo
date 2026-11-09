@echo off
chcp 65001 >nul
title 红外集成云台上位机 - 一键编译与打包脚本

set QT_DIR=C:\Qt\6.12.0\mingw_64
set MINGW_DIR=C:\Qt\Tools\mingw1310_64
set PATH=%QT_DIR%\bin;%MINGW_DIR%\bin;%PATH%

set SCRIPT_DIR=%~dp0
set BUILD_DIR=%SCRIPT_DIR%build\Desktop_Qt_6_12_0_MinGW_64_bit_Release
set PRO_FILE=%SCRIPT_DIR%IRPTZ_demo.pro
set RELEASE_DIR=%SCRIPT_DIR%release

echo ====================================================
echo   红外集成云台目标跟踪上位机 - 一键构建
echo ====================================================
echo [1/4] 创建构建目录: %BUILD_DIR%
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"

echo [2/4] 执行 qmake...
"%QT_DIR%\bin\qmake.exe" "%PRO_FILE%" -spec win32-g++ "CONFIG+=qtquickcompiler"
if errorlevel 1 (
    echo [错误] qmake 执行失败！
    pause
    exit /b 1
)

echo [3/4] 执行 mingw32-make 编译与链接...
"%MINGW_DIR%\bin\mingw32-make.exe" -j4
if errorlevel 1 (
    echo [错误] mingw32-make 编译失败！
    pause
    exit /b 1
)

echo [4/4] 部署 Qt 运行环境与独立运行包...
"%QT_DIR%\bin\windeployqt.exe" --release --no-translations "%BUILD_DIR%\release\IRPTZ_demo.exe"

if not exist "%RELEASE_DIR%" mkdir "%RELEASE_DIR%"
copy /Y "%BUILD_DIR%\release\IRPTZ_demo.exe" "%RELEASE_DIR%\" >nul
copy /Y "%BUILD_DIR%\release\*.dll" "%RELEASE_DIR%\" >nul
xcopy /E /I /Y "%BUILD_DIR%\release\generic" "%RELEASE_DIR%\generic" >nul 2>&1
xcopy /E /I /Y "%BUILD_DIR%\release\iconengines" "%RELEASE_DIR%\iconengines" >nul 2>&1
xcopy /E /I /Y "%BUILD_DIR%\release\imageformats" "%RELEASE_DIR%\imageformats" >nul 2>&1
xcopy /E /I /Y "%BUILD_DIR%\release\networkinformation" "%RELEASE_DIR%\networkinformation" >nul 2>&1
xcopy /E /I /Y "%BUILD_DIR%\release\platforms" "%RELEASE_DIR%\platforms" >nul 2>&1
xcopy /E /I /Y "%BUILD_DIR%\release\styles" "%RELEASE_DIR%\styles" >nul 2>&1
xcopy /E /I /Y "%BUILD_DIR%\release\tls" "%RELEASE_DIR%\tls" >nul 2>&1

echo ====================================================
echo   构建完成！可执行文件已就绪：
echo   - 绿色独立运行目录: %RELEASE_DIR%\IRPTZ_demo.exe
echo   - 或直接双击根目录: 双击启动红外集成云台上位机.bat
echo ====================================================
pause

