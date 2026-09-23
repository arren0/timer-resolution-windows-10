#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
    constexpr UINT WM_TRAYICON = WM_APP + 1;
    constexpr UINT ID_EXIT = 1001;

    constexpr DWORD TIMER_FLAGS =
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION;

    HWND g_hwnd = nullptr;
    HANDLE g_timer = nullptr;
    HANDLE g_stopEvent = nullptr;
    HANDLE g_worker = nullptr;

    NOTIFYICONDATAW g_nid{};

    bool g_timerResolution = false;

    // ---------------------------------------------------------------------
    // High resolution timer worker
    // ---------------------------------------------------------------------

    DWORD WINAPI TimerWorker(LPVOID)
    {
        // 1 ms system timer resolution.
        timeBeginPeriod(1);
        g_timerResolution = true;

        // Relative 1 ms interval.
        LARGE_INTEGER dueTime{};

        // Negative = relative time.
        // 1 ms = 10,000 * 100 ns.
        dueTime.QuadPart = -10000LL;

        while (true)
        {
            if (WaitForSingleObject(
                    g_stopEvent,
                    0) == WAIT_OBJECT_0)
            {
                break;
            }

            // Arm high-resolution timer.
            if (!SetWaitableTimerEx(
                    g_timer,
                    &dueTime,
                    0,
                    nullptr,
                    nullptr,
                    nullptr,
                    0))
            {
                break;
            }

            HANDLE handles[2] =
            {
                g_stopEvent,
                g_timer
            };

            DWORD result = WaitForMultipleObjects(
                2,
                handles,
                FALSE,
                INFINITE
            );

            if (result == WAIT_OBJECT_0)
                break;

            if (result != WAIT_OBJECT_0 + 1)
                break;
        }

        CancelWaitableTimer(g_timer);

        if (g_timerResolution)
        {
            timeEndPeriod(1);
            g_timerResolution = false;
        }

        return 0;
    }

    // ---------------------------------------------------------------------
    // Tray
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

        g_nid.hIcon =
            LoadIconW(
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
    // Window
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

            if (g_stopEvent)
                SetEvent(g_stopEvent);

            if (g_worker)
            {
                WaitForSingleObject(
                    g_worker,
                    INFINITE
                );

                CloseHandle(g_worker);
                g_worker = nullptr;
            }

            RemoveTrayIcon();

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
            L"TimerFixWindowClass";

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
}

// -------------------------------------------------------------------------
// Entry
// -------------------------------------------------------------------------

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    PWSTR,
    int)
{
    // Single instance.
    HANDLE mutex = CreateMutexW(
        nullptr,
        TRUE,
        L"Global\\TimerFix_HighResolution"
    );

    if (!mutex)
        return 1;

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(mutex);
        return 0;
    }

    // Stop event.
    g_stopEvent = CreateEventW(
        nullptr,
        TRUE,
        FALSE,
        nullptr
    );

    if (!g_stopEvent)
    {
        CloseHandle(mutex);
        return 1;
    }

    // High-resolution waitable timer.
    g_timer = CreateWaitableTimerExW(
        nullptr,
        nullptr,
        TIMER_FLAGS,
        TIMER_ALL_ACCESS
    );

    if (!g_timer)
    {
        CloseHandle(g_stopEvent);
        CloseHandle(mutex);
        return 1;
    }

    // Invisible message window.
    g_hwnd = CreateHiddenWindow(hInstance);

    if (!g_hwnd)
    {
        CloseHandle(g_timer);
        CloseHandle(g_stopEvent);
        CloseHandle(mutex);
        return 1;
    }

    // Tray icon.
    if (!AddTrayIcon(g_hwnd))
    {
        DestroyWindow(g_hwnd);
        CloseHandle(g_timer);
        CloseHandle(g_stopEvent);
        CloseHandle(mutex);
        return 1;
    }

    // Start timer worker.
    g_worker = CreateThread(
        nullptr,
        0,
        TimerWorker,
        nullptr,
        0,
        nullptr
    );

    if (!g_worker)
    {
        DestroyWindow(g_hwnd);
        CloseHandle(g_timer);
        CloseHandle(g_stopEvent);
        CloseHandle(mutex);
        return 1;
    }

    // Message loop.
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

    // Cleanup.
    if (g_stopEvent)
        SetEvent(g_stopEvent);

    if (g_worker)
    {
        WaitForSingleObject(
            g_worker,
            INFINITE
        );

        CloseHandle(g_worker);
    }

    if (g_timer)
        CloseHandle(g_timer);

    if (g_stopEvent)
        CloseHandle(g_stopEvent);

    CloseHandle(mutex);

    return 0;
}
