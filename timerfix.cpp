        }
        CloseServiceHandle(gService);
        gService = nullptr;
        gStarted = false;
    }
}

static bool Acquire()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) { Error(L"Cannot open Service Control Manager.", GetLastError()); return false; }
    gService = OpenServiceW(scm, kName, SERVICE_START | SERVICE_STOP);
    DWORD e = gService ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(scm);
    if (!gService) {
        Error(L"Install the signed timerdeltafix.sys driver first. See the source header.", e);
        return false;
    }
    if (StartServiceW(gService, 0, nullptr)) {
        gStarted = true;
    } else {
        e = GetLastError();
        if (e != ERROR_SERVICE_ALREADY_RUNNING) {
            Error(L"Cannot start timerdeltafix.sys. Check its path and accepted driver signature.", e);
            return false;
        }
    }
    gDevice = CreateFileW(L"\\\\.\\timerdeltafix", GENERIC_READ | GENERIC_WRITE,
                          0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (gDevice == INVALID_HANDLE_VALUE) {
        Error(L"Cannot open the timer driver. Another instance may already own it.", GetLastError());
        return false;
    }
    DWORD reply[2] = {}, received = 0;
    if (!DeviceIoControl(gDevice, kQuery, nullptr, 0, reply, sizeof(reply),
                         &received, nullptr)) {
        Error(L"Cannot verify the driver's timer request.", GetLastError());
        return false;
    }
    // Small platform rounding around 1 ms is allowed. This is a resolution
    // check; the user's independent benchmark must verify Sleep behavior.
    if (received != sizeof(reply) || reply[0] != 0x31464454 ||
        reply[1] == 0 || reply[1] > 11000) {
        Error(L"Driver protocol mismatch or reported timer interval exceeds 1.1 ms.", ERROR_INVALID_DATA);
        return false;
    }
    return true;
}

static bool AddIcon()
{
    if (!Shell_NotifyIconW(NIM_ADD, &gIcon)) return false;
    gIcon.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &gIcon);
    return true;
}

static void Menu(HWND window)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, kName);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 1, L"Exit");
    POINT p = {};
    GetCursorPos(&p);
    SetForegroundWindow(window);
    UINT chosen = static_cast<UINT>(TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
        p.x, p.y, 0, window, nullptr));
    DestroyMenu(menu);
    PostMessageW(window, WM_NULL, 0, 0);
    if (chosen == 1) DestroyWindow(window);
}

static LRESULT CALLBACK WindowProc(HWND window, UINT msg, WPARAM wp, LPARAM lp)
{
    if (gTaskbarCreated != 0 && msg == gTaskbarCreated) {
        if (!AddIcon()) {
            MessageBoxW(window, L"Could not restore the tray icon. Exiting.", kName, MB_OK | MB_ICONERROR);
            DestroyWindow(window);
        }
        return 0;
    }
    switch (msg) {
    case kTrayMessage:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_CONTEXTMENU:
        case WM_LBUTTONUP: // fallback if Shell does not accept version 4
        case WM_RBUTTONUP:
            Menu(window);
            break;
        }
        return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION:
        if (wp) DestroyWindow(window);
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &gIcon);
        Release();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    // All memory is fixed-size. Idle work consists only of blocking GetMessage.
    HANDLE singleton = CreateMutexW(nullptr, FALSE, L"Local\\timerdeltafix.tray.v1");
    if (!singleton) { Error(L"Cannot create the instance lock.", GetLastError()); return 1; }
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(singleton); return 0; }
    if (!Acquire()) { Release(); CloseHandle(singleton); return 1; }
    gTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kName;
    if (!gTaskbarCreated || !RegisterClassW(&wc)) {
        Error(L"Cannot register the tray window.", GetLastError());
        Release(); CloseHandle(singleton); return 1;
    }
    // Hidden top-level window receives Explorer restart broadcasts.
    HWND window = CreateWindowExW(0, kName, kName, WS_OVERLAPPED,
                                   0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!window) {
        Error(L"Cannot create the tray window.", GetLastError());
        Release(); CloseHandle(singleton); return 1;
    }
    // Explorer normally runs at medium integrity; this EXE is elevated.
    ChangeWindowMessageFilterEx(window, gTaskbarCreated, MSGFLT_ALLOW, nullptr);
    gIcon.cbSize = sizeof(gIcon);
    gIcon.hWnd = window;
    gIcon.uID = 1;
    gIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    gIcon.uCallbackMessage = kTrayMessage;
    gIcon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    StringCchCopyW(gIcon.szTip, ARRAYSIZE(gIcon.szTip), kName);
    if (!AddIcon()) {
        MessageBoxW(window, L"Could not create the tray icon. Exiting.", kName, MB_OK | MB_ICONERROR);
        DestroyWindow(window); CloseHandle(singleton); return 1;
    }
    MSG msg = {};
    BOOL result;
    while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (result == -1) {
        Error(L"Tray message loop failed.", GetLastError());
        if (IsWindow(window)) DestroyWindow(window);
    }
    Release();
    CloseHandle(singleton);
    return result == -1 ? 1 : 0;
}
#endif
