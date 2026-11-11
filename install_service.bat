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
echo    Instalasi Download Folder Organizer Service
echo ==============================================

cd /d "%~dp0"

echo.
echo [1/3] Mendaftarkan/memperbarui service ke sistem Windows...
sc.exe query DownloadOrganizerSvc >nul 2>&1
if %ERRORLEVEL% equ 0 (
    sc.exe config DownloadOrganizerSvc binPath= "%~dp0service.exe" start= auto DisplayName= "Download Folder Organizer Service"
) else (
    sc.exe create DownloadOrganizerSvc binPath= "%~dp0service.exe" start= auto DisplayName= "Download Folder Organizer Service"
)

echo.
echo [2/3] Mengatur target folder ke C:\Users\Administrator\Downloads...
reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\DownloadOrganizerSvc\Parameters" /v DownloadsPath /t REG_SZ /d "C:\Users\Administrator\Downloads" /f
reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\DownloadOrganizerSvc\Parameters" /v SettleDelaySeconds /t REG_DWORD /d 5 /f

echo.
echo [3/3] Menjalankan service...
sc.exe start DownloadOrganizerSvc

echo.
echo =========================================================
echo SUKSES! Service sudah aktif dan sedang memantau folder:
echo C:\Users\Administrator\Downloads (Wait buffer: 5 detik)
echo =========================================================
pause
