#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#define SERVICE_NAME        L"DownloadOrganizerSvc"
#define SERVICE_DISPLAY_NAME L"Download Folder Organizer Service"
#define PARAM_REG_KEY       L"SYSTEM\\CurrentControlSet\\Services\\DownloadOrganizerSvc\\Parameters"
#define DEFAULT_PATH        L"C:\\Users\\Administrator\\Downloads"
#define DEFAULT_SETTLE_SECONDS 5

// Forward declaration
static void ScheduleDebouncedScan(DWORD delaySeconds);
static const WCHAR *GetFileExt(const WCHAR *filename);

static DWORD g_SettleDelaySeconds = DEFAULT_SETTLE_SECONDS;

// Event indices for WaitForMultipleObjects
enum WaitEvents {
    EVENT_STOP = 0,
    EVENT_DIR_CHANGE,
    EVENT_TIMER,
    EVENT_COUNT
};

// Global Service State
static SERVICE_STATUS        g_ServiceStatus;
static SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
static HANDLE                g_StopEvent    = NULL;
static HANDLE                g_TimerEvent   = NULL;
static HANDLE                g_WatchDir     = INVALID_HANDLE_VALUE;
static OVERLAPPED            g_Overlapped;
static BYTE                  g_ChangeBuf[4096];
static WCHAR                 g_WatchPath[MAX_PATH];

// Category definition
typedef struct {
    const WCHAR *CategoryName;
    const WCHAR *Extensions; // Semicolon-delimited, lowercase (e.g. L"jpg;png;gif")
} CATEGORY_RULE;

static const CATEGORY_RULE g_Categories[] = {
    { L"Images",     L"jpg;jpeg;png;gif;bmp;webp;svg;ico;tiff;tif;raw;heic" },
    { L"Documents",  L"pdf;doc;docx;xls;xlsx;ppt;pptx;txt;csv;rtf;odt;epub;md" },
    { L"Archives",   L"zip;rar;7z;tar;gz;bz2;xz;iso;cab;tgz" },
    { L"Installers", L"exe;msi;pkg;deb;rpm;appx;msix" },
    { L"Audio",      L"mp3;wav;flac;aac;ogg;m4a;wma;opus" },
    { L"Video",      L"mp4;mkv;avi;mov;wmv;flv;webm;m4v" }
};

#define CATEGORY_COUNT (sizeof(g_Categories) / sizeof(g_Categories[0]))
#define CATEGORY_OTHER L"Other"
#define CATEGORY_KAMPUS L"Kampus"

// String & Path Utility Functions (Pure Win32, No CRT)

static DWORD StrLenW(const WCHAR *s) {
    DWORD len = 0;
    while (s && s[len]) len++;
    return len;
}

static WCHAR ToLowerW(WCHAR c) {
    if (c >= L'A' && c <= L'Z') return c + (L'a' - L'A');
    return c;
}

static int StrEqualNoCaseW(const WCHAR *s1, const WCHAR *s2) {
    while (*s1 && *s2) {
        if (ToLowerW(*s1) != ToLowerW(*s2)) return 0;
        s1++;
        s2++;
    }
    return (*s1 == *s2);
}

static BOOL StrContainsNoCaseW(const WCHAR *str, const WCHAR *substr) {
    if (!str || !substr || !*substr) return FALSE;
    DWORD subLen = StrLenW(substr);
    while (*str) {
        DWORD i = 0;
        while (str[i] && substr[i] && ToLowerW(str[i]) == ToLowerW(substr[i])) {
            i++;
        }
        if (i == subLen) return TRUE;
        str++;
    }
    return FALSE;
}

static void CopyStrW(WCHAR *dest, const WCHAR *src, DWORD maxLen) {
    DWORD i = 0;
    if (!dest || maxLen == 0) return;
    while (src && src[i] && i + 1 < maxLen) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = L'\0';
}

static void AppendStrW(WCHAR *dest, const WCHAR *src, DWORD maxLen) {
    DWORD len = StrLenW(dest);
    DWORD i = 0;
    while (src && src[i] && len + i + 1 < maxLen) {
        dest[len + i] = src[i];
        i++;
    }
    dest[len + i] = L'\0';
}

