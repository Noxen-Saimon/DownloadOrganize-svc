@echo off
setlocal enabledelayedexpansion

:: Check if cl is available, if not find and call vcvars64.bat
where cl >nul 2>nul
if %ERRORLEVEL% neq 0 (
    set "VS_VCVARS="
    if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
        set "VS_VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
        set "VS_VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
        set "VS_VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
    )

    if defined VS_VCVARS (
        call "!VS_VCVARS!" >nul
    ) else (
        echo [ERROR] MSVC build tools not found.
        exit /b 1
    )
)

cl /nologo /O1 /GS- /MT service.c /link /OPT:REF /OPT:ICF /ENTRY:mainCRTStartup /SUBSYSTEM:CONSOLE kernel32.lib advapi32.lib /OUT:service.exe

if %ERRORLEVEL% equ 0 (
    echo [OK] Built service.exe successfully.
) else (
    echo [ERROR] Build failed.
    exit /b 1
)
