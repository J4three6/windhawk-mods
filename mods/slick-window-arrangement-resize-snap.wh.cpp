// ==WindhawkMod==
// @id              slick-window-arrangement-resize-snap
// @name            Slick Window Arrangement (Resize Snap)
// @description     Adds snapping not only when moving windows, but also when resizing them (left, right, top, bottom).
// @version         1.2.0
// @author          m417z (original), J4three6 (fork with resize support)
// @github          J4three6
// @license         MIT
// @include         *
// @compilerOptions -lcomctl32 -ldwmapi
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Slick Window Arrangement (Resize Snap)

This fork of the original mod by **m417z** adds:

✅ Snapping when **moving** windows (original behavior)  
✅ Snapping when **resizing** windows from any edge (new)  
✅ Works during live-resize via **WM_SIZING** for instant feedback  

## Settings
- Snap distance in pixels  
- Temporarily disable snapping with Ctrl/Alt/Shift  
- Optional sliding animation after releasing a window

## License
MIT
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- SnapWindowsWhenDragging: true
  $name: Snap windows when dragging
- SnapWindowsDistance: 25
  $name: Snap windows distance
  $description: Distance in pixels for windows to snap
- KeysToDisableSnapping:
  - Ctrl: false
  - Alt: true
  - Shift: false
  $name: Keys to temporarily disable snapping
  $description: Combination of keys to temporarily disable snapping
- SlidingAnimation: true
  $name: Sliding animation
  $description: Keep sliding the window after it's being moved
- SnapWindowsWhenSliding: true
  $name: Snap windows when sliding
- SlidingAnimationSlowdown: 15
  $name: Sliding animation slowdown
  $description: Smaller values = longer sliding (1–99)