// Convert unsigned integer to string
static void UIntToStrW(DWORD val, WCHAR *buf, DWORD bufLen) {
    WCHAR tmp[16];
    DWORD i = 0, j = 0;
    if (val == 0) {
        if (bufLen > 1) { buf[0] = L'0'; buf[1] = L'\0'; }
        return;
    }
    while (val > 0 && i < 15) {
        tmp[i++] = (WCHAR)(L'0' + (val % 10));
        val /= 10;
    }
    while (i > 0 && j + 1 < bufLen) {
        buf[j++] = tmp[--i];
    }
    buf[j] = L'\0';
}

// CRT intrinsic replacements when building with /GS- and custom entry
#pragma function(memcpy)
void *memcpy(void *dest, const void *src, size_t count) {
    char *d = (char *)dest;
    const char *s = (const char *)src;
    while (count--) *d++ = *s++;
    return dest;
}

#pragma function(memset)
void *memset(void *dest, int c, size_t count) {
    char *d = (char *)dest;
    while (count--) *d++ = (char)c;
    return dest;
}

// Windows Event Log Logger
static void LogEvent(WORD wType, const WCHAR *msg) {
    HANDLE hSrc = RegisterEventSourceW(NULL, SERVICE_NAME);
    if (hSrc) {
        const WCHAR *strings[1] = { msg };
        ReportEventW(hSrc, wType, 0, 1001, NULL, 1, 0, strings, NULL);
        DeregisterEventSource(hSrc);
    }
}

// Read configuration from Registry: HKLM\SYSTEM\CurrentControlSet\Services\DownloadOrganizerSvc\Parameters
static void LoadConfiguration(void) {
    HKEY hKey;
    g_WatchPath[0] = L'\0';

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, PARAM_REG_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        WCHAR rawPath[MAX_PATH];
        DWORD rawSize = sizeof(rawPath);
        DWORD dwType = 0;

        if (RegQueryValueExW(hKey, L"DownloadsPath", NULL, &dwType, (LPBYTE)rawPath, &rawSize) == ERROR_SUCCESS) {
            if (dwType == REG_EXPAND_SZ) {
                ExpandEnvironmentStringsW(rawPath, g_WatchPath, MAX_PATH);
            } else if (dwType == REG_SZ) {
                CopyStrW(g_WatchPath, rawPath, MAX_PATH);
            }
        }

        DWORD dwDelay = 0;
        DWORD dwDelaySize = sizeof(dwDelay);
        DWORD dwTypeDelay = 0;
        if (RegQueryValueExW(hKey, L"SettleDelaySeconds", NULL, &dwTypeDelay, (LPBYTE)&dwDelay, &dwDelaySize) == ERROR_SUCCESS) {
            if (dwTypeDelay == REG_DWORD && dwDelay >= 1 && dwDelay <= 300) {
                g_SettleDelaySeconds = dwDelay;
            }
        }
        RegCloseKey(hKey);
    }

    // Fallback if registry value is absent or empty
    if (g_WatchPath[0] == L'\0') {
        CopyStrW(g_WatchPath, DEFAULT_PATH, MAX_PATH);
    }

    // Strip trailing backslash if present
    DWORD len = StrLenW(g_WatchPath);
    if (len > 0 && g_WatchPath[len - 1] == L'\\') {
        g_WatchPath[len - 1] = L'\0';
    }
}

