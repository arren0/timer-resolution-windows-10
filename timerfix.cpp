#include <windows.h>
#include <shellapi.h>
#include <mmsystem.h>
 
#pragma comment(lib, "winmm.lib")
 
#define WM_TRAYICON (WM_USER + 1)
#define ID_EXIT 1001
 
typedef LONG(NTAPI* NtSetTimerResolution)(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);
ULONG currentRes;
MMRESULT timerID = 0;
 
void CALLBACK DummyTimerProc(UINT uTimerID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dw1, DWORD_PTR dw2) {
    // Callback vuoto: serve solo a forzare l'OS ad abbassare il timer delta globale
}
 
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
            if (cmd == ID_EXIT) {
                PostMessage(hwnd, WM_CLOSE, 0, 0);
            }
        }
    } else if (msg == WM_DESTROY) {
        NOTIFYICONDATA nid = { sizeof(nid), hwnd, 1 };
        Shell_NotifyIcon(NIM_DELETE, &nid);
 
        // Pulizia timer
        if (timerID) timeKillEvent(timerID);
        timeEndPeriod(1);
 
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
    HMODULE hNtdll = GetModuleHandle("ntdll.dll");
    if (hNtdll) {
        NtSetTimerResolution pNtSetTimerResolution = (NtSetTimerResolution)GetProcAddress(hNtdll, "NtSetTimerResolution");
        if (pNtSetTimerResolution) pNtSetTimerResolution(5000, TRUE, &currentRes);
    }
 
    // Forza il timer delta globale attivando un timer multimediale continuo
    timeBeginPeriod(1);
    timerID = timeSetEvent(1, 1, DummyTimerProc, 0, TIME_PERIODIC);
 
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "TimerFixWnd";
    RegisterClass(&wc);
    HWND hwnd = CreateWindow("TimerFixWnd", NULL, 0, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
 
    NOTIFYICONDATA nid = { sizeof(nid), hwnd, 1, NIF_ICON | NIF_MESSAGE | NIF_TIP, WM_TRAYICON };
    nid.hIcon = CreateRedSquareIcon();
    lstrcpy(nid.szTip, "Timer Delta Fix (Active)");
    Shell_NotifyIcon(NIM_ADD, &nid);
    DestroyIcon(nid.hIcon);
 
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