*/
// ==/WindhawkModSettings==
/*
- SnapWindowsWhenDragging: true
  $name: Snap windows when dragging
- SnapWindowsDistance: 25
  $name: Snap windows distance
  $description: Set the required distance for windows to snap to other windows
- KeysToDisableSnapping:
  - Ctrl: false
  - Alt: true
  - Shift: false
  $name: Keys to temporarily disable snapping
  $description: A combination of keys that can be used to temporarily disable snapping
- SlidingAnimation: true
  $name: Sliding animation
  $description: Keep sliding the window after it's being moved
- SnapWindowsWhenSliding: true
  $name: Snap windows when sliding
- SlidingAnimationSlowdown: 15
  $name: Sliding animation slowdown
  $description: Set a smaller value for a sliding animation that lasts longer (between 1 and 99)
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "dwmapi.lib")

struct {
    bool snapWindowsWhenDragging;
    int  snapWindowsDistance;
    bool keysToDisableSnappingCtrl;
    bool keysToDisableSnappingAlt;
    bool keysToDisableSnappingShift;
    bool slidingAnimation;
    bool snapWindowsWhenSliding;
    int  slidingAnimationSlowdown;
} g_settings;

#ifndef SWP_STATECHANGED
#define SWP_STATECHANGED 0x8000
#endif

typedef DPI_AWARENESS_CONTEXT (WINAPI *GetThreadDpiAwarenessContext_t)();
typedef DPI_AWARENESS_CONTEXT (WINAPI *SetThreadDpiAwarenessContext_t)(DPI_AWARENESS_CONTEXT dpiContext);
typedef DPI_AWARENESS (WINAPI *GetAwarenessFromDpiAwarenessContext_t)(DPI_AWARENESS_CONTEXT value);
typedef UINT (WINAPI *GetDpiForSystem_t)();
typedef UINT (WINAPI *GetDpiForWindow_t)(HWND hwnd);
typedef BOOL (WINAPI *IsWindowArranged_t)(HWND hwnd);
typedef HRESULT (WINAPI *GetDpiForMonitor_t)(HMONITOR hmonitor, MONITOR_DPI_TYPE dpiType, UINT *dpiX, UINT *dpiY);

GetThreadDpiAwarenessContext_t   pGetThreadDpiAwarenessContext;
SetThreadDpiAwarenessContext_t   pSetThreadDpiAwarenessContext;
GetAwarenessFromDpiAwarenessContext_t pGetAwarenessFromDpiAwarenessContext;
GetDpiForSystem_t pGetDpiForSystem;
GetDpiForWindow_t pGetDpiForWindow;
IsWindowArranged_t pIsWindowArranged;
GetDpiForMonitor_t pGetDpiForMonitor;

// Forwards
static LRESULT CALLBACK SubclassWndProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
static void     CALLBACK WindowSlideTimerProc(HWND, UINT, UINT_PTR, DWORD);

static BOOL IsWindowCloaked(HWND hwnd) {
    BOOL isCloaked = FALSE;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &isCloaked, sizeof(isCloaked))) && isCloaked;
}

static BOOL GetWindowFrameBounds(HWND hWnd, LPRECT lpRect) {
    if (FAILED(DwmGetWindowAttribute(hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, lpRect, sizeof(*lpRect))) &&
        !GetWindowRect(hWnd, lpRect)) {
        return FALSE;
    }

    if (pGetThreadDpiAwarenessContext && pGetAwarenessFromDpiAwarenessContext &&
        pGetAwarenessFromDpiAwarenessContext(pGetThreadDpiAwarenessContext()) == DPI_AWARENESS_PER_MONITOR_AWARE) {
        return TRUE;
    }

    if (pSetThreadDpiAwarenessContext && pGetDpiForMonitor && pGetDpiForSystem) {
        auto prev = pSetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        HMONITOR monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);

        MONITORINFO monitorInfo = { sizeof(monitorInfo) };
        GetMonitorInfo(monitor, &monitorInfo);
        OffsetRect(lpRect, -monitorInfo.rcMonitor.left, -monitorInfo.rcMonitor.top);

        UINT dpiFromX, dpiFromY;
        pGetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiFromX, &dpiFromY);
        UINT dpiFrom = dpiFromX;

        pSetThreadDpiAwarenessContext(prev);

        UINT dpiTo = pGetDpiForSystem();

        lpRect->left   = MulDiv(lpRect->left,   dpiTo, dpiFrom);
        lpRect->top    = MulDiv(lpRect->top,    dpiTo, dpiFrom);
        lpRect->right  = MulDiv(lpRect->right,  dpiTo, dpiFrom);
        lpRect->bottom = MulDiv(lpRect->bottom, dpiTo, dpiFrom);

        GetMonitorInfo(monitor, &monitorInfo);
        OffsetRect(lpRect, monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.top);
    }
    return TRUE;
}

class WindowMagnet {
public:
    WindowMagnet(HWND hTargetWnd) { 
        CalculateMetrics(hTargetWnd);

        InitialWndEnumProcParam enumParam;
        enumParam.hTargetWnd = hTargetWnd;
        EnumWindows(InitialWndEnumProc, (LPARAM)&enumParam);

        for (auto it = enumParam.windowRects.rbegin(); it != enumParam.windowRects.rend(); ++it) {
            const auto& rc = *it;
            RemoveOverlappedTargets(magnetTargetsLeft, rc.left, rc.right, rc.top, rc.bottom);
            RemoveOverlappedTargets(magnetTargetsTop, rc.top, rc.bottom, rc.left, rc.right);
            RemoveOverlappedTargets(magnetTargetsRight, rc.left, rc.right, rc.top, rc.bottom);
            RemoveOverlappedTargets(magnetTargetsBottom, rc.top, rc.bottom, rc.left, rc.right);

            magnetTargetsLeft.emplace(rc.left, rc.top, rc.bottom);
            magnetTargetsTop.emplace(rc.top, rc.left, rc.right);
            magnetTargetsRight.emplace(rc.right, rc.top, rc.bottom);
            magnetTargetsBottom.emplace(rc.bottom, rc.left, rc.right);
        }
        EnumDisplayMonitors(nullptr, nullptr, InitialMonitorEnumProc, (LPARAM)this);
    }

    void MagnetMove(HWND hSourceWnd, int* x, int* y, int* cx, int* cy) {
        if (IsSnappingTemporarilyDisabled()) return;

        CalculateMetrics(hSourceWnd);

        RECT sourceRect = {
            *x + windowBorderRect.left,
            *y + windowBorderRect.top,
            *x + *cx - windowBorderRect.right,
            *y + *cy - windowBorderRect.bottom
        };

        int newX = *x, newY = *y;

        long targetLeft  = FindClosestTarget(magnetTargetsLeft,  sourceRect.right, sourceRect.top, sourceRect.bottom, magnetPixels);
        long targetRight = FindClosestTarget(magnetTargetsRight, sourceRect.left,  sourceRect.top, sourceRect.bottom, magnetPixels);

        if (targetLeft != LONG_MAX && targetRight != LONG_MAX &&
            std::abs(targetLeft - sourceRect.right) < std::abs(targetRight - sourceRect.left)) {
            newX = targetLeft - *cx + windowBorderRect.right;
        } else if (targetRight != LONG_MAX) {
            newX = targetRight - windowBorderRect.left;
        } else if (targetLeft != LONG_MAX) {
            newX = targetLeft - *cx + windowBorderRect.right;
        }

        long targetTop    = FindClosestTarget(magnetTargetsTop,    sourceRect.bottom, sourceRect.left, sourceRect.right, magnetPixels);
        long targetBottom = FindClosestTarget(magnetTargetsBottom, sourceRect.top,    sourceRect.left, sourceRect.right, magnetPixels);

        if (targetTop != LONG_MAX && targetBottom != LONG_MAX &&
            std::abs(targetTop - sourceRect.bottom) < std::abs(targetBottom - sourceRect.top)) {
            newY = targetTop - *cy + windowBorderRect.bottom;
        } else if (targetBottom != LONG_MAX) {
            newY = targetBottom - windowBorderRect.top;
        } else if (targetTop != LONG_MAX) {
            newY = targetTop - *cy + windowBorderRect.bottom;
        }

        if (newX != *x || newY != *y) {
            RECT targetRect = {
                newX + windowBorderRect.left,
                newY + windowBorderRect.top,
                newX + *cx - windowBorderRect.right,
                newY + windowBorderRect.top + 1
            };
            if (IsRectInWorkArea(targetRect)) { *x = newX; *y = newY; }
        }
    }

    // Resize snapping: NOTE the target sets mapping (left↔Right, right↔Left, top↔Bottom, bottom↔Top)
    void MagnetResize(HWND hWnd, int* x, int* y, int* cx, int* cy,
                      bool resizeLeft, bool resizeTop, bool resizeRight, bool resizeBottom) {
        if (IsSnappingTemporarilyDisabled()) return;
        CalculateMetrics(hWnd);

        RECT sourceRect = {
            *x + windowBorderRect.left,
            *y + windowBorderRect.top,
            *x + *cx - windowBorderRect.right,
            *y + *cy - windowBorderRect.bottom
        };

        int newX = *x, newY = *y, newCx = *cx, newCy = *cy;

        int minW = GetSystemMetrics(SM_CXMINTRACK);
        int minH = GetSystemMetrics(SM_CYMINTRACK);

        // LEFT edge -> compare to other RIGHT edges
        if (resizeLeft) {
            long target = FindClosestTarget(magnetTargetsRight, sourceRect.left, sourceRect.top, sourceRect.bottom, magnetPixels);
            if (target != LONG_MAX) {
                int desiredLeft = (int)target - windowBorderRect.left;
                int delta = newX - desiredLeft;
                if (newCx + delta >= minW) {
                    newX = desiredLeft;
                    newCx += delta;
                }
            }
        }

        // RIGHT edge -> compare to other LEFT edges
        if (resizeRight) {
            long target = FindClosestTarget(magnetTargetsLeft, sourceRect.right, sourceRect.top, sourceRect.bottom, magnetPixels);
            if (target != LONG_MAX) {
                int desiredRight = (int)target + windowBorderRect.right;
                int newWidth = desiredRight - newX;
                if (newWidth >= minW) newCx = newWidth;
            }
        }

        // TOP edge -> compare to other BOTTOM edges
        if (resizeTop) {
            long target = FindClosestTarget(magnetTargetsBottom, sourceRect.top, sourceRect.left, sourceRect.right, magnetPixels);
            if (target != LONG_MAX) {
                int desiredTop = (int)target - windowBorderRect.top;
                int delta = newY - desiredTop;
                if (newCy + delta >= minH) {
                    newY = desiredTop;
                    newCy += delta;
                }
            }
        }

        // BOTTOM edge -> compare to other TOP edges
        if (resizeBottom) {
            long target = FindClosestTarget(magnetTargetsTop, sourceRect.bottom, sourceRect.left, sourceRect.right, magnetPixels);
            if (target != LONG_MAX) {
                int desiredBottom = (int)target + windowBorderRect.bottom;
                int newHeight = desiredBottom - newY;
                if (newHeight >= minH) newCy = newHeight;
            }
        }

        RECT titleProbe = {
            newX + windowBorderRect.left,
            newY + windowBorderRect.top,
            newX + newCx - windowBorderRect.right,
            newY + windowBorderRect.top + 1
        };
        if (IsRectInWorkArea(titleProbe)) { *x = newX; *y = newY; *cx = newCx; *cy = newCy; }
    }

private:
    UINT windowDpi = (UINT)-1;
    bool windowMaximized = false;
    RECT windowBorderRect{};

    int magnetPixels = 25;
    std::set<std::tuple<long, long, long>> magnetTargetsLeft;
    std::set<std::tuple<long, long, long>> magnetTargetsTop;
    std::set<std::tuple<long, long, long>> magnetTargetsRight;
    std::set<std::tuple<long, long, long>> magnetTargetsBottom;

    void CalculateMetrics(HWND hTargetWnd) {
        UINT prevWindowDpi = windowDpi;
        windowDpi = pGetDpiForWindow ? pGetDpiForWindow(hTargetWnd) : 0;

        bool prevWindowMaximized = windowMaximized;
        windowMaximized = IsMaximized(hTargetWnd);

        if (prevWindowDpi == windowDpi && prevWindowMaximized == windowMaximized) return;

        RECT rect, frame;
        if (GetWindowRect(hTargetWnd, &rect) && GetWindowFrameBounds(hTargetWnd, &frame)) {
            windowBorderRect.left   = frame.left - rect.left;
            windowBorderRect.top    = frame.top - rect.top;
            windowBorderRect.right  = rect.right - frame.right;
            windowBorderRect.bottom = rect.bottom - frame.bottom;
        }

        magnetPixels = g_settings.snapWindowsDistance;
        if (windowDpi) magnetPixels = MulDiv(magnetPixels, windowDpi, 96);
    }

    struct InitialWndEnumProcParam {
        HWND hTargetWnd = nullptr;
        std::vector<RECT> windowRects;
    };

    static BOOL CALLBACK InitialWndEnumProc(HWND hWnd, LPARAM lParam) {
        auto& param = *(InitialWndEnumProcParam*)lParam;
        if (hWnd == param.hTargetWnd) return TRUE;
        if (!IsWindowVisible(hWnd) || IsWindowCloaked(hWnd) || IsIconic(hWnd)) return TRUE;
        if (GetWindowLong(hWnd, GWL_EXSTYLE) & (WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW)) return TRUE;

        RECT rc; if (!GetWindowFrameBounds(hWnd, &rc)) return TRUE;
        if (rc.left >= rc.right || rc.top >= rc.bottom) return TRUE;

        param.windowRects.push_back(rc);
        return TRUE;
    }

    static BOOL CALLBACK InitialMonitorEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM lParam) {
        auto& wm = *(WindowMagnet*)lParam;
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(monitor, &mi);
        auto& rc = mi.rcWork;

        wm.magnetTargetsLeft.emplace(rc.right,  rc.top, rc.bottom);
        wm.magnetTargetsTop.emplace(rc.bottom,  rc.left, rc.right);
        wm.magnetTargetsRight.emplace(rc.left,  rc.top, rc.bottom);
        wm.magnetTargetsBottom.emplace(rc.top,  rc.left, rc.right);
        return TRUE;
    }

    static void RemoveOverlappedTargets(std::set<std::tuple<long, long, long>>& magnetTargets,
        long start, long end, long otherAxisStart, long otherAxisEnd) {
        for (auto it = magnetTargets.lower_bound({ start, otherAxisStart, otherAxisStart });
             it != magnetTargets.end();) {
            auto a = std::get<0>(*it);
            auto b = std::get<1>(*it);
            auto c = std::get<2>(*it);

            if (a > end || (a == end && b > otherAxisEnd)) break;

            if (otherAxisStart < c && otherAxisEnd > b) {
                it = magnetTargets.erase(it);
                if (otherAxisStart > b) magnetTargets.emplace(a, b, otherAxisStart);
                if (otherAxisEnd < c)   magnetTargets.emplace(a, otherAxisEnd, c);
            } else {
                ++it;
            }
        }
    }

    static long FindClosestTarget(const std::set<std::tuple<long, long, long>>& magnetTargets,
        long source, long otherAxisStart, long otherAxisEnd, int magnetPixels) {
        long target = LONG_MAX;
        long iterStart = source - magnetPixels;
        long iterEnd   = source + magnetPixels;

        for (auto it = magnetTargets.lower_bound({ iterStart, otherAxisStart, otherAxisStart });
             it != magnetTargets.end(); ++it) {
            auto a = std::get<0>(*it);
            auto b = std::get<1>(*it);
            auto c = std::get<2>(*it);

            if (a > iterEnd || (a == iterEnd && b > otherAxisEnd)) break;

            if (target != LONG_MAX) {
                if (a == target) continue;
                if (std::abs(source - a) >= std::abs(source - target)) break;
            }

            if (otherAxisStart < c && otherAxisEnd > b) target = a;
        }
        return target;
    }

    struct IsRectInWorkAreaMonitorEnumProcParam { const RECT* rc; bool inWorkArea; };
    static BOOL CALLBACK IsRectInWorkAreaMonitorEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM lParam) {
        auto& param = *(IsRectInWorkAreaMonitorEnumProcParam*)lParam;
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(monitor, &mi);
        auto& rcA = *param.rc; auto& rcB = mi.rcWork;
        if (rcA.left < rcB.right && rcA.right > rcB.left && rcA.top < rcB.bottom && rcA.bottom > rcB.top) {
            param.inWorkArea = true; return FALSE;
        }
        return TRUE;
    }
    static bool IsRectInWorkArea(const RECT& rc) {
        IsRectInWorkAreaMonitorEnumProcParam p{ &rc, false };
        EnumDisplayMonitors(nullptr, nullptr, IsRectInWorkAreaMonitorEnumProc, (LPARAM)&p);
        return p.inWorkArea;
    }

    static bool IsSnappingTemporarilyDisabled() {
        if (!g_settings.keysToDisableSnappingCtrl &&
            !g_settings.keysToDisableSnappingAlt &&
            !g_settings.keysToDisableSnappingShift) return false;
        return
            (!g_settings.keysToDisableSnappingCtrl || GetKeyState(VK_CONTROL) < 0) &&
            (!g_settings.keysToDisableSnappingAlt  || GetKeyState(VK_MENU)    < 0) &&
            (!g_settings.keysToDisableSnappingShift|| GetKeyState(VK_SHIFT)   < 0);
    }
};

class WindowMoving {
public:
    WindowMoving(HWND hTargetWnd) : windowMagnet(hTargetWnd) {}

    void PreProcessPos(HWND hTargetWnd, int* x, int* y, int* cx, int* cy) {
        MovingState state = GetCurrentMovingState(hTargetWnd, *x, *y);

        if (lastState && lastState->isMaximized == state.isMaximized &&
            lastState->isMinimized == state.isMinimized &&
            lastState->isArranged  == state.isArranged) {

            int lastDeltaX = GET_X_LPARAM(lastState->messagePos) - lastState->x;
            int lastDeltaY = GET_Y_LPARAM(lastState->messagePos) - lastState->y;

            int deltaX = GET_X_LPARAM(state.messagePos) - state.x;
            int deltaY = GET_Y_LPARAM(state.messagePos) - state.y;

            state.x -= lastDeltaX - deltaX;
            state.y -= lastDeltaY - deltaY;

            *x = state.x; *y = state.y;
        }

        lastState = state;
        windowMagnet.MagnetMove(hTargetWnd, x, y, cx, cy);
    }

    void ForgetLastPos() { lastState.reset(); }
    WindowMagnet& GetWindowMagnet() { return windowMagnet; }

private:
    struct MovingState {
        bool  isMinimized;
        bool  isMaximized;
        bool  isArranged;
        DWORD messagePos;
        int   x, y;
    };

    static MovingState GetCurrentMovingState(HWND hTargetWnd, int x, int y) {
        return MovingState{
            .isMinimized = !!IsMinimized(hTargetWnd), // FIX
            .isMaximized = !!IsMaximized(hTargetWnd), // FIX
            .isArranged  = pIsWindowArranged && !!pIsWindowArranged(hTargetWnd),
            .messagePos  = GetMessagePos(),
            .x = x, .y = y,
        };
    }

    std::optional<MovingState> lastState;
    WindowMagnet windowMagnet;
};

class WindowMove {
public:
    void Reset() { lastState.reset(); for (auto& s : snapshot) s = nullptr; }
    void UpdateWithNewPos(HWND hTargetWnd, int x, int y) {
        DWORD tick = GetTickCount();
        if (snapshot[0] && tick - snapshot[0]->tickCount >= 100) Reset();

        WindowState st = GetWindowState(hTargetWnd);
        if (lastState && (st.isMinimized != lastState->isMinimized ||
                          st.isMaximized != lastState->isMaximized ||
                          st.isArranged  != lastState->isArranged)) {
            Reset();
        }
        lastState = st;

        if (!snapshot[0]) snapshot[0] = &store[0];
        else if (!snapshot[1]) { snapshot[1] = &store[0]; snapshot[0] = &store[1]; }

        if (snapshot[1] && tick - snapshot[1]->tickCount >= 100) {
            if (!snapshot[2]) snapshot[2] = &store[2];
            auto* tmp = snapshot[2]; snapshot[2] = snapshot[1]; snapshot[1] = snapshot[0]; snapshot[0] = tmp;
        }

        snapshot[0]->tickCount = tick; snapshot[0]->x = x; snapshot[0]->y = y;
    }

    bool CompleteMove(int* x, int* y, double* vx, double* vy) {
        if (!snapshot[0] || !snapshot[1]) return false;
        if (GetTickCount() - snapshot[0]->tickCount >= 100) return false;

        auto* last = snapshot[0];
        auto* prev = snapshot[2] ? snapshot[2] : snapshot[1];
        int dt = last->tickCount - prev->tickCount;
        if (dt <= 0) return false;

        *x = last->x; *y = last->y;
        *vx = 1000.0 * (last->x - prev->x) / dt;
        *vy = 1000.0 * (last->y - prev->y) / dt;
        return true;
    }

private:
    struct WindowState { bool isMinimized; bool isMaximized; bool isArranged; };
    struct Snap { DWORD tickCount; int x, y; };

    static WindowState GetWindowState(HWND hTargetWnd) {
        return WindowState{
            .isMinimized = !!IsMinimized(hTargetWnd), // FIX
            .isMaximized = !!IsMaximized(hTargetWnd), // FIX
            .isArranged  = pIsWindowArranged && !!pIsWindowArranged(hTargetWnd),
        };
    }

    std::optional<WindowState> lastState;
    Snap store[3]; Snap* snapshot[3]{};
};

std::atomic<bool> g_uninitializing;
std::atomic<int>  g_hookRefCount;
thread_local std::unordered_map<HWND, WindowMoving> g_winMoving;
thread_local std::unordered_map<HWND, WindowMove>   g_winMove;

class WindowSlideTimer;
thread_local std::unordered_map<HWND, std::unique_ptr<WindowSlideTimer>> g_winSlideTimers;
thread_local std::unordered_map<HWND, UINT_PTR> g_activeTimers;

UINT g_unsubclassRegisteredMessage = RegisterWindowMessage(
    L"Windhawk_Unsubclass_slick-window-arrangement-resize-snap-v22");
std::mutex g_subclassedWindowsMutex;
std::unordered_set<HWND> g_subclassedWindows;

thread_local HHOOK g_callWndProcHook;
std::mutex g_allCallWndProcHooksMutex;
std::unordered_set<HHOOK> g_allCallWndProcHooks;

static auto hookRefCountScope() {
    g_hookRefCount++;
    return std::unique_ptr<decltype(g_hookRefCount), void(*)(decltype(g_hookRefCount)*)>{
        &g_hookRefCount, [](auto p){ (*p)--; }
    };
}

static void UnsubclassWindow(HWND hWnd) {
    RemoveWindowSubclass(hWnd, SubclassWndProc, 0);
    std::lock_guard<std::mutex> g(g_subclassedWindowsMutex);
    g_subclassedWindows.erase(hWnd);
}

static bool KillWindowSlideTimer(HWND hWnd) {
    auto it = g_activeTimers.find(hWnd);
    if (it == g_activeTimers.end()) return false;
    KillTimer(nullptr, it->second);
    g_activeTimers.erase(it);
    g_winSlideTimers.erase(hWnd);
    return true;
}

class WindowSlideTimer {
public:
    WindowSlideTimer(HWND hWnd, int cursorX, int cursorY, int x, int y, double vx, double vy,
                     std::optional<WindowMagnet> wm)
        : target(hWnd), cursorPoint{cursorX, cursorY}, x((double)x), y((double)y),
          velocityX(vx), velocityY(vy), windowMagnet(std::move(wm)) {
        HMONITOR monitor = MonitorFromPoint(cursorPoint, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) }; GetMonitorInfo(monitor, &mi);
        workArea = mi.rcWork;
        timerId = SetTimer(nullptr, 0, 10, WindowSlideTimerProc);
        g_activeTimers[hWnd] = timerId;
    }
    ~WindowSlideTimer(){ KillTimer(nullptr, timerId); }

    UINT_PTR Id() const { return timerId; }
    HWND Target() const { return target; }

    bool Next() {
        RECT rect; GetWindowRect(target, &rect);
        if (frame != 0) {
            if (lastX != rect.left || lastY != rect.top ||
                lastCx != (rect.right - rect.left) || lastCy != (rect.bottom - rect.top)) return false;
        }

        int prevX = (int)x, prevY = (int)y;
        x += velocityX / (1000.0 / 15.6);
        y += velocityY / (1000.0 / 15.6);
        int currentX = (int)x, currentY = (int)y;
        if (currentX == prevX && currentY == prevY) return false;

        POINT anchor{ currentX + cursorPoint.x, currentY + cursorPoint.y };
        if (!PtInRect(&workArea, anchor)) {
            bool foundNewMonitor = false;
            HMONITOR m = MonitorFromPoint(anchor, MONITOR_DEFAULTTONULL);
            if (m) {
                MONITORINFO mi = { sizeof(mi) }; GetMonitorInfo(m, &mi);
                if (PtInRect(&mi.rcWork, anchor)) { foundNewMonitor = true; workArea = mi.rcWork; }
            }
            if (!foundNewMonitor) {
                if (anchor.x < workArea.left)      { x = workArea.left  - cursorPoint.x;  velocityX = -velocityX * .05; }
                else if (anchor.x > workArea.right){ x = workArea.right - cursorPoint.x;  velocityX = -velocityX * .05; }
                if (anchor.y < workArea.top)       { y = workArea.top   - cursorPoint.y;  velocityY = -velocityY * .05; }
                else if (anchor.y > workArea.bottom){y = workArea.bottom- cursorPoint.y;  velocityY = -velocityY * .05; }
                currentX = (int)x; currentY = (int)y;
            }
        }

        int slowdown = g_settings.slidingAnimationSlowdown;
        if (slowdown < 1) slowdown = 1; else if (slowdown > 99) slowdown = 99;
        double mul = (100 - slowdown) / 100.0;
        velocityX *= mul; velocityY *= mul;

        if (windowMagnet) {
            int mx = currentX, my = currentY, cx = rect.right - rect.left, cy = rect.bottom - rect.top;
            windowMagnet->MagnetMove(target, &mx, &my, &cx, &cy);
            if (mx != currentX) { x = mx; currentX = mx; velocityX = 0.0; }
            if (my != currentY) { y = my; currentY = my; velocityY = 0.0; }
        }

        SetWindowPos(target, nullptr, currentX, currentY, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);

        lastX = currentX; lastY = currentY;
        lastCx = rect.right - rect.left; lastCy = rect.bottom - rect.top;
        frame++; return frame < 50;
    }

private:
    HWND target; UINT_PTR timerId; int frame = 0;
    POINT cursorPoint; RECT workArea{};
    double x, y, velocityX, velocityY;
    int lastX{}, lastY{}, lastCx{}, lastCy{};
    std::optional<WindowMagnet> windowMagnet;
};

static void CALLBACK WindowSlideTimerProc(HWND, UINT, UINT_PTR idTimer, DWORD) {
    for (auto it = g_winSlideTimers.begin(); it != g_winSlideTimers.end(); ++it) {
        if (it->second && it->second->Id() == idTimer) {
            auto hTarget = it->first;
            auto& timer = it->second;
            if (!timer->Next()) {
                g_activeTimers.erase(hTarget);
                g_winSlideTimers.erase(it);
                UnsubclassWindow(hTarget);
            }
            return;
        }
    }
}

// New: handle WM_SIZING for reliable live snapping
static void OnSizing(HWND hWnd, UINT edge, RECT* rc) {
    auto it = g_winMoving.find(hWnd);
    if (it == g_winMoving.end()) return;

    int x = rc->left, y = rc->top;
    int cx = rc->right - rc->left;
    int cy = rc->bottom - rc->top;

    bool left   = edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT;
    bool right  = edge == WMSZ_RIGHT || edge == WMSZ_TOPRIGHT || edge == WMSZ_BOTTOMRIGHT;
    bool top    = edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT;
    bool bottom = edge == WMSZ_BOTTOM || edge == WMSZ_BOTTOMLEFT || edge == WMSZ_BOTTOMRIGHT;

    auto& magnet = it->second.GetWindowMagnet();
    magnet.MagnetResize(hWnd, &x, &y, &cx, &cy, left, top, right, bottom);

    rc->left = x; rc->top = y; rc->right = x + cx; rc->bottom = y + cy;
}

static void OnEnterSizeMove(HWND hWnd) {
    KillWindowSlideTimer(hWnd);
    // Always setup moving so resize snapping is available
    g_winMoving.try_emplace(hWnd, hWnd);
    if (g_settings.slidingAnimation) g_winMove.try_emplace(hWnd);
}

static void OnExitSizeMove(HWND hWnd) {
    bool unsubclass = true;
    auto itMove = g_winMove.find(hWnd);
    auto itMoving = g_winMoving.find(hWnd);

    if (itMove != g_winMove.end()) {
        auto& wm = itMove->second;
        int x, y; double vx, vy;
        if (wm.CompleteMove(&x, &y, &vx, &vy)) {
            DWORD mp = GetMessagePos();
            auto optMagnet = (itMoving != g_winMoving.end())
                ? std::optional<WindowMagnet>(std::move(itMoving->second.GetWindowMagnet()))
                : std::optional<WindowMagnet>(WindowMagnet(hWnd));
            g_winSlideTimers[hWnd] = std::make_unique<WindowSlideTimer>(
                hWnd, GET_X_LPARAM(mp) - x, GET_Y_LPARAM(mp) - y, x, y, vx, vy,
                g_settings.snapWindowsWhenSliding ? optMagnet : std::nullopt
            );
            unsubclass = false;
        }
        g_winMove.erase(itMove);
    }
    if (itMoving != g_winMoving.end()) g_winMoving.erase(itMoving);
    if (unsubclass) UnsubclassWindow(hWnd);
}

static void OnWindowPosChanging(HWND hWnd, WINDOWPOS* windowPos) {
    if ((windowPos->flags & (SWP_NOSIZE | SWP_NOMOVE)) == (SWP_NOSIZE | SWP_NOMOVE)) return;
    RECT rc; if (!GetWindowRect(hWnd, &rc)) return;

    int x  = (windowPos->flags & SWP_NOMOVE) ? rc.left : windowPos->x;
    int y  = (windowPos->flags & SWP_NOMOVE) ? rc.top  : windowPos->y;
    int cx = (windowPos->flags & SWP_NOSIZE) ? (rc.right - rc.left) : windowPos->cx;
    int cy = (windowPos->flags & SWP_NOSIZE) ? (rc.bottom - rc.top) : windowPos->cy;

    bool posChanged  = (rc.left != x || rc.top != y);
    bool sizeChanged = ((rc.right - rc.left) != cx || (rc.bottom - rc.top) != cy);
    if (!posChanged && !sizeChanged) return;

    auto it = g_winMoving.find(hWnd);
    if (it == g_winMoving.end()) return;

    auto& moving = it->second;
    if (posChanged && !sizeChanged) {
        moving.PreProcessPos(hWnd, &x, &y, &cx, &cy);
    } else if (sizeChanged) {
        moving.ForgetLastPos();

        bool resizeLeft   = posChanged && (x != rc.left);
        bool resizeTop    = posChanged && (y != rc.top);
        bool resizeRight  = (!posChanged) && (cx != (rc.right - rc.left));
        bool resizeBottom = (!posChanged) && (cy != (rc.bottom - rc.top));

        if (!resizeLeft && !resizeRight)   resizeRight  = cx != (rc.right - rc.left);
        if (!resizeTop && !resizeBottom)   resizeBottom = cy != (rc.bottom - rc.top);

        auto& magnet = moving.GetWindowMagnet();
        magnet.MagnetResize(hWnd, &x, &y, &cx, &cy, resizeLeft, resizeTop, resizeRight, resizeBottom);
    }

    if (!(windowPos->flags & SWP_NOMOVE)) { windowPos->x = x; windowPos->y = y; }
    if (!(windowPos->flags & SWP_NOSIZE)) { windowPos->cx = cx; windowPos->cy = cy; }
}

static void OnWindowPosChanged(HWND hWnd, const WINDOWPOS* windowPos) {
    auto it = g_winMove.find(hWnd);
    if (it == g_winMove.end()) {
        if ((windowPos->flags & SWP_STATECHANGED) || (windowPos->flags & 0x00300000)) {
            if (KillWindowSlideTimer(hWnd)) UnsubclassWindow(hWnd);
        }
        return;
    }

    auto& wm = it->second;
    if ((windowPos->flags & SWP_NOMOVE) || !(windowPos->flags & SWP_NOSIZE)) { wm.Reset(); return; }
    wm.UpdateWithNewPos(hWnd, windowPos->x, windowPos->y);
}

static void OnSysCommand(HWND hWnd, WPARAM command) {
    switch (command) {
    case SC_SIZE: case SC_MOVE: case SC_MINIMIZE: case SC_MAXIMIZE:
    case SC_CLOSE: case SC_MOUSEMENU: case SC_KEYMENU: case SC_RESTORE:
        if (KillWindowSlideTimer(hWnd)) UnsubclassWindow(hWnd);
        break;
    }
}

static void OnNcDestroy(HWND hWnd) { KillWindowSlideTimer(hWnd); UnsubclassWindow(hWnd); }

static LRESULT CALLBACK SubclassWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    auto scope = hookRefCountScope();
    switch (uMsg) {
    case WM_ENTERSIZEMOVE:     OnEnterSizeMove(hWnd); break;
    case WM_EXITSIZEMOVE:      OnExitSizeMove(hWnd);  break;
    case WM_WINDOWPOSCHANGING: OnWindowPosChanging(hWnd, (WINDOWPOS*)lParam); break;
    case WM_WINDOWPOSCHANGED:  OnWindowPosChanged(hWnd, (const WINDOWPOS*)lParam); break;
    case WM_SIZING:            OnSizing(hWnd, (UINT)wParam, (RECT*)lParam); break; // NEW
    case WM_SYSCOMMAND:        OnSysCommand(hWnd, wParam); break;
    case WM_NCDESTROY:         OnNcDestroy(hWnd); break;
    default:
        if (uMsg == g_unsubclassRegisteredMessage) {
            KillWindowSlideTimer(hWnd);
            RemoveWindowSubclass(hWnd, SubclassWndProc, 0);
        }
        break;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

static LRESULT CALLBACK CallWndProc(int nCode, WPARAM, LPARAM lParam) {
    auto scope = hookRefCountScope();
    if (nCode != HC_ACTION) return CallNextHookEx(nullptr, nCode, 0, lParam);

    const CWPSTRUCT* cwp = (const CWPSTRUCT*)lParam;
    if (cwp->message == WM_ENTERSIZEMOVE) {
        WCHAR className[32] = L"";
        if (GetClassName(GetAncestor(cwp->hwnd, GA_ROOT), className, ARRAYSIZE(className)) &&
            (_wcsicmp(className, L"Shell_TrayWnd") == 0 || _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0)) {
            // skip taskbar
        } else {
            std::lock_guard<std::mutex> g(g_subclassedWindowsMutex);
            if (!g_uninitializing && SetWindowSubclass(cwp->hwnd, SubclassWndProc, 0, 0)) {
                g_subclassedWindows.insert(cwp->hwnd);
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, 0, lParam);
}

static void SetWindowHookForUiThreadIfNeeded(HWND hWnd) {
    if (!g_callWndProcHook && IsWindowVisible(GetAncestor(hWnd, GA_ROOT))) {
        std::lock_guard<std::mutex> g(g_allCallWndProcHooksMutex);
        if (!g_uninitializing) {
            DWORD tid = GetCurrentThreadId();
            HHOOK hook = SetWindowsHookEx(WH_CALLWNDPROC, CallWndProc, nullptr, tid);
            if (hook) { g_callWndProcHook = hook; g_allCallWndProcHooks.insert(hook); }
        }
    }
}

using DispatchMessageA_t = decltype(&DispatchMessageA);
static DispatchMessageA_t pDispatchMessageA;
static LRESULT WINAPI DispatchMessageAHook(const MSG* lpMsg) {
    auto scope = hookRefCountScope();
    if (lpMsg && lpMsg->hwnd) SetWindowHookForUiThreadIfNeeded(lpMsg->hwnd);
    return pDispatchMessageA(lpMsg);
}

using DispatchMessageW_t = decltype(&DispatchMessageW);
static DispatchMessageW_t pDispatchMessageW;
static LRESULT WINAPI DispatchMessageWHook(const MSG* lpMsg) {
    auto scope = hookRefCountScope();
    if (lpMsg && lpMsg->hwnd) SetWindowHookForUiThreadIfNeeded(lpMsg->hwnd);
    return pDispatchMessageW(lpMsg);
}

using IsDialogMessageA_t = decltype(&IsDialogMessageA);
static IsDialogMessageA_t pIsDialogMessageA;
static LRESULT WINAPI IsDialogMessageAHook(HWND hDlg, LPMSG lpMsg) {
    auto scope = hookRefCountScope();
    if (hDlg) SetWindowHookForUiThreadIfNeeded(hDlg);
    return pIsDialogMessageA(hDlg, lpMsg);
}

using IsDialogMessageW_t = decltype(&IsDialogMessageW);
static IsDialogMessageW_t pIsDialogMessageW;
static LRESULT WINAPI IsDialogMessageWHook(HWND hDlg, LPMSG lpMsg) {
    auto scope = hookRefCountScope();
    if (hDlg) SetWindowHookForUiThreadIfNeeded(hDlg);
    return pIsDialogMessageW(hDlg, lpMsg);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
    case DLL_THREAD_DETACH:
        if (g_callWndProcHook) {
            std::lock_guard<std::mutex> g(g_allCallWndProcHooksMutex);
            auto it = g_allCallWndProcHooks.find(g_callWndProcHook);
            if (it != g_allCallWndProcHooks.end()) {
                UnhookWindowsHookEx(g_callWndProcHook);
                g_allCallWndProcHooks.erase(it);
            }
        }
        break;
    }
    return TRUE;
}

static void LoadSettings() {
    g_settings.snapWindowsWhenDragging = Wh_GetIntSetting(L"SnapWindowsWhenDragging");
    g_settings.snapWindowsDistance = Wh_GetIntSetting(L"SnapWindowsDistance");
    g_settings.keysToDisableSnappingCtrl  = Wh_GetIntSetting(L"KeysToDisableSnapping.Ctrl");
    g_settings.keysToDisableSnappingAlt   = Wh_GetIntSetting(L"KeysToDisableSnapping.Alt");
    g_settings.keysToDisableSnappingShift = Wh_GetIntSetting(L"KeysToDisableSnapping.Shift");
    g_settings.slidingAnimation           = Wh_GetIntSetting(L"SlidingAnimation");
    g_settings.snapWindowsWhenSliding     = Wh_GetIntSetting(L"SnapWindowsWhenSliding");
    g_settings.slidingAnimationSlowdown   = Wh_GetIntSetting(L"SlidingAnimationSlowdown");
}

BOOL Wh_ModInit() {
    LoadSettings();

    HMODULE hUser32 = LoadLibrary(L"user32.dll");
    if (hUser32) {
        pGetThreadDpiAwarenessContext = (GetThreadDpiAwarenessContext_t)GetProcAddress(hUser32, "GetThreadDpiAwarenessContext");
        pSetThreadDpiAwarenessContext = (SetThreadDpiAwarenessContext_t)GetProcAddress(hUser32, "SetThreadDpiAwarenessContext");
        pGetAwarenessFromDpiAwarenessContext = (GetAwarenessFromDpiAwarenessContext_t)GetProcAddress(hUser32, "GetAwarenessFromDpiAwarenessContext");
        pGetDpiForSystem = (GetDpiForSystem_t)GetProcAddress(hUser32, "GetDpiForSystem");
        pGetDpiForWindow = (GetDpiForWindow_t)GetProcAddress(hUser32, "GetDpiForWindow");
        pIsWindowArranged = (IsWindowArranged_t)GetProcAddress(hUser32, "IsWindowArranged");
    }

    HMODULE hShcore = LoadLibrary(L"shcore.dll");
    if (hShcore) pGetDpiForMonitor = (GetDpiForMonitor_t)GetProcAddress(hShcore, "GetDpiForMonitor");

    Wh_SetFunctionHook((void*)DispatchMessageA, (void*)DispatchMessageAHook, (void**)&pDispatchMessageA);
    Wh_SetFunctionHook((void*)DispatchMessageW, (void*)DispatchMessageWHook, (void**)&pDispatchMessageW);
    Wh_SetFunctionHook((void*)IsDialogMessageA, (void*)IsDialogMessageAHook, (void**)&pIsDialogMessageA);
    Wh_SetFunctionHook((void*)IsDialogMessageW, (void*)IsDialogMessageWHook, (void**)&pIsDialogMessageW);

    return TRUE;
}

void Wh_ModUninit() {
    g_uninitializing = true;

    std::unordered_set<HWND> subclassed;
    {
        std::lock_guard<std::mutex> g(g_subclassedWindowsMutex);
        subclassed = std::move(g_subclassedWindows);
        g_subclassedWindows.clear();
    }
    for (HWND hWnd : subclassed) {
        SendMessage(hWnd, g_unsubclassRegisteredMessage, 0, 0);
    }

    {
        std::lock_guard<std::mutex> g(g_allCallWndProcHooksMutex);
        for (HHOOK hook : g_allCallWndProcHooks) UnhookWindowsHookEx(hook);
        g_allCallWndProcHooks.clear();
    }

    while (g_hookRefCount > 0) Sleep(200);
}

void Wh_ModSettingsChanged() { LoadSettings(); }