// Check if a directory exists, create if not
static BOOL EnsureDirectoryExists(const WCHAR *path) {
    DWORD dwAttrib = GetFileAttributesW(path);
    if (dwAttrib != INVALID_FILE_ATTRIBUTES && (dwAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
        return TRUE;
    }
    return CreateDirectoryW(path, NULL);
}

// Check if extension should be skipped (e.g. temporary / partial downloads)
static BOOL IsTemporaryOrPartial(const WCHAR *ext) {
    if (!ext || !*ext) return FALSE;
    if (StrEqualNoCaseW(ext, L"crdownload") ||
        StrEqualNoCaseW(ext, L"part") ||
        StrEqualNoCaseW(ext, L"tmp") ||
        StrEqualNoCaseW(ext, L"download") ||
        StrEqualNoCaseW(ext, L"partial") ||
        StrEqualNoCaseW(ext, L"aria2") ||
        StrEqualNoCaseW(ext, L"unfinished") ||
        StrEqualNoCaseW(ext, L"opdownload") ||
        StrEqualNoCaseW(ext, L"!ut") ||
        StrEqualNoCaseW(ext, L"qbdownload")) {
        return TRUE;
    }
    return FALSE;
}

// Check if a filename matches the PB<digits>MAT pattern (e.g. PB2MAT, PB10MAT)
static BOOL HasPBMATPattern(const WCHAR *str) {
    if (!str) return FALSE;
    while (*str) {
        if ((str[0] == L'P' || str[0] == L'p') && (str[1] == L'B' || str[1] == L'b')) {
            const WCHAR *p = str + 2;
            if (*p >= L'0' && *p <= L'9') {
                while (*p >= L'0' && *p <= L'9') p++;
                if ((p[0] == L'M' || p[0] == L'm') &&
                    (p[1] == L'A' || p[1] == L'a') &&
                    (p[2] == L'T' || p[2] == L't')) {
                    return TRUE;
                }
            }
        }
        str++;
    }
    return FALSE;
}

// Check if file is a campus/academic file
static BOOL IsCampusFile(const WCHAR *fileName) {
    const WCHAR *ext = GetFileExt(fileName);

    static const WCHAR *academicExts[] = {
        L"pdf", L"doc", L"docx", L"xls", L"xlsx",
        L"ppt", L"pptx", L"txt", L"csv", L"zip", L"rar", L"7z"
    };
    BOOL extValid = FALSE;
    for (DWORD i = 0; i < sizeof(academicExts) / sizeof(academicExts[0]); i++) {
        if (StrEqualNoCaseW(ext, academicExts[i])) {
            extValid = TRUE;
            break;
        }
    }
    if (!extValid) return FALSE;

    // 1. Pattern PB*MAT (e.g. PB2MAT+Pertemuan2.pdf, PB10MAT_TIZ22...)
    if (HasPBMATPattern(fileName)) {
        return TRUE;
    }

    // 2. Course / Campus keywords (e.g. TIB, TIZ, UBM, Diskrit)
    static const WCHAR *campusKeywords[] = {
        L"TIB", L"TIZ", L"UBM", L"Diskrit"
    };
    for (DWORD i = 0; i < sizeof(campusKeywords) / sizeof(campusKeywords[0]); i++) {
        if (StrContainsNoCaseW(fileName, campusKeywords[i])) {
            return TRUE;
        }
    }

    return FALSE;
}

// Check if folder name is one of our managed category folders
static BOOL IsManagedCategoryFolder(const WCHAR *folderName) {
    if (StrEqualNoCaseW(folderName, CATEGORY_KAMPUS)) {
        return TRUE;
    }
    for (DWORD i = 0; i < CATEGORY_COUNT; i++) {
        if (StrEqualNoCaseW(folderName, g_Categories[i].CategoryName)) {
            return TRUE;
        }
    }
    if (StrEqualNoCaseW(folderName, CATEGORY_OTHER)) {
        return TRUE;
    }
    return FALSE;
}

// Match extension to category
static const WCHAR *GetCategoryForExtension(const WCHAR *ext) {
    if (!ext || !*ext) return CATEGORY_OTHER;

    for (DWORD i = 0; i < CATEGORY_COUNT; i++) {
        const WCHAR *list = g_Categories[i].Extensions;
        WCHAR token[16];
        DWORD tIdx = 0;

        while (*list) {
            if (*list == L';') {
                token[tIdx] = L'\0';
                if (StrEqualNoCaseW(ext, token)) {
                    return g_Categories[i].CategoryName;
                }
                tIdx = 0;
            } else if (tIdx + 1 < sizeof(token) / sizeof(token[0])) {
                token[tIdx++] = *list;
            }
            list++;
        }
        token[tIdx] = L'\0';
        if (StrEqualNoCaseW(ext, token)) {
            return g_Categories[i].CategoryName;
        }
    }

    return CATEGORY_OTHER;
}

// Extract file extension without leading dot
static const WCHAR *GetFileExt(const WCHAR *filename) {
    const WCHAR *dot = NULL;
    while (*filename) {
        if (*filename == L'.') dot = filename + 1;
        filename++;
    }
    return dot ? dot : L"";
}

// Build destination path with collision resolution: "file.ext" -> "file (1).ext"
static void BuildUniqueTargetPath(const WCHAR *destDir, const WCHAR *fileName, WCHAR *outPath, DWORD maxLen) {
    WCHAR baseName[MAX_PATH];
    WCHAR ext[64];
    const WCHAR *dot = NULL;

    // Separate base and ext
    const WCHAR *p = fileName;
    while (*p) {
        if (*p == L'.') dot = p;
        p++;
    }

    if (dot) {
        DWORD baseLen = (DWORD)(dot - fileName);
        if (baseLen >= MAX_PATH) baseLen = MAX_PATH - 1;
        for (DWORD i = 0; i < baseLen; i++) baseName[i] = fileName[i];
        baseName[baseLen] = L'\0';
        CopyStrW(ext, dot, sizeof(ext) / sizeof(ext[0])); // includes leading dot
    } else {
        CopyStrW(baseName, fileName, MAX_PATH);
        ext[0] = L'\0';
    }

    // Try primary path: destDir\fileName
    CopyStrW(outPath, destDir, maxLen);
    AppendStrW(outPath, L"\\", maxLen);
    AppendStrW(outPath, fileName, maxLen);

    if (GetFileAttributesW(outPath) == INVALID_FILE_ATTRIBUTES) {
        return; // No collision
    }

    // Collision resolution loop: "base (n).ext"
    for (DWORD counter = 1; counter < 10000; counter++) {
        WCHAR countStr[16];
        UIntToStrW(counter, countStr, sizeof(countStr) / sizeof(countStr[0]));

        CopyStrW(outPath, destDir, maxLen);
        AppendStrW(outPath, L"\\", maxLen);
        AppendStrW(outPath, baseName, maxLen);
        AppendStrW(outPath, L" (", maxLen);
        AppendStrW(outPath, countStr, maxLen);
        AppendStrW(outPath, L")", maxLen);
        AppendStrW(outPath, ext, maxLen);

        if (GetFileAttributesW(outPath) == INVALID_FILE_ATTRIBUTES) {
            return;
        }
    }
}

// Move a single file into its category folder
static void ProcessFileItem(const WCHAR *baseDir, const WCHAR *fileName) {
    const WCHAR *ext = GetFileExt(fileName);

    // Skip temp/partial download extensions
    if (IsTemporaryOrPartial(ext)) {
        return;
    }

    const WCHAR *category = NULL;
    if (IsCampusFile(fileName)) {
        category = CATEGORY_KAMPUS;
    } else {
        category = GetCategoryForExtension(ext);
    }

    // Ensure category folder exists: baseDir\category
    WCHAR destDir[MAX_PATH];
    CopyStrW(destDir, baseDir, MAX_PATH);
    AppendStrW(destDir, L"\\", MAX_PATH);
    AppendStrW(destDir, category, MAX_PATH);

    if (!EnsureDirectoryExists(destDir)) {
        LogEvent(EVENTLOG_ERROR_TYPE, L"Failed to create target category directory.");
        return;
    }

    // Full source path
    WCHAR srcPath[MAX_PATH];
    CopyStrW(srcPath, baseDir, MAX_PATH);
    AppendStrW(srcPath, L"\\", MAX_PATH);
    AppendStrW(srcPath, fileName, MAX_PATH);

    // Unique destination path
    WCHAR destPath[MAX_PATH];
    BuildUniqueTargetPath(destDir, fileName, destPath, MAX_PATH);

    // Move file. If file is locked (e.g. browser still flushing), MoveFileExW will fail
    // and waitable timer/subsequent notification will retry later.
    if (!MoveFileExW(srcPath, destPath, MOVEFILE_COPY_ALLOWED)) {
        DWORD err = GetLastError();
        if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED) {
            WCHAR errBuf[64];
            UIntToStrW(err, errBuf, 64);
            WCHAR msg[256];
            CopyStrW(msg, L"MoveFileEx failed with error: ", 256);
            AppendStrW(msg, errBuf, 256);
            LogEvent(EVENTLOG_WARNING_TYPE, msg);
        }
    }
}

