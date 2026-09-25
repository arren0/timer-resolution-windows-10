#include 
#include 

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 1001

// Definizione per la funzione non documentata di ntdll.dll
typedef NTSTATUS(CALLBACK* PNTSETTIMERRESOLUTION)(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);
ULONG currentRes = 0;

// Genera un'icona quadrata rossa in memoria
HICON CreateRedIcon() {
    int width = GetSystemMetrics(SM_CXSMICON);
    int height = GetSystemMetrics(SM_CYSMICON);
    HDC hdc = GetDC(NULL);
    HDC hMemDC = CreateCompatibleDC(hdc);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdc, width, height);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hMemDC, hBitmap);
    
    HBRUSH hBrush = CreateSolidBrush(RGB(255, 0, 0));
    RECT rect = { 0, 0, width, height };
    FillRect(hMemDC, &rect, hBrush);
    
    SelectObject(hMemDC, hOldBitmap);
    DeleteObject(hBrush);
    DeleteDC(hMemDC);
    ReleaseDC(NULL, hdc);

    ICONINFO ii = { 0 };
    ii.fIcon = TRUE;
    ii.hbmColor = hBitmap;
    ii.hbmMask = hBitmap;
    
    HICON hIcon = CreateIconIndirect(&ii);
    DeleteObject(hBitmap);
    return hIcon;
}

// Gestore dei messaggi della finestra nascosta
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_TRAYICON) {
        if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            InsertMenu(hMenu, 0, MF_BYPOSITION | MF_STRING, ID_TRAY_EXIT, L"Exit");
            
            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
        }
    }
    else if (uMsg == WM_COMMAND && LOWORD(wParam) == ID_TRAY_EXIT) {
        PostQuitMessage(0);
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // 1. Forza la timer resolution a 1ms (10000 * 100ns)
    HMODULE hNtdll = GetModuleHandle(L"ntdll.dll");
    PNTSETTIMERRESOLUTION NtSetTimerResolution = NULL;
    if (hNtdll) {
        NtSetTimerResolution = (PNTSETTIMERRESOLUTION)GetProcAddress(hNtdll, "NtSetTimerResolution");
        if (NtSetTimerResolution) {
            NtSetTimerResolution(10000, TRUE, &currentRes);
        }
    }

    // 2. Registra e crea la finestra message-only
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"TimerResFixClass";
    RegisterClass(&wc);
    HWND hwnd = CreateWindow(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);

    // 3. Aggiunge l'icona rossa alla System Tray
    NOTIFYICONDATA nid = { 0 };
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = CreateRedIcon();
    lstrcpyW(nid.szTip, L"Timer Resolution: 1ms\n(Click destro per uscire)");
    Shell_NotifyIcon(NIM_ADD, &nid);

    // 4. Loop dei messaggi
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 5. Cleanup e ripristino della risoluzione originale
    Shell_NotifyIcon(NIM_DELETE, &nid);
    DestroyIcon(nid.hIcon);
    if (NtSetTimerResolution) {
        NtSetTimerResolution(currentRes, FALSE, &currentRes);
    }

    return (int)msg.wParam;
}
