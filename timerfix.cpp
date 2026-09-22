// timerfix.cpp
//
// Ultra-lightweight background utility for Windows 10 that fixes the
// broken "delta timer" behavior by forcing the system timer to its
// finest available resolution (typically 0.5ms instead of the default
// ~15.6ms), exactly the same side effect that keeping DPC Latency
// Checker open has. Sits in the tray as a plain colored square with a
// single "Exit" option. No console, no dialogs, ~0% CPU, a few MB RAM.
//
// Build (MinGW-w64, one line, no resource compiler needed):
//   g++ -O2 -s -static -mwindows -o TimerFix.exe timerfix.cpp -lshell32 -luser32 -lgdi32
//
// Build (MSVC, from a "Developer Command Prompt"):
//   cl /O2 /EHsc timerfix.cpp /link /SUBSYSTEM:WINDOWS shell32.lib user32.lib gdi32.lib /OUT:TimerFix.exe
//
// Runs fine unelevated (no admin rights required). Works on Win 10 and 11.

#include <windows.h>
#include <shellapi.h>
#include <avrt.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "avrt.lib")

// ---- Undocumented NTDLL timer resolution API -------------------------
// Same functions DPC Latency Checker / ClockRes / "Timer Resolution"
// tools use. Resolution values are in 100-nanosecond units.
typedef LONG(NTAPI* NtSetTimerResolution_t)(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);
typedef LONG(NTAPI* NtQueryTimerResolution_t)(PULONG MinimumResolution, PULONG MaximumResolution, PULONG CurrentResolution);

static NtSetTimerResolution_t   pNtSetTimerResolution   = nullptr;
static NtQueryTimerResolution_t pNtQueryTimerResolution  = nullptr;

static const wchar_t* kWindowClass = L"TimerFixTrayWndClass";
static const wchar_t* kMutexName   = L"Local\\TimerFix_SingleInstance_Mutex";
#define WM_TRAYICON   (WM_APP + 1)
#define ID_TRAY_EXIT  1001

static NOTIFYICONDATAW g_nid = {};
static ULONG g_appliedResolution = 0; // 100ns units, what we actually set
static bool  g_resolutionActive  = false;
static bool  g_winmmPeriodActive = false; // timeBeginPeriod(1) held
static HANDLE g_mmcssHandle = nullptr;    // MMCSS registration

// Build a small solid-color square icon at runtime so we don't need a
// .ico resource file or a resource compiler.
static HICON CreateSquareIcon(COLORREF fill, int size = 16)
{
    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, color);

    RECT r = { 0, 0, size, size };
    HBRUSH brush = CreateSolidBrush(fill);
    FillRect(memDC, &r, brush);
    DeleteObject(brush);

    // small border so it reads clearly at tray size
    HBRUSH border = CreateSolidBrush(RGB(20, 20, 20));
    FrameRect(memDC, &r, border);
    DeleteObject(border);

    SelectObject(memDC, oldBmp);

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr); // fully opaque mask

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;

    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(color);
    DeleteObject(mask);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    return icon;
}

static void AddTrayIcon(HWND hwnd, bool active)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = CreateSquareIcon(active ? RGB(60, 200, 90) : RGB(200, 60, 60));

    wchar_t tip[128];
    if (active && g_appliedResolution > 0) {
        double ms = g_appliedResolution / 10000.0;
        wsprintfW(tip, L"Timer Resolution Fix - active (%.2f ms)", ms);
    } else {
        lstrcpyW(tip, L"Timer Resolution Fix - inactive");
    }
    lstrcpyW(g_nid.szTip, tip);

    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon()
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_nid.hIcon) DestroyIcon(g_nid.hIcon);
}

static bool ApplyFinestTimerResolution()
{
    if (!pNtSetTimerResolution || !pNtQueryTimerResolution)
        return false;

    ULONG minRes = 0, maxRes = 0, curRes = 0; // 100ns units
    // Note: "MaximumResolution" is the SMALLEST number = the FINEST
    // (highest precision) timer interval the system supports.
    if (pNtQueryTimerResolution(&minRes, &maxRes, &curRes) != 0)
        return false;

    ULONG desired = maxRes; // finest available, typically 5000 (0.5ms)
    ULONG achieved = 0;
    LONG status = pNtSetTimerResolution(desired, TRUE, &achieved);
    if (status != 0)
        return false;

    g_appliedResolution = achieved;
    g_resolutionActive = true;

    // Also hold the request through the documented winmm API. Both APIs
    // move the same underlying interrupt interval, but some undocumented
    // scheduler heuristics for whether other processes' Sleep() calls get
    // to benefit from it appear to key off which API/path was used to ask
    // - so we hold both rather than relying on NtSetTimerResolution alone.
    UINT periodMs = (UINT)(achieved / 10000); // 100ns units -> ms
    if (periodMs < 1) periodMs = 1;
    if (timeBeginPeriod(periodMs) == TIMERR_NOERROR)
        g_winmmPeriodActive = true;

    // Register this thread with the Multimedia Class Scheduler Service as
    // doing latency-sensitive work. This is the officially sanctioned way
    // for a real-time-ish process to tell the scheduler "treat my timing
    // requests as important" - the same category real-time audio/capture
    // tools (which is the kind of tool DPC Latency Checker is) register
    // under, and Microsoft's own docs note MMCSS scheduling decisions can
    // depend on factors like foreground status.
    DWORD taskIndex = 0;
    g_mmcssHandle = AvSetMmThreadCharacteristicsW(L"Games", &taskIndex);

    return true;
}

static void ReleaseTimerResolution()
{
    if (g_mmcssHandle)
    {
        AvRevertMmThreadCharacteristics(g_mmcssHandle);
        g_mmcssHandle = nullptr;
    }
    if (g_winmmPeriodActive)
    {
        UINT periodMs = (UINT)(g_appliedResolution / 10000);
        if (periodMs < 1) periodMs = 1;
        timeEndPeriod(periodMs);
        g_winmmPeriodActive = false;
    }
    if (!g_resolutionActive || !pNtSetTimerResolution)
        return;
    ULONG achieved = 0;
    pNtSetTimerResolution(g_appliedResolution, FALSE, &achieved);
    g_resolutionActive = false;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP)
        {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");
            SetForegroundWindow(hwnd); // required so the menu closes properly
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_TRAY_EXIT)
        {
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_DESTROY:
        ReleaseTimerResolution();
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    // Single instance only.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    // Resolve the undocumented ntdll entry points.
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll)
    {
        pNtSetTimerResolution = (NtSetTimerResolution_t)GetProcAddress(ntdll, "NtSetTimerResolution");
        pNtQueryTimerResolution = (NtQueryTimerResolution_t)GetProcAddress(ntdll, "NtQueryTimerResolution");
    }

    bool ok = ApplyFinestTimerResolution();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    // A real (not message-only) top-level window, kept out of the taskbar
    // and alt-tab via WS_EX_TOOLWINDOW and never shown. Some of Windows'
    // undocumented scheduling heuristics for timer-resolution propagation
    // appear sensitive to whether the requesting process is a genuine
    // window-owning application versus a pure background/message-only
    // process, so we use a real window here rather than HWND_MESSAGE.
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"TimerFix",
                                 WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);

    AddTrayIcon(hwnd, ok);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return (int)msg.wParam;
}
