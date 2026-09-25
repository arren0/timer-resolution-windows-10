// timerfix.cpp
// Windows 10 - DPC Latency Checker driver activator
//
// This version reproduces the user-mode portion visible in the supplied
// API Monitor capture:
//
//   OpenSCManagerA
//   CreateServiceA / StartServiceA for "dpclat_driver"
//   CreateFileA("\\\\.\\dpclat_static_device")
//   DeviceIoControl(IOCTL 0x81772010, 20-byte buffer)
//
// It intentionally does NOT call timeBeginPeriod/timeSetEvent/Sleep as the
// fix. The observed effect is associated with the kernel driver remaining
// active.
//
// The existing dpclat_driver.sys is preferred. If the service does not exist,
// the program looks for dpclat_driver.sys beside TimerFix.exe or in
// %SystemRoot%\\System32\\drivers.
//
// Build:
//   cl /O2 /Os /EHsc /DUNICODE /D_UNICODE timerfix.cpp /link /SUBSYSTEM:WINDOWS
//      user32.lib shell32.lib advapi32.lib /OUT:TimerFix.exe
//
// Run elevated because creating/starting a kernel service requires admin.
// The tray icon is created before driver setup so failures are visible.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>
#include <winsvc.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace {

constexpr wchar_t kServiceName[] = L"dpclat_driver";
constexpr wchar_t kDeviceName[]  = L"\\\\.\\dpclat_static_device";
constexpr DWORD   kIoctl        = 0x81772010u;

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT ID_EXIT = 1001;

HWND   g_hwnd = nullptr;
HANDLE g_stop = nullptr;
HANDLE g_worker = nullptr;
HANDLE g_device = INVALID_HANDLE_VALUE;
SC_HANDLE g_scm = nullptr;
SC_HANDLE g_service = nullptr;

bool g_serviceCreatedByUs = false;
bool g_serviceWasRunning = false;

NOTIFYICONDATAW g_nid{};

bool GetExeDir(wchar_t* out, DWORD cch)
{
    DWORD n = GetModuleFileNameW(nullptr, out, cch);
    if (!n || n >= cch)
        return false;

    while (n && out[n - 1] != L'\\')
        --n;

    if (!n)
        return false;

    out[n - 1] = L'\0';
    return true;
}

bool FileExists(const wchar_t* path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES &&
           !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool FindDriverPath(wchar_t* out, DWORD cch)
{
    wchar_t exeDir[MAX_PATH]{};
    if (GetExeDir(exeDir, ARRAYSIZE(exeDir))) {
        if (swprintf_s(out, cch, L"%s\\dpclat_driver.sys", exeDir) > 0 &&
            FileExists(out))
            return true;
    }

    wchar_t sys[MAX_PATH]{};
    UINT n = GetSystemDirectoryW(sys, ARRAYSIZE(sys));
    if (!n || n >= ARRAYSIZE(sys))
        return false;

    if (swprintf_s(out, cch, L"%s\\drivers\\dpclat_driver.sys", sys) <= 0)
        return false;

    return FileExists(out);
}

bool OpenServiceManager()
{
    g_scm = OpenSCManagerW(
        nullptr,
        nullptr,
        SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);

    return g_scm != nullptr;
}

bool OpenOrCreateDriverService()
{
    g_service = OpenServiceW(
        g_scm,
        kServiceName,
        SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_STOP);

    if (g_service)
        return true;

    if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST)
        return false;

    wchar_t driverPath[MAX_PATH]{};
    if (!FindDriverPath(driverPath, ARRAYSIZE(driverPath)))
        return false;

    g_service = CreateServiceW(
        g_scm,
        kServiceName,
        kServiceName,
        SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_STOP,
        SERVICE_KERNEL_DRIVER,
        SERVICE_DEMAND_START,
        SERVICE_ERROR_NORMAL,
        driverPath,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr);

    if (!g_service)
        return false;

    g_serviceCreatedByUs = true;
    return true;
}

bool QueryServiceRunning(bool& running)
{
    SERVICE_STATUS_PROCESS s{};
    DWORD bytes = 0;

    if (!QueryServiceStatusEx(
            g_service,
            SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&s),
            sizeof(s),
            &bytes))
        return false;

    running = (s.dwCurrentState == SERVICE_RUNNING);
    return true;
}

bool StartDriver()
{
    bool running = false;
    if (!QueryServiceRunning(running))
        return false;

    if (running) {
        g_serviceWasRunning = true;
        return true;
    }

    if (!StartServiceW(g_service, 0, nullptr)) {
        DWORD e = GetLastError();
        if (e != ERROR_SERVICE_ALREADY_RUNNING)
            return false;
    }

    for (int i = 0; i < 100; ++i) {
        Sleep(10);

        if (!QueryServiceRunning(running))
            return false;

        if (running)
            return true;
    }

    return false;
}