// Debounce / retry timer: trigger sweep after specified delay in seconds
static void ScheduleDebouncedScan(DWORD delaySeconds) {
    LARGE_INTEGER liDueTime;
    liDueTime.QuadPart = -((LONGLONG)delaySeconds * 10000000LL);
    SetWaitableTimer(g_TimerEvent, &liDueTime, 0, NULL, NULL, FALSE);
}

// Check if file is newly created, actively downloading, or locked by another process
static BOOL IsFileBeingDownloaded(const WCHAR *baseDir, const WCHAR *fileName, const WIN32_FIND_DATAW *fd) {
    const WCHAR *ext = GetFileExt(fileName);

    // 1. Skip temporary/partial download extensions
    if (IsTemporaryOrPartial(ext)) {
        return TRUE;
    }

    // 2. Check for companion partial download files (e.g. fileName.crdownload, fileName.part)
    static const WCHAR *companionExts[] = {
        L".crdownload", L".part", L".download", L".tmp", L".partial", L".aria2", L".!ut", L".opdownload"
    };
    WCHAR compPath[MAX_PATH];
    for (DWORD i = 0; i < sizeof(companionExts) / sizeof(companionExts[0]); i++) {
        CopyStrW(compPath, baseDir, MAX_PATH);
        AppendStrW(compPath, L"\\", MAX_PATH);
        AppendStrW(compPath, fileName, MAX_PATH);
        AppendStrW(compPath, companionExts[i], MAX_PATH);
        if (GetFileAttributesW(compPath) != INVALID_FILE_ATTRIBUTES) {
            return TRUE; // Download companion exists, file is still downloading
        }
    }

    // 3. Check file age: if modified or created within the settle delay window, wait
    FILETIME ftNow;
    GetSystemTimeAsFileTime(&ftNow);

    ULARGE_INTEGER nowVal, writeVal, createVal;
    nowVal.LowPart = ftNow.dwLowDateTime;
    nowVal.HighPart = ftNow.dwHighDateTime;
    writeVal.LowPart = fd->ftLastWriteTime.dwLowDateTime;
    writeVal.HighPart = fd->ftLastWriteTime.dwHighDateTime;
    createVal.LowPart = fd->ftCreationTime.dwLowDateTime;
    createVal.HighPart = fd->ftCreationTime.dwHighDateTime;

    ULONGLONG waitUnits = (ULONGLONG)g_SettleDelaySeconds * 10000000ULL;

    if (nowVal.QuadPart >= writeVal.QuadPart) {
        if ((nowVal.QuadPart - writeVal.QuadPart) < waitUnits) {
            return TRUE; // Modified less than g_SettleDelaySeconds ago
        }
    }

    if (nowVal.QuadPart >= createVal.QuadPart) {
        if ((nowVal.QuadPart - createVal.QuadPart) < waitUnits) {
            return TRUE; // Created less than g_SettleDelaySeconds ago
        }
    }

    // 4. Check if 0-byte file created recently (< 30 sec)
    if (fd->nFileSizeHigh == 0 && fd->nFileSizeLow == 0) {
        if (nowVal.QuadPart >= createVal.QuadPart) {
            if ((nowVal.QuadPart - createVal.QuadPart) < (30ULL * 10000000ULL)) {
                return TRUE; // Empty placeholder created recently
            }
        }
    }

    // 5. Test if file is open/locked by another process (exclusive access check)
    WCHAR fullPath[MAX_PATH];
    CopyStrW(fullPath, baseDir, MAX_PATH);
    AppendStrW(fullPath, L"\\", MAX_PATH);
    AppendStrW(fullPath, fileName, MAX_PATH);

    HANDLE hFile = CreateFileW(
        fullPath,
        GENERIC_READ | GENERIC_WRITE,
        0, // Exclusive access: fail if ANY process has an open handle
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION || err == ERROR_ACCESS_DENIED) {
            return TRUE; // Actively held open by browser or downloader
        }
    } else {
        CloseHandle(hFile);
    }

    return FALSE;
}

