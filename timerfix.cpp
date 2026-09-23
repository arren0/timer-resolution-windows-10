#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

LRESULT CALLBACK W(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == 1025 && (lp == 514 || lp == 516)) {
        POINT p; GetCursorPos(&p);
        HMENU menu = CreatePopupMenu(); InsertMenuA(menu, 0, 0, 1, "Exit");
        SetForegroundWindow(h);
        if (TrackPopupMenu(menu, 256, p.x, p.y, 0, h, 0)) ExitProcess(0);
    }
    return DefWindowProc(h, m, wp, lp);
}

// Il trucco di DPC Latency Checker: forzare la CPU a non andare in deep idle
DWORD WINAPI AntiIdleThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    timeBeginPeriod(1);
    while (true) Sleep(1);
    return 0;
}

int WINAPI WinMain(HINSTANCE I, HINSTANCE, LPSTR, int) {
    ULONG r;
    // 1. Imposta la Timer Resolution a 0.5ms (5000 100-ns)
    ((LONG(__stdcall*)(ULONG,BOOLEAN,PULONG))GetProcAddress(GetModuleHandleA("ntdll"), "NtSetTimerResolution"))(5000, 1, &r);
    
    // 2. Lancia il polling continuo per globalizzare il delta
    CreateThread(0, 0, AntiIdleThread, 0, 0, 0);

    // 3. Crea Finestra nascosta e Icona
    WNDCLASSA c = {0}; c.lpfnWndProc = W; c.lpszClassName = "T"; RegisterClassA(&c);
    HWND h = CreateWindowA("T", 0,0,0,0,0,0,0,0,I,0);

    HDC d = GetDC(0), md = CreateCompatibleDC(d);
    HBITMAP cb = CreateCompatibleBitmap(d, 16, 16), mb = CreateBitmap(16, 16, 1, 1, 0);
    SelectObject(md, cb); RECT rc = {0,0,16,16};
    FillRect(md, &rc, CreateSolidBrush(255));
    ICONINFO ii = {1, 0, 0, mb, cb};

    NOTIFYICONDATAA n = {sizeof(n), h, 1, 7, 1025, CreateIconIndirect(&ii)};
    Shell_NotifyIconA(0, &n);

    MSG msg; while (GetMessage(&msg, 0, 0, 0)) DispatchMessage(&msg);
}
