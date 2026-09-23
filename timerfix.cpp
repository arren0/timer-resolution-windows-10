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
    constexpr UINT TIMER_PERIOD_MS = 1;

    HWND g_hwnd = nullptr;
    NOTIFYICONDATAW g_nid{};
    bool g_timerActive = false;

    bool EnableTimerResolution()
    {
        if (g_timerActive)
            return true;

        if (timeBeginPeriod(TIMER_PERIOD_MS) == TIMERR_NOERROR)
        {
            g_timerActive = true;
            return true;
        }

        return false;
    }

    void DisableTimerResolution()
    {
        if (!g_timerActive)
            return;

        timeEndPeriod(TIMER_PERIOD_MS);
        g_timerActive = false;
    }

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

        // Explicitly use the Unicode resource identifier.
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

        POINT point{};
        GetCursorPos(&point);

        SetForegroundWindow(hwnd);

        TrackPopupMenu(
            menu,
            TPM_RIGHTBUTTON |
            TPM_BOTTOMALIGN |
            TPM_LEFTALIGN,
            point.x,
            point.y,
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

            RemoveTrayIcon();
            DisableTimerResolution();

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
}

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    PWSTR,
    int)
{
    // Prevent multiple instances.
    HANDLE mutex = CreateMutexW(
        nullptr,
        TRUE,
        L"Global\\TimerFix_SingleInstance"
    );

    if (!mutex)
        return 1;

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(mutex);
        return 0;
    }

    g_hwnd = CreateHiddenWindow(hInstance);

    if (!g_hwnd)
    {
        CloseHandle(mutex);
        return 1;
    }

    if (!AddTrayIcon(g_hwnd))
    {
        DestroyWindow(g_hwnd);
        CloseHandle(mutex);
        return 1;
    }

    // Request 1 ms timer resolution.
    EnableTimerResolution();

    MSG message{};

    while (GetMessageW(
        &message,
        nullptr,
        0,
        0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    DisableTimerResolution();

    CloseHandle(mutex);

    return 0;
}
