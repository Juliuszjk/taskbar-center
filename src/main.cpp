// TaskbarCenter: keeps Windows 10 taskbar app icons centered on the screen.
//
// The program has no UI. It sleeps until Windows reports a change that can
// move or resize the taskbar buttons, then measures the buttons with UI
// Automation and moves the button list window (MSTaskListWClass) so the
// buttons sit in the middle of the monitor. Start, search and the tray stay
// where Explorer puts them.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <initguid.h>
#include <windows.h>
#include <shellapi.h>
#include <uiautomation.h>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <algorithm>
#include <vector>

namespace {

const wchar_t kAppName[] = L"TaskbarCenter";
const wchar_t kWindowClass[] = L"TaskbarCenterHiddenWindow";
const wchar_t kMutexName[] = L"Local\\TaskbarCenterSingleInstance";
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

const UINT_PTR kTimerRecalc = 1;
const UINT_PTR kTimerSafety = 2;
const UINT kFirstDelayMs = 60;               // lets a burst of events settle
const UINT kFollowUpDelaysMs[] = {250, 700};  // catch button grow/shrink animations
const UINT kSafetyIntervalMs = 2000;          // safety net in case an event is missed

HWND g_window = nullptr;
UINT g_taskbarCreatedMsg = 0;
IUIAutomation* g_uia = nullptr;
HWINEVENTHOOK g_globalHook = nullptr;
HWINEVENTHOOK g_explorerHook = nullptr;
DWORD g_explorerPid = 0;
bool g_timerPending = false;
bool g_pendingIsFollowUp = false;
int g_followUpsLeft = 0;
const char* g_pendingReason = "start";
bool g_debug = false;
wchar_t g_logPath[MAX_PATH] = L"";

struct TaskList {
    HWND list;    // MSTaskListWClass: the window that holds the buttons
    HWND parent;  // MSTaskSwWClass on the main taskbar, WorkerW on the others
    HWND tray;    // Shell_TrayWnd or Shell_SecondaryTrayWnd
};

struct Original {
    HWND list;
    POINT pos;  // in parent client coordinates
};
std::vector<Original> g_originals;

// ---------------------------------------------------------------- logging

void Log(const char* fmt, ...) {
    if (!g_debug) return;
    FILE* f = _wfopen(g_logPath, L"a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

void InitLogPath() {
    // Next to the exe if that folder is writable, otherwise in %TEMP%.
    wchar_t dir[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, dir, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (n && slash) {
        *slash = 0;
        swprintf(g_logPath, MAX_PATH, L"%ls\\TaskbarCenter.log", dir);
        FILE* f = _wfopen(g_logPath, L"a");
        if (f) { fclose(f); return; }
    }
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    swprintf(g_logPath, MAX_PATH, L"%lsTaskbarCenter.log", temp);
}

// ---------------------------------------------------------------- taskbars

bool HasClass(HWND hwnd, const wchar_t* name) {
    wchar_t buf[64];
    return hwnd && GetClassNameW(hwnd, buf, 64) && wcscmp(buf, name) == 0;
}

HWND FindDescendant(HWND root, const wchar_t* cls) {
    struct Ctx { const wchar_t* cls; HWND found; } ctx{cls, nullptr};
    EnumChildWindows(root, [](HWND h, LPARAM p) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(p);
        if (HasClass(h, c->cls)) { c->found = h; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

std::vector<TaskList> FindTaskLists() {
    std::vector<TaskList> result;
    auto add = [&](HWND tray) {
        HWND list = FindDescendant(tray, L"MSTaskListWClass");
        if (list) result.push_back({list, GetParent(list), tray});
    };
    if (HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr)) add(primary);
    HWND secondary = nullptr;
    while ((secondary = FindWindowExW(nullptr, secondary, L"Shell_SecondaryTrayWnd", nullptr)))
        add(secondary);
    return result;
}

// Only the windows whose changes can move the buttons: the button list, its
// parent and the taskbar itself. Tray icons and the clock live elsewhere.
bool IsTaskbarLayoutWindow(HWND hwnd) {
    if (HasClass(hwnd, L"MSTaskListWClass") || HasClass(hwnd, L"MSTaskSwWClass") ||
        HasClass(hwnd, L"Shell_TrayWnd") || HasClass(hwnd, L"Shell_SecondaryTrayWnd"))
        return true;
    return HasClass(hwnd, L"WorkerW") && HasClass(GetParent(hwnd), L"Shell_SecondaryTrayWnd");
}

bool IsMirrored(HWND hwnd) {
    return (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) != 0;
}

// Union of the button rectangles, in screen pixels. Returns false if the
// list has no visible buttons.
bool MeasureButtons(HWND list, RECT* out, int* count) {
    *count = 0;
    if (!g_uia) return false;
    IUIAutomationElement* root = nullptr;
    if (FAILED(g_uia->ElementFromHandle(list, &root)) || !root) return false;

    bool ok = false;
    IUIAutomationCondition* all = nullptr;
    IUIAutomationElementArray* children = nullptr;
    if (SUCCEEDED(g_uia->CreateTrueCondition(&all)) &&
        SUCCEEDED(root->FindAll(TreeScope_Children, all, &children)) && children) {
        int n = 0;
        children->get_Length(&n);
        RECT u{};
        for (int i = 0; i < n; ++i) {
            IUIAutomationElement* el = nullptr;
            if (FAILED(children->GetElement(i, &el)) || !el) continue;
            RECT r{};
            if (SUCCEEDED(el->get_CurrentBoundingRectangle(&r)) &&
                r.right > r.left && r.bottom > r.top) {
                if (*count == 0) u = r;
                else UnionRect(&u, &u, &r);
                ++*count;
            }
            el->Release();
        }
        if (*count > 0) { *out = u; ok = true; }
        children->Release();
    }
    if (all) all->Release();
    root->Release();
    return ok;
}

POINT ClientPos(HWND child, HWND parent) {
    // Map the whole rect: for a mirrored (right-to-left) parent Windows then
    // swaps left and right, so r.left is the origin SetWindowPos expects.
    RECT r;
    GetWindowRect(child, &r);
    MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(&r), 2);
    return {r.left, r.top};
}

void RememberOriginal(HWND list, HWND parent) {
    for (const auto& o : g_originals)
        if (o.list == list) return;
    g_originals.push_back({list, ClientPos(list, parent)});
}

void MoveList(HWND list, int x, int y) {
    // Async so a hung Explorer never blocks us; NOSENDCHANGING so Explorer's
    // WM_WINDOWPOSCHANGING handler cannot veto the move.
    SetWindowPos(list, nullptr, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                 SWP_NOSENDCHANGING | SWP_ASYNCWINDOWPOS);
}

void CenterOne(const TaskList& t, const char* reason) {
    RECT parentRect, buttons;
    int count = 0;
    if (!GetWindowRect(t.parent, &parentRect)) return;
    if (!MeasureButtons(t.list, &buttons, &count)) {
        Log("[%s] list %p: no buttons, skipped", reason, (void*)t.list);
        return;
    }

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(t.tray, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& mon = mi.rcMonitor;

    const bool horizontal = (parentRect.right - parentRect.left) >= (parentRect.bottom - parentRect.top);
    // Work on one axis; the code is the same for a horizontal and a vertical taskbar.
    const long monStart = horizontal ? mon.left : mon.top;
    const long monLen = horizontal ? mon.right - mon.left : mon.bottom - mon.top;
    const long parStart = horizontal ? parentRect.left : parentRect.top;
    const long parLen = horizontal ? parentRect.right - parentRect.left : parentRect.bottom - parentRect.top;
    const long btnStart = horizontal ? buttons.left : buttons.top;
    const long btnLen = horizontal ? buttons.right - buttons.left : buttons.bottom - buttons.top;

    long target = monStart + (monLen - btnLen) / 2;
    // Never cover Start, search or the tray: stay inside the button area.
    const long lo = parStart;
    const long hi = parStart + parLen - btnLen;
    if (hi < lo) target = lo;  // more buttons than space: Explorer's own layout
    else if (target < lo) target = lo;
    else if (target > hi) target = hi;

    const long delta = target - btnStart;
    Log("[%s] list %p: monitor %ld+%ld, area %ld+%ld, %d buttons at %ld+%ld, delta %ld",
        reason, (void*)t.list, monStart, monLen, parStart, parLen, count, btnStart, btnLen, delta);
    if (delta >= -1 && delta <= 1) return;

    RememberOriginal(t.list, t.parent);
    POINT p = ClientPos(t.list, t.parent);
    // In a mirrored parent, x grows to the left.
    if (horizontal) p.x += IsMirrored(t.parent) ? -delta : delta; else p.y += delta;
    MoveList(t.list, p.x, p.y);
}

void CenterAll(const char* reason) {
    const auto lists = FindTaskLists();
    // Forget taskbars that are gone (monitor unplugged), so a reused handle
    // never gets moved on exit.
    g_originals.erase(std::remove_if(g_originals.begin(), g_originals.end(), [&](const Original& o) {
        return std::none_of(lists.begin(), lists.end(), [&](const TaskList& t) { return t.list == o.list; });
    }), g_originals.end());
    for (const auto& t : lists) CenterOne(t, reason);
}

void RestoreAll() {
    for (const auto& o : g_originals) {
        if (!HasClass(o.list, L"MSTaskListWClass")) continue;
        MoveList(o.list, o.pos.x, o.pos.y);
        Log("restored list %p to %ld,%ld", (void*)o.list, o.pos.x, o.pos.y);
    }
    g_originals.clear();
}

// ---------------------------------------------------------------- triggers

void ScheduleRecalc(const char* reason) {
    g_followUpsLeft = 2;
    // Keep a pending first check, so a stream of events can't starve the timer,
    // but bring a slow follow-up check forward.
    if (g_timerPending && !g_pendingIsFollowUp) return;
    g_timerPending = true;
    g_pendingIsFollowUp = false;
    g_pendingReason = reason;
    SetTimer(g_window, kTimerRecalc, kFirstDelayMs, nullptr);
}

// Would this top-level window get (or lose) a taskbar button?
bool LooksLikeAppWindow(HWND hwnd) {
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return false;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return false;
    if (ex & WS_EX_APPWINDOW) return true;
    if (ex & WS_EX_NOACTIVATE) return false;  // tooltips, hover cards, IME popups
    return GetWindow(hwnd, GW_OWNER) == nullptr;
}

void CALLBACK OnGlobalEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_HIDE) return;
    if (HasClass(hwnd, L"Shell_SecondaryTrayWnd")) { ScheduleRecalc("monitor"); return; }
    if (LooksLikeAppWindow(hwnd)) ScheduleRecalc("window");
}

void CALLBACK OnExplorerEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    switch (event) {
        case EVENT_OBJECT_CREATE: case EVENT_OBJECT_DESTROY: case EVENT_OBJECT_SHOW:
        case EVENT_OBJECT_HIDE: case EVENT_OBJECT_REORDER: case EVENT_OBJECT_LOCATIONCHANGE:
            break;
        default:
            return;  // hover, focus and state changes don't move buttons
    }
    if (hwnd && IsTaskbarLayoutWindow(hwnd)) ScheduleRecalc("taskbar");
}

void UpdateHooks() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD pid = 0;
    if (tray) GetWindowThreadProcessId(tray, &pid);
    if (pid == g_explorerPid && (g_explorerHook || !pid)) return;
    if (g_explorerHook) { UnhookWinEvent(g_explorerHook); g_explorerHook = nullptr; }
    if (pid != g_explorerPid) g_originals.clear();  // a new Explorer means new taskbar windows
    g_explorerPid = pid;
    if (pid)
        g_explorerHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
                                         OnExplorerEvent, pid, 0, WINEVENT_OUTOFCONTEXT);
    Log("explorer pid %lu, hook %p", pid, (void*)g_explorerHook);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreatedMsg && msg) {
        Log("Explorer restarted");
        UpdateHooks();
        ScheduleRecalc("explorer");
        return 0;
    }
    switch (msg) {
        case WM_TIMER:
            if (wp == kTimerRecalc) {
                KillTimer(hwnd, kTimerRecalc);
                g_timerPending = false;
                CenterAll(g_pendingReason);
                if (g_followUpsLeft > 0) {
                    UINT delay = kFollowUpDelaysMs[2 - g_followUpsLeft];
                    --g_followUpsLeft;
                    g_timerPending = true;
                    g_pendingIsFollowUp = true;
                    g_pendingReason = "follow-up";
                    SetTimer(hwnd, kTimerRecalc, delay, nullptr);
                }
            } else if (wp == kTimerSafety) {
                UpdateHooks();
                if (!g_timerPending) CenterAll("safety");
            }
            return 0;
        case WM_DISPLAYCHANGE:
            ScheduleRecalc("display");
            return 0;
        case WM_SETTINGCHANGE:
            ScheduleRecalc("settings");
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- commands

void SetDpiAwareness() {
    // Per-monitor v2 so all coordinates are real pixels on every monitor.
    using Fn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")));
    if (!fn || !fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) SetProcessDPIAware();
}

void Message(const wchar_t* text, UINT icon = MB_ICONINFORMATION) {
    MessageBoxW(nullptr, text, kAppName, MB_OK | icon);
}

bool StopRunning() {
    HWND other = FindWindowW(kWindowClass, nullptr);
    if (!other) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(other, WM_CLOSE, 0, 0);
    if (proc) { WaitForSingleObject(proc, 5000); CloseHandle(proc); }
    return true;
}

bool SetAutostart(bool enable, const wchar_t* exePath) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;
    LONG rc;
    if (enable) {
        wchar_t value[MAX_PATH + 32];
        swprintf(value, MAX_PATH + 32, L"\"%ls\" --autostart", exePath);
        rc = RegSetValueExW(key, kAppName, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                            static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(key, kAppName);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

bool OnRemovableDrive(const wchar_t* path) {
    wchar_t root[4] = {path[0], L':', L'\\', 0};
    return GetDriveTypeW(root) == DRIVE_REMOVABLE;
}

const wchar_t kHelp[] =
    L"TaskbarCenter keeps taskbar icons centered on the screen.\n\n"
    L"TaskbarCenter.exe\t\tstart centering now\n"
    L"--install\t\tstart now and at every sign-in\n"
    L"--uninstall\t\tstop and remove from sign-in\n"
    L"--stop\t\t\tstop and restore the taskbar\n"
    L"--debug\t\t\tstart and write TaskbarCenter.log\n"
    L"--help\t\t\tshow this message";

int Run() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) ||
        FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IUIAutomation, reinterpret_cast<void**>(&g_uia)))) {
        Message(L"UI Automation is not available on this system.", MB_ICONERROR);
        return 1;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;
    RegisterClassW(&wc);
    // A hidden top-level window (not message-only), because broadcasts such as
    // TaskbarCreated and WM_DISPLAYCHANGE only reach top-level windows.
    g_window = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, kAppName, WS_POPUP,
                               0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_window, g_taskbarCreatedMsg, MSGFLT_ALLOW, nullptr);

    g_globalHook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, nullptr, OnGlobalEvent,
                                   0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    UpdateHooks();
    ScheduleRecalc("start");
    SetTimer(g_window, kTimerSafety, kSafetyIntervalMs, nullptr);
    Log("started");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_globalHook) UnhookWinEvent(g_globalHook);
    if (g_explorerHook) UnhookWinEvent(g_explorerHook);
    RestoreAll();
    Log("stopped");
    g_uia->Release();
    CoUninitialize();
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetDpiAwareness();

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool install = false, uninstall = false, stop = false, autostart = false;
    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (!wcscmp(a, L"--install")) install = true;
        else if (!wcscmp(a, L"--uninstall")) uninstall = true;
        else if (!wcscmp(a, L"--stop")) stop = true;
        else if (!wcscmp(a, L"--autostart")) autostart = true;
        else if (!wcscmp(a, L"--debug")) g_debug = true;
        else { Message(kHelp); return !wcscmp(a, L"--help") ? 0 : 2; }
    }
    LocalFree(argv);

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    if (uninstall || stop) {
        bool wasRunning = StopRunning();
        if (uninstall) {
            if (!SetAutostart(false, exePath)) {
                Message(L"Could not remove the sign-in entry from the registry.", MB_ICONERROR);
                return 1;
            }
            Message(L"TaskbarCenter is stopped and will no longer start at sign-in.");
        } else {
            Message(wasRunning ? L"TaskbarCenter is stopped and the taskbar is restored."
                               : L"TaskbarCenter was not running.");
        }
        return 0;
    }

    if (install) {
        if (!SetAutostart(true, exePath)) {
            Message(L"Could not add the sign-in entry to the registry.", MB_ICONERROR);
            return 1;
        }
        wchar_t text[MAX_PATH + 256];
        swprintf(text, MAX_PATH + 256,
                 L"TaskbarCenter will start at every sign-in from:\n%ls\n\n"
                 L"Keep the file in this folder. To remove it, run it with --uninstall.%ls",
                 exePath,
                 OnRemovableDrive(exePath) ? L"\n\nWarning: this is a removable drive. Without it, "
                                             L"TaskbarCenter will not start." : L"");
        Message(text);
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!autostart && !install)
            Message(L"TaskbarCenter is already running.\nRun it with --stop to stop it.");
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    if (g_debug) InitLogPath();
    int rc = Run();
    if (mutex) CloseHandle(mutex);
    return rc;
}
