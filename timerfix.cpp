#include 
#include 
#include 

// Istruisce il linker di MSVC a includere la libreria necessaria per timeBeginPeriod
#pragma comment(lib, "winmm.lib")

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAYICON 1
#define ID_EXIT 1001

// Genera un'icona quadrata rossa in memoria per evitare dipendenze esterne
HICON CreateRedIcon() {
    int width = 32, height = 32;
    HDC hdc = GetDC(NULL);
    HDC hMemDC = CreateCompatibleDC(hdc);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdc, width, height);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hMemDC, hBitmap);

    // Disegna il quadrato rosso
    HBRUSH hBrush = CreateSolidBrush(RGB(255, 0, 0));
    RECT rect = { 0, 0, width, height };
    FillRect(hMemDC, &rect, hBrush);

    SelectObject(hMemDC, hOldBitmap);
    DeleteObject(hBrush);
    DeleteDC(hMemDC);
    ReleaseDC(NULL, hdc);

    // Crea la maschera di trasparenza (tutto opaco)
    HBITMAP hMask = CreateBitmap(width, height, 1, 1, NULL);
    
    ICONINFO ii = {0};
    ii.fIcon = TRUE;
    ii.hbmMask = hMask;
    ii.hbmColor = hBitmap;

    HICON hIcon = CreateIconIndirect(&ii);
    
    // Pulizia
    DeleteObject(hBitmap);
    DeleteObject(hMask);
    return hIcon;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    static NOTIFYICONDATA nid = {0};

    switch (uMsg) {
        case WM_CREATE: {
            // Forza la timer resolution a 1ms
            timeBeginPeriod(1);

            // Inizializza l'icona nella System Tray
            nid.cbSize = sizeof(NOTIFYICONDATA);
            nid.hWnd = hwnd;
            nid.uID = ID_TRAYICON;
            nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            nid.uCallbackMessage = WM_TRAYICON;
            nid.hIcon = CreateRedIcon();
            lstrcpy(nid.szTip, TEXT("Timer Resolution Fix (1ms)"));
            Shell_NotifyIcon(NIM_ADD, &nid);
            return 0;
        }
        case WM_TRAYICON: {
            // Gestione del click destro per il menu di uscita
            if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);
                HMENU hMenu = CreatePopupMenu();
                AppendMenu(hMenu, MF_STRING, ID_EXIT, TEXT("Exit"));
                
                SetForegroundWindow(hwnd);
                TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);
            }
            return 0;
        }
        case WM_COMMAND: {
            // Ricezione del comando "Exit"
            if (LOWORD(wParam) == ID_EXIT) {
                PostMessage(hwnd, WM_CLOSE, 0, 0);
            }
            return 0;
        }
        case WM_CLOSE: {
            // Pulizia risorse e ripristino timer
            Shell_NotifyIcon(NIM_DELETE, &nid);
            timeEndPeriod(1); // Ripristina la resolution standard
            if (nid.hIcon) DestroyIcon(nid.hIcon);
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// Entry point nascosto (senza console)
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    const char CLASS_NAME[] = "TimerFixClass";
    
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;

    RegisterClass(&wc);

    // Crea una window "Message-Only" invisibile
    HWND hwnd = CreateWindowEx(
        0, CLASS_NAME, "TimerFix", 0, 
        0, 0, 0, 0, 
        HWND_MESSAGE, NULL, hInstance, NULL
    );

    if (hwnd == NULL) return 0;

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    return 0;
}