// Scan and organize top-level directory items
static void ScanAndOrganize(const WCHAR *watchPath) {
    WCHAR searchPattern[MAX_PATH];
    CopyStrW(searchPattern, watchPath, MAX_PATH);
    AppendStrW(searchPattern, L"\\*", MAX_PATH);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    BOOL hasPendingFiles = FALSE;

    do {
        // Skip "." and ".."
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) {
            continue;
        }

        // Skip hidden or system files
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) {
            continue;
        }

        // Handle directories: ignore category folders and unconfigured directories
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Folders are preserved unless custom rule matches; category folders ignored
            continue;
        }

        // Check if file is newly created, downloading, or in-use
        if (IsFileBeingDownloaded(watchPath, fd.cFileName, &fd)) {
            hasPendingFiles = TRUE;
            continue;
        }

        // Process top-level file
        ProcessFileItem(watchPath, fd.cFileName);

    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);

    if (hasPendingFiles) {
        // Retry scan after settle delay to process files once they finish downloading
        ScheduleDebouncedScan(g_SettleDelaySeconds);
    }
}

// Migrate existing campus files from Documents to Kampus folder (one-time sweep at startup)
static void MigrateExistingDocuments(const WCHAR *watchPath) {
    WCHAR docDir[MAX_PATH];
    CopyStrW(docDir, watchPath, MAX_PATH);
    AppendStrW(docDir, L"\\Documents", MAX_PATH);

    WCHAR kampusDir[MAX_PATH];
    CopyStrW(kampusDir, watchPath, MAX_PATH);
    AppendStrW(kampusDir, L"\\" CATEGORY_KAMPUS, MAX_PATH);
    EnsureDirectoryExists(kampusDir);

    WCHAR searchPattern[MAX_PATH];
    CopyStrW(searchPattern, docDir, MAX_PATH);
    AppendStrW(searchPattern, L"\\*", MAX_PATH);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) {
            continue;
        }

        if (IsCampusFile(fd.cFileName)) {
            WCHAR srcPath[MAX_PATH];
            CopyStrW(srcPath, docDir, MAX_PATH);
            AppendStrW(srcPath, L"\\", MAX_PATH);
            AppendStrW(srcPath, fd.cFileName, MAX_PATH);

            WCHAR destPath[MAX_PATH];
            BuildUniqueTargetPath(kampusDir, fd.cFileName, destPath, MAX_PATH);

            MoveFileExW(srcPath, destPath, MOVEFILE_COPY_ALLOWED);
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

// Issue asynchronous ReadDirectoryChangesW request
static BOOL ArmDirectoryWatch(void) {
    if (g_WatchDir == INVALID_HANDLE_VALUE) return FALSE;

    ResetEvent(g_Overlapped.hEvent);

    // bWatchSubtree is FALSE to watch only the top-level Downloads directory.
    // Changes made inside category subfolders will not trigger events (avoids loop feedback).
    BOOL ok = ReadDirectoryChangesW(
        g_WatchDir,
        g_ChangeBuf,
        sizeof(g_ChangeBuf),
        FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE,
        NULL,
        &g_Overlapped,
        NULL
    );

    return ok || (GetLastError() == ERROR_IO_PENDING);
}

// SCM Status Reporting
static void ReportServiceStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint) {
    static DWORD dwCheckPoint = 1;

    g_ServiceStatus.dwServiceType      = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState     = dwCurrentState;
    g_ServiceStatus.dwWin32ExitCode    = dwWin32ExitCode;
    g_ServiceStatus.dwWaitHint         = dwWaitHint;
    g_ServiceStatus.dwControlsAccepted = (dwCurrentState == SERVICE_START_PENDING)
                                             ? 0
                                             : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);

    if (dwCurrentState == SERVICE_RUNNING || dwCurrentState == SERVICE_STOPPED) {
        g_ServiceStatus.dwCheckPoint = 0;
    } else {
        g_ServiceStatus.dwCheckPoint = dwCheckPoint++;
    }

    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

