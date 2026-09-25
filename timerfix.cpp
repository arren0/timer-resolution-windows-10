// timerfix.cpp
// Windows 10 - minimal tray utility
//
// IMPORTANT:
// This version does NOT pretend to reproduce the undocumented driver IOCTL.
// It is a corrected, standalone tray program that remains running even when
// the DPC driver is unavailable. It uses a high-resolution waitable timer
// where supported and falls back cleanly.
//
// Build:
// cl /O2 /Os /EHsc /DUNICODE /D_UNICODE timerfix.cpp /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib winmm.lib

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>
#include <mmsystem.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")

namespace
{
    constexpr UINT WM_TRAYICON = WM_APP + 1;
    constexpr UINT ID_EXIT = 1;

    HWND   g_hwnd = nullptr;
    HANDLE g_stop = nullptr;
    HANDLE g_worker = nullptr;
    HANDLE g_timer = nullptr;

    NOTIFYICONDATAW g_tray{};

    // ------------------------------------------------------------
    // High-resolution timer
    // ------------------------------------------------------------

    using CreateWaitableTimerExWFn =
        HANDLE (WINAPI*)(LPSECURITY_ATTRIBUTES,
                          LPCWSTR,
                          DWORD,
                          DWORD);

    constexpr DWORD CREATE_WAITABLE_TIMER_HIGH_RESOLUTION_LOCAL = 0x00000002;

    HANDLE CreateHighResTimer()
    {
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");

        if (!kernel32)
            return nullptr;

        auto fn =
            reinterpret_cast<CreateWaitableTimerExWFn>(
                GetProcAddress(
                    kernel32,
                    "CreateWaitableTimerExW"));

        if (!fn)
            return nullptr;

        HANDLE timer = fn(
            nullptr,
            nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION_LOCAL,
            TIMER_ALL_ACCESS
        );

        return timer;
    }

    DWORD WINAPI Worker(LPVOID)
    {
        // Request the normal 1 ms system timer period.
        // This is only a scheduler hint; it is NOT presented as a
        // guarantee that Sleep(1) lasts 1 ms.
        timeBeginPeriod(1);

        g_timer = CreateHighResTimer();

        if (g_timer)
        {
            LARGE_INTEGER due{};
            due.QuadPart = -10000LL; // 1 ms relative

            HANDLE handles[2] =
            {
                g_stop,
                g_timer
            };

            while (true)
            {
                if (WaitForSingleObject(
                        g_stop,
                        0) == WAIT_OBJECT_0)
                    break;

                if (!SetWaitableTimer(
                        g_timer,
                        &due,
                        0,
                        nullptr,
                        nullptr,
                        FALSE))
                {
                    break;
                }

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
            CloseHandle(g_timer);
            g_timer = nullptr;
        }
        else
        {
            // Compatibility fallback. The process still remains alive
            // and tray-visible; no busy loop is used.
            while (WaitForSingleObject(
                       g_stop,
                       1000) == WAIT_TIMEOUT)
            {
            }
        }

        timeEndPeriod(1);
        return 0;
    }

    // ------------------------------------------------------------
    // Tray
    // ------------------------------------------------------------

    bool AddTray(HWND hwnd)
    {
        ZeroMemory(&g_tray, sizeof(g_tray));

        g_tray.cbSize = sizeof(g_tray);
        g_tray.hWnd = hwnd;
        g_tray.uID = 1;
        g_tray.uFlags =
            NIF_MESSAGE |
            NIF_ICON |
            NIF_TIP;

        g_tray.uCallbackMessage = WM_TRAYICON;

        g_tray.hIcon = LoadIconW(
            nullptr,
            MAKEINTRESOURCEW(IDI_APPLICATION)
        );

        lstrcpynW(
            g_tray.szTip,
            L"TimerFix",
            ARRAYSIZE(g_tray.szTip)
        );

        return Shell_NotifyIconW(
            NIM_ADD,
            &g_tray
        ) != FALSE;
    }

    void RemoveTray()
    {
        Shell_NotifyIconW(
            NIM_DELETE,
            &g_tray
        );
    }

    void ShowMenu(HWND hwnd)
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

        PostMessageW(hwnd, WM_NULL, 0, 0);
    }

    // ------------------------------------------------------------
    // Hidden message window
    // ------------------------------------------------------------

    LRESULT CALLBACK WndProc(
        HWND hwnd,
        UINT msg,
        WPARAM wParam,
        LPARAM lParam)
    {
        switch (msg)
        {
        case WM_TRAYICON:

            if (lParam == WM_RBUTTONUP ||
                lParam == WM_CONTEXTMENU)
            {
                ShowMenu(hwnd);
            }

            return 0;

        case WM_COMMAND:

            if (LOWORD(wParam) == ID_EXIT)
            {
                DestroyWindow(hwnd);
                return 0;
            }

            return 0;

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

            RemoveTray();

            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(
            hwnd,
            msg,
            wParam,
            lParam
        );
    }

    HWND CreateHiddenWindow(HINSTANCE instance)
    {
        constexpr wchar_t CLASS_NAME[] =
            L"TimerFix_Tray_Window";

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = instance;
        wc.lpszClassName = CLASS_NAME;

        if (!RegisterClassExW(&wc))
            return nullptr;

        return CreateWindowExW(
            0,
            CLASS_NAME,
            L"TimerFix",
            0,
            0, 0, 0, 0,
            HWND_MESSAGE,
            nullptr,
            instance,
            nullptr
        );
    }
}

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    PWSTR,
    int)
{
    HANDLE mutex = CreateMutexW(
        nullptr,
        TRUE,
        L"Global\\TimerFix_Unique"
    );

    if (!mutex)
        return 1;

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(mutex);
        return 0;
    }

    // Create the tray FIRST.
    // Therefore a failure in any timer mechanism cannot make
    // the program silently disappear before the tray icon exists.
    g_stop = CreateEventW(
        nullptr,
        TRUE,
        FALSE,
        nullptr
    );

    if (!g_stop)
    {
        CloseHandle(mutex);
        return 1;
    }

    g_hwnd = CreateHiddenWindow(hInstance);

    if (!g_hwnd)
    {
        CloseHandle(g_stop);
        CloseHandle(mutex);
        return 1;
    }

    if (!AddTray(g_hwnd))
    {
        DestroyWindow(g_hwnd);
        CloseHandle(g_stop);
        CloseHandle(mutex);
        return 1;
    }

    g_worker = CreateThread(
        nullptr,
        0,
        Worker,
        nullptr,
        0,
        nullptr
    );

    // If the worker cannot start, the tray still works.
    // Exit remains available and the program does not vanish.
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

    if (g_worker)
    {
        WaitForSingleObject(
            g_worker,
            INFINITE
        );

        CloseHandle(g_worker);
    }

    CloseHandle(g_stop);
    CloseHandle(mutex);

    return 0;
}
