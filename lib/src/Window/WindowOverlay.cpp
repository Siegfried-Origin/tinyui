#include <Window/WindowOverlay.h>

#ifndef USE_SDL

#include <windowsx.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <codecvt>
#include <tlhelp32.h>

#include "inter.cpp"

// TODO: Temporary
HWND g_overlayWnd = NULL;
HWND g_targetWnd = NULL;
WindowOverlay* g_overlay = nullptr;

WindowOverlay::WindowOverlay(
    WindowSystem* sys,
    const std::string& title,
    const std::filesystem::path& config,
    const std::wstring& processName)
#ifdef USE_VULKAN
    : Window(sys, title, config, 0, 0)
#else
    : Window(sys, title, config, 0, 0, DXGI_SWAP_EFFECT_SEQUENTIAL)
#endif
{
    // TODO: find a cleaner solution than global vars
    // Already hooked once
    if (g_overlayWnd) {
        throw std::runtime_error("Cannot hook two overlays");
    }

    g_overlayWnd = _hwnd;
    g_overlay = this;

    // Global hook
    HWINEVENTHOOK hook = SetWinEventHook(
        EVENT_OBJECT_CREATE,
        EVENT_OBJECT_LOCATIONCHANGE,
        NULL,
        w32WinEventProc,
        0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    if (!hook) {
        throw std::runtime_error("SetWinEventHook failed");
    }

    LONG_PTR exStyle =
        WS_EX_TOPMOST |         // Overlay
        WS_EX_TRANSPARENT |     // No click registered
        WS_EX_NOACTIVATE |      // No mouse catch
        WS_EX_TOOLWINDOW |      // No taskbar icon
        WS_EX_LAYERED;
    //LONG_PTR exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED;
    LONG_PTR styleWin = WS_POPUP;

    SetWindowLongPtr(_hwnd, GWL_EXSTYLE, exStyle);
    SetWindowLongPtr(_hwnd, GWL_STYLE, styleWin);

    //SetWindowPos(_hwnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

    SetLayeredWindowAttributes(_hwnd, 0, 255, LWA_ALPHA);
    const MARGINS margin = { -1, 0, 0, 0 };
    DwmExtendFrameIntoClientArea(_hwnd, &margin);

    SetWindowPos(_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    // Enable activation / deactivation of overlay focus with Pause / Break key
    RegisterHotKey(
        _hwnd,
        1,                  // ID
        0,                  // modifiers (CTRL, ALT, etc)
        VK_PAUSE            // touche Pause/Break
    );

    setShown(_shown);

    _processWatcher = std::thread(&WindowOverlay::processWatch, this, processName);
}


WindowOverlay::~WindowOverlay()
{
    _stopWatch = true;

    if (_processWatcher.joinable()) {
        _processWatcher.join();
    }

    g_overlayWnd = NULL;
    g_targetWnd = NULL;
    g_overlay = NULL;
}


void WindowOverlay::setShown(bool shown)
{
    _shown = shown;
    ::ShowWindow(_hwnd, active() ? SW_SHOW : SW_HIDE);
    ::UpdateWindow(_hwnd);
}


void WindowOverlay::startOverlay(DWORD pid)
{
    auto windows = getWindowsForPID(pid);

    if (!windows.empty()) {
        g_targetWnd = windows[0];

        positionOverlayOnTarget();
        _active = true;
        setShown(_shown);
    }
}


void WindowOverlay::stopOverlay()
{
    g_targetWnd = nullptr;
    ::ShowWindow(_hwnd, SW_HIDE);
    _active = false;

    ::ShowWindow(_hwnd, SW_MINIMIZE);
    ::UpdateWindow(_hwnd);
}


void WindowOverlay::processWatch(const std::wstring& processName)
{
    DWORD lastPid = 0;

    while (!_stopWatch) {
        DWORD pid = getProcessIdByName(processName);
        if (pid == 0 && lastPid != 0) {
            stopOverlay();
            lastPid = pid;
        }
        else if (pid != 0 && lastPid == 0) {
            startOverlay(pid);
            lastPid = pid;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    if (lastPid != 0) {
        stopOverlay();
    }
}


void WindowOverlay::toggleClickThrough()
{
    if (!_shown || !_active) return;

    _focused = !_focused;

    LONG_PTR ex = GetWindowLongPtr(_hwnd, GWL_EXSTYLE);

    bool transparent = (ex & WS_EX_TRANSPARENT) != 0;
    
    LONG_PTR exStyle;

    if (_focused) {
        exStyle =
            WS_EX_TOPMOST |         // Overlay
            //WS_EX_TRANSPARENT |     // No click registered
            //WS_EX_NOACTIVATE |      // No mouse catch
            WS_EX_TOOLWINDOW |      // No taskbar icon
            WS_EX_LAYERED;
        ::SetFocus(_hwnd);
        ::BringWindowToTop(_hwnd);
    }
    else {
        exStyle =
            WS_EX_TOPMOST |         // Overlay
            WS_EX_TRANSPARENT |     // No click registered
            WS_EX_NOACTIVATE |      // No mouse catch
            WS_EX_TOOLWINDOW |      // No taskbar icon
            WS_EX_LAYERED;
        //::SetFocus(g_targetWnd);
        ::BringWindowToTop(g_targetWnd);
    }

    SetWindowLongPtr(_hwnd, GWL_EXSTYLE, exStyle);

    SetWindowPos(_hwnd, NULL, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED
    );
}


void WindowOverlay::beginFrame() {
    Window::beginFrame();

    if (_focused) {
        ImGui::GetBackgroundDrawList()->AddRectFilled(
            ImVec2(0, 0),
            ImGui::GetIO().DisplaySize,
            IM_COL32(0, 0, 0, 128)
        );
    }
}


LRESULT WindowOverlay::w32WndProc(UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Switch between game / overlay
    if (msg == WM_HOTKEY) {
        if (wParam == 1) {
            toggleClickThrough();
            return 0;
        }
    }

    //if (GetAsyncKeyState(VK_PAUSE) & 0x1) {
    //    toggleClickThrough();
    //}

    return ::DefWindowProcW(_hwnd, msg, wParam, lParam);
}


void CALLBACK WindowOverlay::w32WinEventProc(
    HWINEVENTHOOK, DWORD event, HWND hwnd,
    LONG, LONG, DWORD, DWORD)
{
    if (g_targetWnd == nullptr || hwnd != g_targetWnd) { return; }

    if (event == EVENT_OBJECT_LOCATIONCHANGE || event == EVENT_OBJECT_CREATE) {
        positionOverlayOnTarget();
    }
}


DWORD WindowOverlay::getProcessIdByName(const std::wstring& exeName)
{
    DWORD resultPid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName.c_str()) == 0) {
                resultPid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    return resultPid;
}


bool WindowOverlay::isRealWindow(HWND hwnd) {
    return (GetWindow(hwnd, GW_OWNER) == NULL) && IsWindowVisible(hwnd);
}


std::vector<HWND> WindowOverlay::getWindowsForPID(DWORD pid)
{
    std::vector<HWND> out;
    struct EnumData { DWORD pid; std::vector<HWND>* res; } data{ pid, &out };

    auto enumProc = [](HWND hwnd, LPARAM lparam)->BOOL {
        EnumData* d = reinterpret_cast<EnumData*>(lparam);
        if (!isRealWindow(hwnd)) return TRUE;

        DWORD wpid = 0;
        GetWindowThreadProcessId(hwnd, &wpid);
        if (wpid == d->pid) d->res->push_back(hwnd);
        return TRUE;
        };

    EnumWindows((WNDENUMPROC)enumProc, (LPARAM)&data);
    return out;
}


void WindowOverlay::positionOverlayOnTarget() {
    if (!g_overlayWnd || !g_targetWnd) return;

    RECT r;
    if (GetWindowRect(g_targetWnd, &r)) {
        const uint32_t width = r.right - r.left;
        const uint32_t height = r.bottom - r.top;

        ShowWindow(g_overlayWnd, SW_RESTORE);

        SetWindowPos(
            g_overlayWnd, HWND_TOPMOST,
            r.left, r.top,
            width, height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED
        );
    }
}


#endif