#include <windows.h>

// Definizioni per disabilitare il Power Throttling di Windows 10
#define ProcessPowerThrottling (PROCESS_INFORMATION_CLASS)4
struct THROTTLING_STATE { ULONG Version; ULONG ControlMask; ULONG StateMask; };

LRESULT CALLBACK W(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == 1025 && (lp == 514 || lp == 516)) {
        POINT p; GetCursorPos(&p);
        HMENU menu = CreatePopupMenu(); 
        InsertMenuA(menu, 0, 0, 1, "Exit");
        SetForegroundWindow(h);
        if (TrackPopupMenu(menu, 256, p.x, p.y, 0, h, 0)) ExitProcess(0);
    }
    return DefWindowProc(h, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE I, HINSTANCE, LPSTR, int) {
    // 1. DISABILITA L'ISOLAMENTO DEL TIMER (Il trucco chiave per Win10)
    THROTTLING_STATE pts = {1, 1, 0};
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pts, sizeof(pts));
    
    // 2. Priorità alta e impedimento sleep di sistema
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadExecutionState(0x80000001); // ES_CONTINUOUS | ES_SYSTEM_REQUIRED

    // 3. Imposta Risoluzione a 0.5ms
    ULONG r;
    ((LONG(__stdcall*)(ULONG,BOOLEAN,PULONG))GetProcAddress(GetModuleHandleA("ntdll"), "NtSetTimerResolution"))(5000, 1, &r);

    // 4. UI: Quadratino Rosso
    WNDCLASSA c = {0}; c.lpfnWndProc = W; c.lpszClassName = "T"; RegisterClassA(&c);
    HWND h = CreateWindowA("T", 0,0,0,0,0,0,0,0,I,0);

    HDC d = GetDC(0), md = CreateCompatibleDC(d);
    HBITMAP cb = CreateCompatibleBitmap(d, 16, 16), mb = CreateBitmap(16, 16, 1, 1, 0);
    SelectObject(md, cb); RECT rc = {0,0,16,16}; 
    FillRect(md, &rc, CreateSolidBrush(255));
    ICONINFO ii = {1, 0, 0, mb, cb};

    NOTIFYICONDATAA n = {sizeof(n), h, 1, 7, 1025, CreateIconIndirect(&ii)};
    Shell_NotifyIconA(0, &n);

    // 5. Loop messaggi
    MSG msg; while (GetMessage(&msg, 0, 0, 0)) DispatchMessage(&msg);
}
