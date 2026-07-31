@echo off
REM build_daemon.bat — собирает NadbDaemon.java в DEX-jar, пригодный
REM для запуска на устройстве через `app_process`.
REM
REM Требуется: JDK на PATH (javac/jar), Android SDK с установленными
REM  - SDK Tools -> Android SDK Build-Tools (даёт d8)
REM  - SDK Platforms -> любая свежая android-XX (даёт android.jar,
REM    ТОЛЬКО для компиляции — на устройство не пакуется и не нужен рут)
REM
REM Использование:  build_daemon.bat
REM Результат:       nadb_daemon.jar в этой же папке

setlocal enabledelayedexpansion

set ANDROID_HOME=C:\Users\shara\AppData\Local\Android\Sdk

if not exist "%ANDROID_HOME%" (
    echo [ERROR] ANDROID_HOME не найден: %ANDROID_HOME%
    exit /b 1
)

REM ── найти последнюю установленную версию build-tools (даёт d8.bat) ──
set BUILD_TOOLS_VER=
for /f "delims=" %%v in ('dir /b /ad "%ANDROID_HOME%\build-tools" 2^>nul ^| sort') do set BUILD_TOOLS_VER=%%v
if "%BUILD_TOOLS_VER%"=="" (
    echo [ERROR] Не найдено ни одной версии в %ANDROID_HOME%\build-tools
    echo         Установите Build-Tools через Android Studio: File -^> Settings -^>
    echo         Languages ^& Frameworks -^> Android SDK -^> SDK Tools
    exit /b 1
)
set D8=%ANDROID_HOME%\build-tools\%BUILD_TOOLS_VER%\d8.bat

REM ── найти последнюю установленную platform (даёт android.jar) ──
set PLATFORM_VER=
for /f "delims=" %%p in ('dir /b /ad "%ANDROID_HOME%\platforms" 2^>nul ^| sort') do set PLATFORM_VER=%%p
if "%PLATFORM_VER%"=="" (
    echo [ERROR] Не найдено ни одной платформы в %ANDROID_HOME%\platforms
    echo         Установите любую свежую платформу через Android Studio: File -^> Settings -^>
    echo         Languages ^& Frameworks -^> Android SDK -^> SDK Platforms
    exit /b 1
)
set ANDROID_JAR=%ANDROID_HOME%\platforms\%PLATFORM_VER%\android.jar

echo Build-tools: %BUILD_TOOLS_VER%
echo Platform:    %PLATFORM_VER%
echo.

if exist build rmdir /s /q build
mkdir build

echo [1/3] javac...
javac -encoding UTF-8 --release 8 -cp "%ANDROID_JAR%" -d build NadbDaemon.java
if errorlevel 1 (
    echo [ERROR] Компиляция не удалась.
    exit /b 1
)

echo [2/3] d8 (class -^> dex)...
call "%D8%" --output build build\NadbDaemon.class
if errorlevel 1 (
    echo [ERROR] d8 не удался.
    exit /b 1
)

echo [3/3] упаковка в jar...
if exist nadb_daemon.jar del nadb_daemon.jar
pushd build
jar cf ..\nadb_daemon.jar classes.dex
popd
if errorlevel 1 (
    echo [ERROR] Упаковка в jar не удалась.
    exit /b 1
)

echo.
echo Готово: nadb_daemon.jar
echo.
echo Дальше:
echo   adb push nadb_daemon.jar /data/local/tmp/nadb_daemon.jar
echo   adb shell app_process -cp /data/local/tmp/nadb_daemon.jar /data/local/tmp NadbDaemon
echo.
echo Проверка с хоста в отдельном окне:
echo   adb forward tcp:9999 localabstract:nadb_daemon
echo   (дальше любым telnet/nc на 127.0.0.1:9999, отправить "ping", ждать "OK pong")
