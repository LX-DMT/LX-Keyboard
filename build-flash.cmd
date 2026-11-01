@echo off
setlocal
cd /d "%~dp0"
if "%~1"=="" goto usage
if "%~2"=="" goto usage
python scripts\build.py %1 --flash --port %2 %3 %4 %5 %6 %7 %8 %9
exit /b %errorlevel%
:usage
echo Run in the ESP-IDF 5.5.6 terminal:
echo   build-flash.cmd keyboard DOWNLOAD_PORT
echo   build-flash.cmd knob DOWNLOAD_PORT
echo   build-flash.cmd receiver DOWNLOAD_PORT [--control-port RESET_PORT]
exit /b 2
