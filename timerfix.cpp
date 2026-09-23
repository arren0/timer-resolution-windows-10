#define _WIN32_WINNT 0x0602
#include <windows.h>
#include <shellapi.h>
 
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
 
// Flag indispensabile per scavalcare il risparmio energetico di Windows 10 2004+ in user-mode
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
 
#define WM_TRAYICON (WM_USER + 1)
#define ID_EXIT 1001
 
typedef LONG(NTAPI* NtSetTimerResolution)(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);
ULONG currentRes;
HANDLE hWaitTimer = NULL;
bool keepRunning = true;
 
// Thread ad altissima priorità che costringe il Kernel globale a tenere il delta basso
DWORD WINAPI ForceGlobalDeltaThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
 
    hWaitTimer = CreateWaitableTimerEx(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!hWaitTimer) return 0;
 
    LARGE_INTEGER dueTime;
    dueTime.QuadPart = -5000; // 0.5 ms
    // Costringe l'OS a interrompere ogni 1ms con timer ad alta risoluzione
    SetWaitableTimer(hWaitTimer, &dueTime, 1, NULL, NULL, FALSE);
 
    while (keepRunning) {
        if (WaitForSingleObject(hWaitTimer, INFINITE) != WAIT_OBJECT_0) break;
    }
 
    CloseHandle(hWaitTimer);
    return 0;
}
 
// Disegna un quadrato rosso 16x16 in RAM
HICON CreateRedSquareIcon() {
    ICONINFO ii = { 0 };
    ii.fIcon = TRUE;
    BYTE andMask[32] = { 0 };
    ii.hbmMask = CreateBitmap(16, 16, 1, 1, andMask);
    HDC hdc = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP hbmColor = CreateCompatibleBitmap(hdc, 16, 16);
    SelectObject(memDC, hbmColor);
    HBRUSH brush = CreateSolidBrush(RGB(255, 0, 0));
    RECT r = { 0, 0, 16, 16 };
    FillRect(memDC, &r, brush);
    DeleteObject(brush);
    DeleteDC(memDC);
    ReleaseDC(NULL, hdc);
    ii.hbmColor = hbmColor;
    HICON hIcon = CreateIconIndirect(&ii);
    DeleteObject(hbmColor);
    DeleteObject(ii.hbmMask);
    return hIcon;
}
 
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TRAYICON) {
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            InsertMenu(hMenu, 0, MF_BYPOSITION | MF_STRING, ID_EXIT, "Exit");
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
            if (cmd == ID_EXIT) PostMessage(hwnd, WM_CLOSE, 0, 0);
        }
    } else if (msg == WM_DESTROY) {
        keepRunning = false;
        if (hWaitTimer) CancelWaitableTimer(hWaitTimer);
 
        NOTIFYICONDATA nid = { sizeof(nid), hwnd, 1 };
        Shell_NotifyIcon(NIM_DELETE, &nid);
 
        HMODULE hNtdll = GetModuleHandle("ntdll.dll");
        if (hNtdll) {
            NtSetTimerResolution pNtSetTimerResolution = (NtSetTimerResolution)GetProcAddress(hNtdll, "NtSetTimerResolution");
            if (pNtSetTimerResolution) pNtSetTimerResolution(currentRes, FALSE, &currentRes);
        }
        PostQuitMessage(0);
    } else {
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}
 
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    // 1. Richiesta Risoluzione
    HMODULE hNtdll = GetModuleHandle("ntdll.dll");
    if (hNtdll) {
        NtSetTimerResolution pNtSetTimerResolution = (NtSetTimerResolution)GetProcAddress(hNtdll, "NtSetTimerResolution");
        if (pNtSetTimerResolution) pNtSetTimerResolution(5000, TRUE, &currentRes); // 0.5ms
    }
 
    // 2. Forza applicazione globale del delta
    CreateThread(NULL, 0, ForceGlobalDeltaThread, NULL, 0, NULL);
 
    // 3. Setup Finestra Invisibile e Icona Tray
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "GlobalDeltaFix";
    RegisterClass(&wc);
    HWND hwnd = CreateWindow("GlobalDeltaFix", NULL, 0, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
 
    NOTIFYICONDATA nid = { sizeof(nid), hwnd, 1, NIF_ICON | NIF_MESSAGE | NIF_TIP, WM_TRAYICON };
    nid.hIcon = CreateRedSquareIcon();
    lstrcpy(nid.szTip, "Timer Delta Fix (0.5ms)");
    Shell_NotifyIcon(NIM_ADD, &nid);
    DestroyIcon(nid.hIcon);
 
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
