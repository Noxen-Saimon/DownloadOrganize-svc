@echo off
setlocal

:: Request Administrator elevation if not elevated
net session >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [INFO] Requesting Administrator privileges...
    powershell -Command "Start-Process cmd -ArgumentList '/c \"\"%~f0\"\"' -Verb RunAs"
    exit /b
)

echo ==============================================
echo  Updating and Restarting DownloadOrganizerSvc
echo ==============================================

cd /d "%~dp0"

echo.
echo [1/4] Stopping active service...
sc.exe stop DownloadOrganizerSvc >nul 2>&1
timeout /t 2 /nobreak >nul

echo [2/4] Building updated service binary...
call build.bat
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to compile service.exe.
    pause
    exit /b 1
)

echo [3/4] Updating registry parameters...
reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\DownloadOrganizerSvc\Parameters" /v DownloadsPath /t REG_SZ /d "C:\Users\Administrator\Downloads" /f
reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\DownloadOrganizerSvc\Parameters" /v SettleDelaySeconds /t REG_DWORD /d 5 /f

echo [4/4] Starting service...
sc.exe config DownloadOrganizerSvc binPath= "%~dp0service.exe" >nul 2>&1
sc.exe start DownloadOrganizerSvc

echo.
echo [SUCCESS] Service restarted successfully!
echo Target folder: C:\Users\Administrator\Downloads (Wait buffer: 5s)
pause
