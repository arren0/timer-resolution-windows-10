#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

LRESULT CALLBACK W(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == 1025 && (lp == 514 || lp == 516)) { // Click tray icon
        POINT p; GetCursorPos(&p);
        HMENU menu = CreatePopupMenu(); InsertMenuA(menu, 0, 0, 1, "Exit");
        SetForegroundWindow(h);
        if (TrackPopupMenu(menu, 256, p.x, p.y, 0, h, 0)) ExitProcess(0);
    }
    return DefWindowProc(h, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE I, HINSTANCE, LPSTR, int) {
    ULONG r; HWAVEOUT w; WAVEFORMATEX f = {1, 1, 8000, 8000, 1, 8, 0};
    

    ((LONG(__stdcall*)(ULONG,BOOLEAN,PULONG))GetProcAddress(GetModuleHandleA("ntdll"), "NtSetTimerResolution"))(5000, 1, &r);
    waveOutOpen(&w, (UINT_PTR)-1, &f, 0, 0, 0);

 
    WNDCLASSA c = {0}; c.lpfnWndProc = W; c.lpszClassName = "T"; RegisterClassA(&c);
    HWND h = CreateWindowA("T", 0,0,0,0,0,0,0,0,I,0);


    HDC d = GetDC(0), md = CreateCompatibleDC(d);
    HBITMAP cb = CreateCompatibleBitmap(d, 16, 16), mb = CreateBitmap(16, 16, 1, 1, 0);
    SelectObject(md, cb); RECT rc = {0,0,16,16};
    FillRect(md, &rc, CreateSolidBrush(255)); // 255 = Rosso
    ICONINFO ii = {1, 0, 0, mb, cb};

    NOTIFYICONDATAA n = {sizeof(n), h, 1, 7, 1025, CreateIconIndirect(&ii)};
    Shell_NotifyIconA(0, &n);

    MSG msg; while (GetMessage(&msg, 0, 0, 0)) DispatchMessage(&msg);
}
