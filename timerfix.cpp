#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

// ------------------------------------------------------------
// Configurazione
// ------------------------------------------------------------

static constexpr UINT TIMER_RESOLUTION_MS = 1;

// Intervallo del wake-up.
// 1 ms = comportamento più aggressivo.
// 2-4 ms = molto più leggero.
// 1 ms è quello da provare se il problema è proprio il delta.
static constexpr LONG PERIOD_MS = 1;

static constexpr UINT WM_TRAYICON = WM_APP + 1;

static constexpr UINT ID_EXIT = 1001;
static constexpr UINT ID_ENABLE = 1002;

// ------------------------------------------------------------
// Stato globale
// ------------------------------------------------------------

static HWND      g_hwnd = nullptr;
static HANDLE    g_timer = nullptr;
static HANDLE    g_thread = nullptr;
static HICON     g_icon = nullptr;

static volatile LONG g_running = 1;
static volatile LONG g_enabled = 1;

static NOTIFYICONDATAW g_nid{};

// ------------------------------------------------------------
// Timer resolution
// ------------------------------------------------------------

static bool EnableTimerResolution()
{
    return timeBeginPeriod(TIMER_RESOLUTION_MS) == TIMERR_NOERROR;
}

static void DisableTimerResolution()
{
    timeEndPeriod(TIMER_RESOLUTION_MS);
}

// ------------------------------------------------------------
// High resolution timer
// ------------------------------------------------------------

static bool CreateHighResolutionTimer()
{
    // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION è disponibile
    // nelle versioni moderne di Windows 10.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

    g_timer = CreateWaitableTimerExW(
        nullptr,
        nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS
    );

    if (!g_timer)
    {
        // Fallback a waitable timer normale.
        g_timer = CreateWaitableTimerW(
            nullptr,
            FALSE,
            nullptr
        );
    }

    return g_timer != nullptr;
}

// ------------------------------------------------------------
// Thread timer
// ------------------------------------------------------------

static DWORD WINAPI TimerThread(LPVOID)
{
    // Priorità leggermente sopra il normale.
    // Non usiamo REALTIME_PRIORITY_CLASS.
    SetThreadPriority(
        GetCurrentThread(),
        THREAD_PRIORITY_ABOVE_NORMAL
    );

    LARGE_INTEGER dueTime{};

    // Primo evento tra PERIOD_MS.
    //
    // FILETIME/NT timeout:
    // negativo = relativo
    // unità = 100 ns
    dueTime.QuadPart =
        -static_cast<LONGLONG>(PERIOD_MS) * 10000LL;

    if (!SetWaitableTimer(
        g_timer,
        &dueTime,
        PERIOD_MS,
        nullptr,
        nullptr,
        FALSE))
    {
        return 0;
    }

    while (InterlockedCompareExchange(
        &g_running,
        1,
        1) != 0)
    {
        if (InterlockedCompareExchange(
            &g_enabled,
            1,
            1) == 0)
        {
            Sleep(50);
            continue;
        }

        DWORD result = WaitForSingleObject(
            g_timer,
            INFINITE
        );

        if (result != WAIT_OBJECT_0)
            break;

        // ----------------------------------------------------
        // Wake-up intenzionalmente vuoto.
        //
        // Lo scopo non è fare lavoro:
        // vogliamo solamente mantenere una sorgente
        // periodica di timer ad alta risoluzione.
        // ----------------------------------------------------

        YieldProcessor();
    }

    CancelWaitableTimer(g_timer);

    return 0;
}

// ------------------------------------------------------------
// Tray icon
// ------------------------------------------------------------

static void UpdateTrayTooltip()
{
    if (!g_hwnd)
        return;

    if (InterlockedCompareExchange(
        &g_enabled,
        1,
        1) != 0)
    {
        wcscpy_s(
            g_nid.szTip,
            L"TimerFix - ON"
        );
    }
    else
    {
        wcscpy_s(
            g_nid.szTip,
            L"TimerFix - OFF"
        );
    }

    Shell_NotifyIconW(
        NIM_MODIFY,
        &g_nid
    );
}

static void AddTrayIcon(HWND hwnd)
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

    g_nid.hIcon = g_icon;

    wcscpy_s(
        g_nid.szTip,
        L"TimerFix - ON"
    );

    Shell_NotifyIconW(
        NIM_ADD,
        &g_nid
    );

    // Windows moderno può richiedere versione 4.
    g_nid.uVersion = NOTIFYICON_VERSION_4;

    Shell_NotifyIconW(
        NIM_SETVERSION,
        &g_nid
    );
}

static void RemoveTrayIcon()
{
    Shell_NotifyIconW(
        NIM_DELETE,
        &g_nid
    );
}

// ------------------------------------------------------------
// Menu tray
// ------------------------------------------------------------

