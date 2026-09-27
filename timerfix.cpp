/*
 timerdeltafix.cpp -- Windows 10 x64; one source, TWO build targets.

 IMPORTANT: This is source for a tray EXE AND a kernel SYS, not a driverless
 global timer fix. Requires MSVC + Windows SDK/WDK and a driver signature
 accepted by the target Windows installation. Source-reviewed, NOT built or
 runtime-tested on Windows by the authoring environment.

 WHY: The supplied API Monitor capture has 92,367 records. One-based records:
   73520: WriteFile writes 0x5000 bytes of an x64 driver (only 1024 captured).
   73527: WriteFile writes a further 0x2F0 bytes.
   73632: CreateServiceA("dpclat_driver", SERVICE_KERNEL_DRIVER, DEMAND_START).
   73671: StartServiceA succeeds.
   73747: CreateFileA("\\\\.\\dpclat_static_device") succeeds.
   73761: DeviceIoControl(0x81772008), DWORD input 1000, output 999, succeeds.
   80769 onwards: 0x81772010 requests return 20 bytes of measurement data.
 No timeBeginPeriod or NtSetTimerResolution call is recorded. Definitions of
 these APIs in the archive are NOT evidence of calls. The IOCTL units and the
 original driver's internal implementation are NOT proven by this capture.
 The truncated driver cannot be disassembled to establish those details.

 This replacement independently uses the documented kernel API
 ExSetTimerResolution(10000, TRUE) (100 ns units: 1 ms). It does not reproduce
 proprietary IOCTLs, sampling, DPCs, or polling. A device handle owns the
 request; IRP_MJ_CLEANUP releases it even if the tray process is terminated.
 Sleep completion is still subject to scheduling. No guaranteed delta/FPS
 improvement is claimed. Higher clock frequency can increase power use.

 BUILD EXE (x64 Native Tools Command Prompt for Visual Studio):
   cl /nologo /O1 /W4 /MT /DUNICODE /D_UNICODE timerdeltafix.cpp /link /SUBSYSTEM:WINDOWS /OUT:timerdeltafix.exe
 The linker pragma below supplies an elevation manifest and required libs.

 BUILD SYS (Visual Studio with compatible Windows SDK + WDK installed):
   Create an Empty WDM Driver project, target Desktop / Windows 10 / x64.
   Remove any generated source/INF items and add THIS file.
   Add TIMERDELTAFIX_KERNEL to C/C++ > Preprocessor Definitions.
   Set Target Name to timerdeltafix; build Release x64 using the WDK toolset.
   Link Wdmsec.lib (also specified below). Keep WDK security/linker defaults.
   No KMDF dependency, C++ exceptions, RTTI, or CRT is needed in this branch.
   Sign the resulting timerdeltafix.sys for the target machine's policy.
 A normal release Windows machine will not load an arbitrary unsigned SYS.
 This source does not change signature enforcement or boot security settings.

 INSTALL ONCE (elevated CMD; replace the absolute path with your signed SYS):
   sc.exe create timerdeltafix type= kernel start= demand binPath= "C:\Tools\timerdeltafix\timerdeltafix.sys"
 Then run timerdeltafix.exe. It starts the installed driver on demand and
 opens its device. Click or right-click its icon: title timerdeltafix, Exit.
 Exit releases the timer request and stops the driver if this EXE started it.
 The demand-start service registration remains for the next launch.
 Uninstall, after exiting:
   sc.exe stop timerdeltafix
   sc.exe delete timerdeltafix
 Then delete the EXE/SYS. A crash may leave the driver loaded but inactive;
 the request is tied to the handle, NOT to service lifetime.

 VALIDATE ON THE ACTUAL PC:
   Close DPC Checker and other timer tools. Run the unchanged benchmark in
   a separate process; retain at least 1000 samples before, during, and after
   timerdeltafix. Compare median/p95 and maximum sleep/delta, not just the
   reported resolution. Also test Exit, Task Manager termination, and tray
   restoration after Explorer restart. Run Driver Verifier in a test VM
   before considering the driver validated. The EXE checks driver protocol
   and the actual reported interval at startup, not the external Sleep delta.

 References:
 https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod
 https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-exsettimerresolution
 https://learn.microsoft.com/en-us/windows-hardware/drivers/install/kernel-mode-code-signing-policy--windows-vista-and-later-
*/

#if defined(TIMERDELTAFIX_KERNEL)

#include <ntddk.h>
#include <wdmsec.h>
#pragma comment(lib, "Wdmsec.lib")

static const ULONG kQuery = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800,
                                    METHOD_BUFFERED, FILE_READ_DATA);
static const ULONG kMagic = 0x31464454; // TDF1
static UNICODE_STRING gLink = RTL_CONSTANT_STRING(L"\\DosDevices\\timerdeltafix");
static KMUTEX gLock;
static BOOLEAN gRequested = FALSE;
static ULONG gInterval = 0;
static const GUID kClassGuid =
    {0x8b90484f, 0x5d56, 0x4749, {0xb8, 0x9c, 0x47, 0xa1, 0xc8, 0x26, 0x23, 0x1c}};

