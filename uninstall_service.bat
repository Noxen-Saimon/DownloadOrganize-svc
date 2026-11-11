@echo off
setlocal

:: Request Administrator elevation if not elevated
net session >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [INFO] Requesting Administrator privileges...
    powershell -Command "Start-Process cmd -ArgumentList '/c \"\"%~f0\"\"' -Verb RunAs"
    exit /b
)

echo ===========================================
echo       Uninstall DownloadOrganizerSvc
echo ===========================================

echo.
echo [1/3] Menghentikan service...
sc.exe stop DownloadOrganizerSvc >nul 2>&1
timeout /t 2 /nobreak >nul

echo [2/3] Menghapus service dari sistem Windows...
sc.exe delete DownloadOrganizerSvc
sc.exe delete MinimalService >nul 2>&1

echo [3/3] Membersihkan registry parameter...
reg.exe delete "HKLM\SYSTEM\CurrentControlSet\Services\DownloadOrganizerSvc" /f >nul 2>&1

echo.
echo ===========================================
echo Service BERHASIL dihapus dari sistem!
echo ===========================================
pause
