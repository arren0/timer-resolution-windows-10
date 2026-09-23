#include <windows.h>
#include <shellapi.h>
 
#define WM_TRAYICON (WM_USER + 1)
#define ID_EXIT 1001
 
// Funzione non documentata di Windows per forzare il Global Timer Resolution
typedef LONG(NTAPI* NtSetTimerResolution)(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);
ULONG currentRes;
 
// Crea dinamicamente un "quadratino rosso" senza usare file .ico esterni
HICON CreateRedSquareIcon() {
    ICONINFO ii = { 0 };
    ii.fIcon = TRUE;
 
    // Maschera trasparenza (tutto nero = opaco)
    BYTE andMask[32] = { 0 };
    ii.hbmMask = CreateBitmap(16, 16, 1, 1, andMask);
 
    HDC hdc = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP hbmColor = CreateCompatibleBitmap(hdc, 16, 16);
    SelectObject(memDC, hbmColor);
 
    // Disegna il quadrato rosso
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
 
        // Ripristina il timer originale in chiusura
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
    // Carica ntdll.dll e forza il timer a 5000 (0.5 ms)
    HMODULE hNtdll = GetModuleHandle("ntdll.dll");
    if (hNtdll) {
        NtSetTimerResolution pNtSetTimerResolution = (NtSetTimerResolution)GetProcAddress(hNtdll, "NtSetTimerResolution");
        // 5000 = 0.5ms (valore minimo stabile su Windows 10)
        if (pNtSetTimerResolution) pNtSetTimerResolution(5000, TRUE, &currentRes);
    }
 
    // Registra finestra invisibile
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "TimerFixWnd";
    RegisterClass(&wc);
    HWND hwnd = CreateWindow("TimerFixWnd", NULL, 0, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
 
    // Configura Tray Icon
    NOTIFYICONDATA nid = { sizeof(nid), hwnd, 1, NIF_ICON | NIF_MESSAGE | NIF_TIP, WM_TRAYICON };
    nid.hIcon = CreateRedSquareIcon();
    lstrcpy(nid.szTip, "Timer Resolution Fix (0.5ms)");
    Shell_NotifyIcon(NIM_ADD, &nid);
    DestroyIcon(nid.hIcon); // Pulizia, ora gestito dalla tray
 
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