static void ShowTrayMenu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();

    if (!menu)
        return;

    bool enabled =
        InterlockedCompareExchange(
            &g_enabled,
            1,
            1
        ) != 0;

    AppendMenuW(
        menu,
        MF_STRING | (enabled ? MF_CHECKED : 0),
        ID_ENABLE,
        L"Timer fix attivo"
    );

    AppendMenuW(
        menu,
        MF_SEPARATOR,
        0,
        nullptr
    );

    AppendMenuW(
        menu,
        MF_STRING,
        ID_EXIT,
        L"Esci"
    );

    POINT pt{};
    GetCursorPos(&pt);

    // Necessario per far funzionare correttamente il menu
    // della tray quando viene aperto con il mouse.
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

    PostMessageW(
        hwnd,
        WM_NULL,
        0,
        0
    );

    DestroyMenu(menu);
}

// ------------------------------------------------------------
// Window procedure
// ------------------------------------------------------------

static LRESULT CALLBACK WndProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (msg)
    {
        case WM_TRAYICON:
        {
            if (lParam == WM_RBUTTONUP)
            {
                ShowTrayMenu(hwnd);
            }
            else if (lParam == WM_LBUTTONDBLCLK)
            {
                bool enabled =
                    InterlockedCompareExchange(
                        &g_enabled,
                        1,
                        1
                    ) != 0;

                InterlockedExchange(
                    &g_enabled,
                    enabled ? 0 : 1
                );

                UpdateTrayTooltip();
            }

            return 0;
        }

        case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
                case ID_ENABLE:
                {
                    bool enabled =
                        InterlockedCompareExchange(
                            &g_enabled,
                            1,
                            1
                        ) != 0;

                    InterlockedExchange(
                        &g_enabled,
                        enabled ? 0 : 1
                    );

                    UpdateTrayTooltip();

                    return 0;
                }

                case ID_EXIT:
                {
                    InterlockedExchange(
                        &g_running,
                        0
                    );

                    PostQuitMessage(0);

                    return 0;
                }
            }

            break;
        }

        case WM_DESTROY:
        {
            InterlockedExchange(
                &g_running,
                0
            );

            PostQuitMessage(0);

            return 0;
        }
    }

    return DefWindowProcW(
        hwnd,
        msg,
        wParam,
        lParam
    );
}

// ------------------------------------------------------------
// WinMain
// ------------------------------------------------------------

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    PWSTR,
    int)
{
    // --------------------------------------------------------
    // Classe finestra nascosta
    // --------------------------------------------------------

    const wchar_t CLASS_NAME[] =
        L"TimerFixHiddenWindow";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = hInstance;
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = CLASS_NAME;

    wc.hIcon = LoadIconW(
        nullptr,
        IDI_APPLICATION
    );

    wc.hCursor = LoadCursorW(
        nullptr,
        IDC_ARROW
    );

    if (!RegisterClassExW(&wc))
        return 1;

    g_icon = LoadIconW(
        nullptr,
        IDI_APPLICATION
    );

    // --------------------------------------------------------
    // Finestra completamente nascosta
    // --------------------------------------------------------

    g_hwnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"TimerFix",
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!g_hwnd)
        return 1;

    // --------------------------------------------------------
    // Timer resolution
    // --------------------------------------------------------

    bool timerResolutionOK =
        EnableTimerResolution();

    // Anche se timeBeginPeriod fallisce,
    // proviamo comunque il timer ad alta risoluzione.
    (void)timerResolutionOK;

    // --------------------------------------------------------
    // Waitable timer
    // --------------------------------------------------------

    if (!CreateHighResolutionTimer())
    {
        DisableTimerResolution();
        DestroyWindow(g_hwnd);
        return 1;
    }

    // --------------------------------------------------------
    // Thread
    // --------------------------------------------------------

    g_thread = CreateThread(
        nullptr,
        0,
        TimerThread,
        nullptr,
        0,
        nullptr
    );

    if (!g_thread)
    {
        CloseHandle(g_timer);
        g_timer = nullptr;

        DisableTimerResolution();

        DestroyWindow(g_hwnd);

        return 1;
    }

    // --------------------------------------------------------
    // Tray
    // --------------------------------------------------------

    AddTrayIcon(g_hwnd);

    // --------------------------------------------------------
    // Message loop
    // --------------------------------------------------------

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

    // --------------------------------------------------------
    // Shutdown
    // --------------------------------------------------------

    InterlockedExchange(
        &g_running,
        0
    );

    if (g_timer)
    {
        CancelWaitableTimer(g_timer);

        // Sblocca eventualmente il thread.
        SetEvent(g_timer);
    }

    if (g_thread)
    {
        WaitForSingleObject(
            g_thread,
            2000
        );

        CloseHandle(g_thread);
        g_thread = nullptr;
    }

    if (g_timer)
    {
        CloseHandle(g_timer);
        g_timer = nullptr;
    }

    RemoveTrayIcon();

    DisableTimerResolution();

    return 0;
}