// SCM Control Handler
static void WINAPI ServiceCtrlHandler(DWORD dwCtrlCode) {
    switch (dwCtrlCode) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            ReportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
            if (g_StopEvent) {
                SetEvent(g_StopEvent);
            }
            break;
        default:
            break;
    }
}

// Main Service Routine
static void WINAPI ServiceMain(DWORD dwArgc, LPWSTR *lpszArgv) {
    (void)dwArgc;
    (void)lpszArgv;

    g_StatusHandle = RegisterServiceCtrlHandlerW(SERVICE_NAME, ServiceCtrlHandler);
    if (!g_StatusHandle) return;

    ReportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 5000);

    LoadConfiguration();

    // Create sync events
    g_StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_TimerEvent = CreateWaitableTimerW(NULL, FALSE, NULL);
    g_Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    if (!g_StopEvent || !g_TimerEvent || !g_Overlapped.hEvent) {
        LogEvent(EVENTLOG_ERROR_TYPE, L"Failed to create synchronization events.");
        ReportServiceStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    // Ensure watch path exists
    if (!EnsureDirectoryExists(g_WatchPath)) {
        LogEvent(EVENTLOG_ERROR_TYPE, L"Cannot access or create target Downloads folder.");
        ReportServiceStatus(SERVICE_STOPPED, ERROR_PATH_NOT_FOUND, 0);
        return;
    }

    // Open directory handle with overlapped I/O
    g_WatchDir = CreateFileW(
        g_WatchPath,
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        NULL
    );

    if (g_WatchDir == INVALID_HANDLE_VALUE) {
        LogEvent(EVENTLOG_ERROR_TYPE, L"Failed to open directory handle for watching.");
        ReportServiceStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    // Start directory change monitoring
    if (!ArmDirectoryWatch()) {
        LogEvent(EVENTLOG_ERROR_TYPE, L"Failed to arm ReadDirectoryChangesW.");
        ReportServiceStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    // Perform initial startup scan of existing items
    ScanAndOrganize(g_WatchPath);
    MigrateExistingDocuments(g_WatchPath);

    ReportServiceStatus(SERVICE_RUNNING, NO_ERROR, 0);

    // Event wait handles: Stop, Directory Change, Debounce Timer
    HANDLE hWaitObjects[EVENT_COUNT];
    hWaitObjects[EVENT_STOP]       = g_StopEvent;
    hWaitObjects[EVENT_DIR_CHANGE] = g_Overlapped.hEvent;
    hWaitObjects[EVENT_TIMER]      = g_TimerEvent;

    // Event-driven loop (0% idle CPU)
    for (;;) {
        DWORD dwWait = WaitForMultipleObjects(EVENT_COUNT, hWaitObjects, FALSE, INFINITE);

        if (dwWait == WAIT_OBJECT_0 + EVENT_STOP) {
            break;
        }

        if (dwWait == WAIT_OBJECT_0 + EVENT_DIR_CHANGE) {
            DWORD dwBytesTransferred = 0;
            GetOverlappedResult(g_WatchDir, &g_Overlapped, &dwBytesTransferred, FALSE);

            // Re-arm immediately for next notifications
            ArmDirectoryWatch();

            // Schedule debounced scan (avoids fighting lock while browser is flushing write stream)
            ScheduleDebouncedScan(g_SettleDelaySeconds);
        } else if (dwWait == WAIT_OBJECT_0 + EVENT_TIMER) {
            // Timer expired: execute directory organization pass
            ScanAndOrganize(g_WatchPath);
        }
    }

    ReportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 3000);

    // Cleanup resources
    if (g_WatchDir != INVALID_HANDLE_VALUE) {
        CancelIo(g_WatchDir);
        CloseHandle(g_WatchDir);
        g_WatchDir = INVALID_HANDLE_VALUE;
    }

    if (g_Overlapped.hEvent) CloseHandle(g_Overlapped.hEvent);
    if (g_TimerEvent)        CloseHandle(g_TimerEvent);
    if (g_StopEvent)         CloseHandle(g_StopEvent);

    ReportServiceStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

// Process entry point
void __cdecl mainCRTStartup(void) {
    static const SERVICE_TABLE_ENTRYW serviceTable[] = {
        { (LPWSTR)SERVICE_NAME, ServiceMain },
        { NULL, NULL }
    };

    if (!StartServiceCtrlDispatcherW(serviceTable)) {
        ExitProcess(GetLastError());
    }

    ExitProcess(0);
}