static void Lock()
{
    KeWaitForSingleObject(&gLock, Executive, KernelMode, FALSE, nullptr);
}
static void Unlock() { KeReleaseMutex(&gLock, FALSE); }
static void ReleaseRequest()
{
    if (gRequested) {
        ExSetTimerResolution(0, FALSE);
        gRequested = FALSE;
        gInterval = 0;
    }
}

DRIVER_DISPATCH Dispatch;
_Use_decl_annotations_
NTSTATUS Dispatch(PDEVICE_OBJECT device, PIRP irp)
{
    UNREFERENCED_PARAMETER(device);
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR bytes = 0;
    // Create/cleanup/close/device-control for this legacy control device are
    // handled synchronously at <= APC_LEVEL. No power IRP path calls ExSet*.
    switch (sp->MajorFunction) {
    case IRP_MJ_CREATE:
        if (sp->FileObject->FileName.Length != 0) {
            status = STATUS_OBJECT_NAME_NOT_FOUND;
            break;
        }
        Lock();
        if (gRequested) {
            status = STATUS_SHARING_VIOLATION;
        } else {
            gInterval = ExSetTimerResolution(10000, TRUE);
            gRequested = TRUE;
            sp->FileObject->FsContext = reinterpret_cast<PVOID>(1);
            status = STATUS_SUCCESS;
        }
        Unlock();
        break;
    case IRP_MJ_CLEANUP:
        Lock();
        if (sp->FileObject->FsContext != nullptr) {
            ReleaseRequest();
            sp->FileObject->FsContext = nullptr;
        }
        Unlock();
        status = STATUS_SUCCESS;
        break;
    case IRP_MJ_CLOSE:
        status = STATUS_SUCCESS;
        break;
    case IRP_MJ_DEVICE_CONTROL:
        if (sp->Parameters.DeviceIoControl.IoControlCode != kQuery)
            break;
        if (sp->Parameters.DeviceIoControl.OutputBufferLength < 2 * sizeof(ULONG)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        Lock();
        if (sp->FileObject->FsContext != nullptr && gRequested) {
            ULONG* out = static_cast<ULONG*>(irp->AssociatedIrp.SystemBuffer);
            out[0] = kMagic;
            out[1] = gInterval;
            bytes = 2 * sizeof(ULONG);
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_INVALID_DEVICE_STATE;
        }
        Unlock();
        break;
    }
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = bytes;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

DRIVER_UNLOAD Unload;
_Use_decl_annotations_
void Unload(PDRIVER_OBJECT driver)
{
    Lock();
    ReleaseRequest();
    Unlock();
    IoDeleteSymbolicLink(&gLink);
    IoDeleteDevice(driver->DeviceObject);
}

extern "C" DRIVER_INITIALIZE DriverEntry;
_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    UNREFERENCED_PARAMETER(registry);
    KeInitializeMutex(&gLock, 0);
    for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i)
        driver->MajorFunction[i] = Dispatch;
    driver->DriverUnload = Unload;
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\Device\\timerdeltafix");
    PDEVICE_OBJECT device = nullptr;
    NTSTATUS status = IoCreateDeviceSecure(driver, 0, &name,
        FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, TRUE,
        &SDDL_DEVOBJ_SYS_ALL_ADM_ALL, &kClassGuid, &device);
    if (!NT_SUCCESS(status)) return status;
    status = IoCreateSymbolicLink(&gLink, &name);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(device);
        return status;
    }
    device->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

#else // Win32 tray application

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <shellapi.h>
#include <winioctl.h>
#include <strsafe.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(linker, "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")

static const wchar_t kName[] = L"timerdeltafix";
static const UINT kTrayMessage = WM_APP + 1;
static const DWORD kQuery = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800,
                                    METHOD_BUFFERED, FILE_READ_DATA);
static HANDLE gDevice = INVALID_HANDLE_VALUE;
static SC_HANDLE gService = nullptr;
static bool gStarted = false;
static NOTIFYICONDATAW gIcon = {};
static UINT gTaskbarCreated = 0;

static void Error(const wchar_t* action, DWORD code)
{
    wchar_t detail[512] = {}, message[1024] = {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, detail, ARRAYSIZE(detail), nullptr);
    StringCchPrintfW(message, ARRAYSIZE(message),
        L"%s\n\nWindows error %lu: %s", action, code, detail);
    MessageBoxW(nullptr, message, kName, MB_OK | MB_ICONERROR);
}

static void Release()
{
    if (gDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(gDevice); // kernel cleanup releases the timer request
        gDevice = INVALID_HANDLE_VALUE;
    }
    if (gService) {
        if (gStarted) {
            SERVICE_STATUS s = {};
            if (!ControlService(gService, SERVICE_CONTROL_STOP, &s)) {
                DWORD e = GetLastError();
                if (e != ERROR_SERVICE_NOT_ACTIVE)
                    Error(L"Timer request released, but driver service did not stop.", e);
            }
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
