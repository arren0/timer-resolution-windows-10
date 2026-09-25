// timerfix.cpp
//
// TimerFix for Windows 10
//
// Based on the observed DPC Latency Checker initialization sequence:
//   Service: dpclat_driver
//   Driver : %SystemRoot%\System32\drivers\dpclat_driver.sys
//   Device : \\.\dpclat_static_device
//
// The API Monitor capture showed:
//   CreateServiceA -> StartServiceA -> CreateFileA("\\.\dpclat_static_device")
//   followed by DeviceIoControl with IOCTL 0x81772010.
//
// IMPORTANT:
// This program does NOT implement a fake timer-resolution workaround.
// It keeps the observed Thesycon DPC driver active. The driver file itself
// is NOT included here.
//
// If dpclat_driver is already installed, TimerFix uses it.
// If it is not installed, TimerFix looks for dpclat_driver.sys next to
// TimerFix.exe and installs it temporarily.
//
// Build with MSVC:
//   cl /O2 /Os /EHsc /DUNICODE /D_UNICODE timerfix.cpp ^
//      /link /SUBSYSTEM:WINDOWS user32.lib advapi32.lib shell32.lib
//
// The program has no visible window and only one tray item: Exit.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>
#include <winsvc.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
    constexpr wchar_t SERVICE_NAME[] = L"dpclat_driver";
    constexpr wchar_t DEVICE_NAME[]  = L"\\\\.\\dpclat_static_device";
    constexpr DWORD IOCTL_DPC_QUERY  = 0x81772010;

    constexpr UINT WM_TRAYICON = WM_APP + 1;
    constexpr UINT ID_EXIT     = 1001;

    HWND g_hwnd = nullptr;
    HANDLE g_device = INVALID_HANDLE_VALUE;
    HANDLE g_worker = nullptr;
    HANDLE g_stop = nullptr;

    SC_HANDLE g_scm = nullptr;
    SC_HANDLE g_service = nullptr;

    bool g_createdService = false;
    bool g_startedService = false;

    NOTIFYICONDATAW g_nid{};

    // ---------------------------------------------------------------------
    // Find our executable directory
    // ---------------------------------------------------------------------

    bool GetExeDirectory(wchar_t* out, DWORD count)
    {
        if (!out || count < 2)
            return false;

        DWORD n = GetModuleFileNameW(
            nullptr,
            out,
            count
        );

        if (!n || n >= count)
            return false;

        for (DWORD i = n; i > 0; --i)
        {
            if (out[i - 1] == L'\\')
            {
                out[i - 1] = L'\0';
                return true;
            }
        }

        return false;
    }

    // ---------------------------------------------------------------------
    // Service handling
    // ---------------------------------------------------------------------

    bool OpenSCM()
    {
        g_scm = OpenSCManagerW(
            nullptr,
            nullptr,
            SC_MANAGER_CONNECT |
            SC_MANAGER_CREATE_SERVICE
        );

        return g_scm != nullptr;
    }

    bool OpenExistingService()
    {
        if (!g_scm)
            return false;

        g_service = OpenServiceW(
            g_scm,
            SERVICE_NAME,
            SERVICE_START |
            SERVICE_STOP |
            SERVICE_QUERY_STATUS |
            DELETE
        );

        return g_service != nullptr;
    }

    bool InstallServiceFromLocalDriver()
    {
        wchar_t dir[MAX_PATH]{};

        if (!GetExeDirectory(dir, ARRAYSIZE(dir)))
            return false;

        wchar_t source[MAX_PATH]{};
        wchar_t systemDir[MAX_PATH]{};
        wchar_t target[MAX_PATH]{};

        lstrcpynW(source, dir, ARRAYSIZE(source));

        if (lstrlenW(source) + 18 >= ARRAYSIZE(source))
            return false;

        lstrcatW(source, L"\\dpclat_driver.sys");

        if (GetSystemDirectoryW(
                systemDir,
                ARRAYSIZE(systemDir)) == 0)
        {
            return false;
        }

        if (lstrlenW(systemDir) + 22 >= ARRAYSIZE(target))
            return false;

        lstrcpynW(target, systemDir, ARRAYSIZE(target));
        lstrcatW(target, L"\\drivers\\dpclat_driver.sys");

        DWORD attrs = GetFileAttributesW(source);

        if (attrs == INVALID_FILE_ATTRIBUTES)
            return false;

        // Copy only if the system copy is missing.
        if (GetFileAttributesW(target) == INVALID_FILE_ATTRIBUTES)
        {
            if (!CopyFileW(
                    source,
                    target,
                    TRUE))
            {
                return false;
            }
        }

        g_service = CreateServiceW(
            g_scm,
            SERVICE_NAME,
            SERVICE_NAME,
            SERVICE_START |
            SERVICE_STOP |
            SERVICE_QUERY_STATUS |
            DELETE,
            SERVICE_KERNEL_DRIVER,
            SERVICE_DEMAND_START,
            SERVICE_ERROR_NORMAL,
            target,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr
        );

        if (!g_service)
            return false;

        g_createdService = true;
        return true;
    }

    bool StartDriver()
    {
        if (!g_service)
            return false;

        SERVICE_STATUS_PROCESS status{};
        DWORD needed = 0;

        if (!QueryServiceStatusEx(
                g_service,
                SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&status),
                sizeof(status),
                &needed))
        {
            return false;
        }

        if (status.dwCurrentState == SERVICE_RUNNING)
            return true;

        if (!StartServiceW(
                g_service,
                0,
                nullptr))
        {
            DWORD err = GetLastError();

            if (err == ERROR_SERVICE_ALREADY_RUNNING)
                return true;

            return false;
        }

        g_startedService = true;

        // Wait briefly for the driver to become running.
        for (int i = 0; i < 100; ++i)
        {
            Sleep(10);

            if (!QueryServiceStatusEx(
                    g_service,
                    SC_STATUS_PROCESS_INFO,
                    reinterpret_cast<LPBYTE>(&status),
                    sizeof(status),
                    &needed))
            {
                return false;
            }

            if (status.dwCurrentState == SERVICE_RUNNING)
                return true;

            if (status.dwCurrentState == SERVICE_STOPPED)
                return false;
        }

        return false;
    }

    void StopAndCleanupDriver()
    {
        if (g_service)
        {
            if (g_startedService)
            {
                SERVICE_STATUS status{};

                ControlService(
                    g_service,
                    SERVICE_CONTROL_STOP,
                    &status
                );
            }

            if (g_createdService)
            {
                DeleteService(g_service);
            }

            CloseServiceHandle(g_service);
            g_service = nullptr;
        }

        if (g_scm)
        {
            CloseServiceHandle(g_scm);
            g_scm = nullptr;
        }
    }

    // ---------------------------------------------------------------------
    // Device
    // ---------------------------------------------------------------------

    bool OpenDpcDevice()
    {
        g_device = CreateFileW(
            DEVICE_NAME,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        return g_device != INVALID_HANDLE_VALUE;
    }

    // The captured program passes a 20-byte structure through
    // IOCTL 0x81772010. These values reproduce the initial structure
    // visible in the capture. The driver owns the output values.
    bool QueryDriver()
    {
        if (g_device == INVALID_HANDLE_VALUE)
            return false;

        DWORD data[5] =
        {
            0xFFFFFFFFu,
            0u,
            300u,
            275u,
            1u
        };

        DWORD returned = 0;

        return DeviceIoControl(
            g_device,
            IOCTL_DPC_QUERY,
            data,
            sizeof(data),
            data,
            sizeof(data),
            &returned,
            nullptr
        ) != FALSE;
    }

    // Keep the same driver interaction alive once per second.
    DWORD WINAPI WorkerThread(LPVOID)
    {
        // The actual latency-generating/measurement mechanism is in
        // dpclat_driver.sys. The user-mode part stays almost completely
        // idle and only performs the observed 1 Hz IOCTL.
        QueryDriver();

        while (WaitForSingleObject(
                   g_stop,
                   1000) == WAIT_TIMEOUT)
        {
            QueryDriver();
        }

        return 0;
    }

    // ---------------------------------------------------------------------
    // Tray icon
    // ---------------------------------------------------------------------

    bool AddTrayIcon(HWND hwnd)
    {
        ZeroMemory(&g_nid, sizeof(g_nid));

        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = hwnd;
        g_nid.uID = 1;
        g_nid.uFlags =
            NIF_MESSAGE |
            NIF_ICON |
            NIF_TIP;

        g_nid.uCallbackMessage = WM_TRAYICON;

        g_nid.hIcon = LoadIconW(
            nullptr,
            MAKEINTRESOURCEW(IDI_APPLICATION)
        );

        lstrcpynW(
            g_nid.szTip,
            L"TimerFix",
            ARRAYSIZE(g_nid.szTip)
        );

        return Shell_NotifyIconW(
            NIM_ADD,
            &g_nid
        ) != FALSE;
    }

    void RemoveTrayIcon()
    {
        Shell_NotifyIconW(
            NIM_DELETE,
            &g_nid
        );
    }

    void ShowTrayMenu(HWND hwnd)
    {
        HMENU menu = CreatePopupMenu();

        if (!menu)
            return;

        AppendMenuW(
            menu,
            MF_STRING,
            ID_EXIT,
            L"Exit"
        );

        POINT pt{};
        GetCursorPos(&pt);

        SetForegroundWindow(hwnd);

        TrackPopupMenu(
            menu,
            TPM_RIGHTBUTTON |
            TPM_BOTTOMALIGN |
            TPM_LEFTALIGN,
            pt.x,
            pt.y,
            0,
            hwnd,
            nullptr
        );

        DestroyMenu(menu);

        PostMessageW(
            hwnd,
            WM_NULL,
            0,
            0
        );
    }

    // ---------------------------------------------------------------------
    // Hidden window
    // ---------------------------------------------------------------------

    LRESULT CALLBACK WindowProc(
        HWND hwnd,
        UINT message,
        WPARAM wParam,
        LPARAM lParam)
    {
        switch (message)
        {
        case WM_TRAYICON:

            if (lParam == WM_RBUTTONUP ||
                lParam == WM_CONTEXTMENU)
            {
                ShowTrayMenu(hwnd);
            }

            return 0;

        case WM_COMMAND:

            if (LOWORD(wParam) == ID_EXIT)
            {
                DestroyWindow(hwnd);
                return 0;
            }

            break;

        case WM_DESTROY:

            if (g_stop)
                SetEvent(g_stop);

            if (g_worker)
            {
                WaitForSingleObject(
                    g_worker,
                    INFINITE
                );

                CloseHandle(g_worker);
                g_worker = nullptr;
            }

            if (g_device != INVALID_HANDLE_VALUE)
            {
                CloseHandle(g_device);
                g_device = INVALID_HANDLE_VALUE;
            }

            RemoveTrayIcon();

            StopAndCleanupDriver();

            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(
            hwnd,
            message,
            wParam,
            lParam
        );
    }

    HWND CreateHiddenWindow(HINSTANCE instance)
    {
        constexpr wchar_t CLASS_NAME[] =
            L"TimerFixHiddenWindow";

        WNDCLASSEXW wc{};

        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance;
        wc.lpszClassName = CLASS_NAME;

        if (!RegisterClassExW(&wc))
            return nullptr;

        return CreateWindowExW(
            0,
            CLASS_NAME,
            L"TimerFix",
            0,
            0,
            0,
            0,
            0,
            HWND_MESSAGE,
            nullptr,
            instance,
            nullptr
        );
    }

    bool AlreadyRunning()
    {
        HANDLE mutex = CreateMutexW(
            nullptr,
            TRUE,
            L"Global\\TimerFix_Dpclat"
        );

        if (!mutex)
            return false;

        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            CloseHandle(mutex);
            return true;
        }

        // Intentionally keep the mutex alive for the lifetime of the process.
        static HANDLE keepAlive = nullptr;
        keepAlive = mutex;

        return false;
    }
}