bool OpenDevice()
{
    g_device = CreateFileW(
        kDeviceName,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    return g_device != INVALID_HANDLE_VALUE;
}

bool SendObservedIoctl()
{
    if (g_device == INVALID_HANDLE_VALUE)
        return false;

    // This is the 20-byte input buffer observed in the supplied APMX trace.
    // It is passed as both input and output by dpclat.exe.
    DWORD data[5] = {
        0xFFFFFFFFu,
        0x00000000u,
        0x0000012Cu, // 300
        0x00000113u, // 275
        0x00000001u
    };

    DWORD returned = 0;

    return DeviceIoControl(
        g_device,
        kIoctl,
        data,
        sizeof(data),
        data,
        sizeof(data),
        &returned,
        nullptr) != FALSE;
}

DWORD WINAPI Worker(LPVOID)
{
    // The original application sends this request approximately once per
    // second while the driver/device stays open.
    SendObservedIoctl();

    while (WaitForSingleObject(g_stop, 1000) == WAIT_TIMEOUT)
        SendObservedIoctl();

    return 0;
}

bool AddTrayIcon(HWND hwnd)
{
    ZeroMemory(&g_nid, sizeof(g_nid));

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;

    g_nid.hIcon = LoadIconW(
        nullptr,
        MAKEINTRESOURCEW(IDI_APPLICATION));

    lstrcpynW(
        g_nid.szTip,
        L"TimerFix",
        ARRAYSIZE(g_nid.szTip));

    return Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
}

void RemoveTrayIcon()
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

void ShowTrayMenu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;

    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");

    POINT p{};
    GetCursorPos(&p);
    SetForegroundWindow(hwnd);

    TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
        p.x, p.y, 0, hwnd, nullptr);

    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);
}

void Cleanup()
{
    if (g_stop)
        SetEvent(g_stop);

    if (g_worker) {
        WaitForSingleObject(g_worker, INFINITE);
        CloseHandle(g_worker);
        g_worker = nullptr;
    }

    if (g_device != INVALID_HANDLE_VALUE) {
        CloseHandle(g_device);
        g_device = INVALID_HANDLE_VALUE;
    }

    // IMPORTANT:
    // Do not stop a pre-existing dpclat driver on Exit. DPC Latency
    // Checker's effect comes from the kernel driver being active.
    //
    // If we created the service only because it was absent, leave it
    // installed/running just like the normal DPC Latency Checker session.
    //
    // This also avoids tearing down the kernel component while another
    // process may still be using it.

    if (g_service) {
        CloseServiceHandle(g_service);
        g_service = nullptr;
    }

    if (g_scm) {
        CloseServiceHandle(g_scm);
        g_scm = nullptr;
    }
}

LRESULT CALLBACK WndProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (msg) {
    case WM_TRAY:
        if (lParam == WM_RBUTTONUP ||
            lParam == WM_CONTEXTMENU)
            ShowTrayMenu(hwnd);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_EXIT) {
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;

    case WM_DESTROY:
        Cleanup();
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND CreateHiddenWindow(HINSTANCE hInstance)
{
    constexpr wchar_t kClass[] = L"TimerFix_DPC_Tray";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = hInstance;
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = kClass;

    if (!RegisterClassExW(&wc))
        return nullptr;

    return CreateWindowExW(
        0,
        kClass,
        L"TimerFix",
        0,
        0, 0, 0, 0,
        HWND_MESSAGE,
        nullptr,
        hInstance,
        nullptr);
}

} // namespace

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    PWSTR,
    int)
{
    HANDLE mutex = CreateMutexW(
        nullptr,
        TRUE,
        L"Global\\TimerFix_DPC_Driver");

    if (!mutex)
        return 1;

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }

    // Tray exists independently of driver setup.
    g_stop = CreateEventW(
        nullptr,
        TRUE,
        FALSE,
        nullptr);

    if (!g_stop) {
        CloseHandle(mutex);
        return 1;
    }

    g_hwnd = CreateHiddenWindow(hInstance);
    if (!g_hwnd) {
        CloseHandle(g_stop);
        CloseHandle(mutex);
        return 1;
    }

    if (!AddTrayIcon(g_hwnd)) {
        DestroyWindow(g_hwnd);
        CloseHandle(g_stop);
        CloseHandle(mutex);
        return 1;
    }

    // Driver setup.
    bool ok =
        OpenServiceManager() &&
        OpenOrCreateDriverService() &&
        StartDriver() &&
        OpenDevice();

    if (ok)
        SendObservedIoctl();

    // Keep the process/tray alive even if driver setup failed.
    // The icon is therefore always visible and Exit always works.
    if (ok) {
        g_worker = CreateThread(
            nullptr,
            0,
            Worker,
            nullptr,
            0,
            nullptr);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_worker) {
        WaitForSingleObject(g_worker, INFINITE);
        CloseHandle(g_worker);
        g_worker = nullptr;
    }

    CloseHandle(g_stop);
    CloseHandle(mutex);

    return 0;
}