// -------------------------------------------------------------------------
// Entry point
// -------------------------------------------------------------------------

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    PWSTR,
    int)
{
    if (AlreadyRunning())
        return 0;

    // Driver/service setup.
    if (!OpenSCM())
        return 1;

    if (!OpenExistingService())
    {
        // Optional local driver installation.
        if (!InstallServiceFromLocalDriver())
        {
            CloseServiceHandle(g_scm);
            g_scm = nullptr;
            return 1;
        }
    }

    if (!StartDriver())
    {
        StopAndCleanupDriver();
        return 1;
    }

    // Open the exact device observed in the API Monitor capture.
    if (!OpenDpcDevice())
    {
        StopAndCleanupDriver();
        return 1;
    }

    // Create the invisible tray window.
    g_stop = CreateEventW(
        nullptr,
        TRUE,
        FALSE,
        nullptr
    );

    if (!g_stop)
    {
        CloseHandle(g_device);
        g_device = INVALID_HANDLE_VALUE;
        StopAndCleanupDriver();
        return 1;
    }

    g_hwnd = CreateHiddenWindow(hInstance);

    if (!g_hwnd)
    {
        CloseHandle(g_stop);
        CloseHandle(g_device);
        g_device = INVALID_HANDLE_VALUE;
        StopAndCleanupDriver();
        return 1;
    }

    if (!AddTrayIcon(g_hwnd))
    {
        DestroyWindow(g_hwnd);
        CloseHandle(g_stop);
        CloseHandle(g_device);
        g_device = INVALID_HANDLE_VALUE;
        StopAndCleanupDriver();
        return 1;
    }

    // One tiny worker. No busy loop.
    g_worker = CreateThread(
        nullptr,
        0,
        WorkerThread,
        nullptr,
        0,
        nullptr
    );

    if (!g_worker)
    {
        DestroyWindow(g_hwnd);
        CloseHandle(g_stop);
        CloseHandle(g_device);
        g_device = INVALID_HANDLE_VALUE;
        StopAndCleanupDriver();
        return 1;
    }

    MSG msg{};

    while (GetMessageW(
        &msg,
        nullptr,
        0,
        0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}
