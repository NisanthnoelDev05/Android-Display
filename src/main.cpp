#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <gdiplus.h>
#include <strsafe.h>

#include "Theme.h"
#include "AdbClient.h"
#include "MediaPipeline.h"
#include "Dialogs.h"
#include "ResourceIds.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr UINT WM_APP_DEVICES_UPDATED = WM_APP + 104;
constexpr UINT WM_APP_STATUS_MSG      = WM_APP + 105;
constexpr UINT_PTR TIMER_REFRESH_DEVS = 1;

// Android KeyEvents
constexpr int AKEYCODE_HOME        = 3;
constexpr int AKEYCODE_BACK        = 4;
constexpr int AKEYCODE_VOLUME_UP   = 24;
constexpr int AKEYCODE_VOLUME_DOWN = 25;
constexpr int AKEYCODE_POWER       = 26;
constexpr int AKEYCODE_DEL         = 67;
constexpr int AKEYCODE_ENTER       = 66;
constexpr int AKEYCODE_DPAD_UP     = 19;
constexpr int AKEYCODE_DPAD_DOWN   = 20;
constexpr int AKEYCODE_DPAD_LEFT   = 21;
constexpr int AKEYCODE_DPAD_RIGHT  = 22;
constexpr int AKEYCODE_APP_SWITCH  = 187;

enum ControlId {
    IDC_BTN_DEVICES    = 1001,
    IDC_BTN_REFRESH    = 1002,
    IDC_BTN_WIFI       = 1003,
    IDC_BTN_SETTINGS   = 1004,
    IDC_BTN_START_STOP = 1005,
    IDC_BTN_NAV_BACK   = 1006,
    IDC_BTN_NAV_HOME   = 1007,
    IDC_BTN_NAV_RECENTS= 1008,
    IDC_BTN_VOL_DOWN   = 1009,
    IDC_BTN_VOL_UP     = 1010,
    IDC_BTN_POWER      = 1011,
    IDC_BTN_ROTATE     = 1012,
    IDC_BTN_SCREENSHOT = 1013,
    IDC_BTN_RECORD     = 1014,
    IDC_BTN_CAPTURE    = 1015,
    IDC_BTN_FOCUS      = 1016
};

struct AppState {
    HWND hwndMain = nullptr;
    HWND hwndDeviceDrop = nullptr;
    bool isDarkMode = false;
    ThemePalette pal;

    HFONT hFontTitle = nullptr;
    HFONT hFontSubtitle = nullptr;
    HFONT hFontBody = nullptr;
    HFONT hFontSmall = nullptr;

    AdbClient adb;
    AppSettings settings;
    MirrorSession session;
    std::atomic<bool> isRefreshingDevices{ false };

    std::vector<AdbDevice> devices;
    int selectedDeviceIdx = -1;

    RECT rcTopBar = {};
    RECT rcStage = {};
    RECT rcBottomBar = {};
    RECT rcVideoFrame = {};

    std::shared_ptr<const DecodedFrame> currentFrame;
    bool isPointerDown = false;
    bool isZoomDragging = false;
    int lastHoverX = -1;
    int lastHoverY = -1;
    float camZoom = 1.0f;
    RECT rcZoomSlider = {};
    std::wstring transientStatus;
    int lastTelW = 0;
    int lastTelH = 0;
    int lastTelFps = -1;
    bool lastTelAudio = false;
    std::wstring lastTelStatus;

    HDC hdcBack = nullptr;
    HBITMAP hbmBack = nullptr;
    int backW = 0;
    int backH = 0;

    HWND hwndVideo = nullptr;
    RECT rcPlacedVideo = {};
    bool videoShown = false;

    std::mutex renderMutex;
    HANDLE hFrameEvent = nullptr;
    std::atomic<bool> renderStop{ false };
    std::thread renderThread;
    std::atomic<int> targetDrawW{ 0 };
    std::atomic<int> targetDrawH{ 0 };
    std::atomic<bool> videoVisible{ false };

    ID3D11Device* d3dDevice = nullptr;
    ID2D1Factory1* d2dFactory = nullptr;
    ID2D1Device* d2dDevice = nullptr;
    ID2D1DeviceContext* d2dCtx = nullptr;
    ID2D1Bitmap1* d2dSrcBmp = nullptr;
    ID2D1Bitmap1* d2dTargetBmp = nullptr;
    IDXGISwapChain1* dxgiSwap = nullptr;
    int d2dBmpW = 0;
    int d2dBmpH = 0;
    int d2dDstW = 0;
    int d2dDstH = 0;

    bool isFullscreen = false;
    bool isImmersive = false;
    bool isImmersiveHovered = false;
    bool isImmersivePressed = false;
    HWND hwndImmersiveBtn = nullptr;
    RECT savedNormalRect = {};
    int lastImmFrameW = 0;
    int lastImmFrameH = 0;
    bool screenPowerOn = true;
    WINDOWPLACEMENT savedPlacement = { sizeof(WINDOWPLACEMENT) };

    std::unordered_map<int, HWND> buttons;
    std::unordered_map<int, bool> btnHovered;
};

LRESULT CALLBACK FluentBtnSubclass(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    auto* state = reinterpret_cast<AppState*>(dwRefData);
    int id = GetDlgCtrlID(hwnd);

    switch (uMsg) {
    case WM_MOUSEMOVE:
        if (state && !state->btnHovered[id]) {
            state->btnHovered[id] = true;
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        if (state && state->btnHovered[id]) {
            state->btnHovered[id] = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, FluentBtnSubclass, uIdSubclass);
        break;
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

void CreateFonts(HWND hwnd, AppState* s) {
    if (s->hFontTitle)    DeleteObject(s->hFontTitle);
    if (s->hFontSubtitle) DeleteObject(s->hFontSubtitle);
    if (s->hFontBody)     DeleteObject(s->hFontBody);
    if (s->hFontSmall)    DeleteObject(s->hFontSmall);

    s->hFontTitle    = CreateModernFont(hwnd, 20, FW_NORMAL);
    s->hFontSubtitle = CreateModernFont(hwnd, 11, FW_NORMAL);
    s->hFontBody     = CreateModernFont(hwnd, 10, FW_NORMAL);
    s->hFontSmall    = CreateModernFont(hwnd, 9, FW_NORMAL);
}

void ApplyTheme(HWND hwnd, AppState* s) {
    s->isDarkMode = IsSystemInDarkMode();
    s->pal = GetThemePalette(s->isDarkMode);
    ApplyDwmWindowTheme(hwnd, s->isDarkMode);

    if (s->hwndDeviceDrop) {
        InvalidateRect(s->hwndDeviceDrop, nullptr, TRUE);
    }
    InvalidateRect(hwnd, nullptr, TRUE);
    for (auto& kv : s->buttons) {
        InvalidateRect(kv.second, nullptr, TRUE);
    }
}

std::vector<std::wstring> BuildDeviceDropdownItems(const AppState* s) {
    std::vector<std::wstring> items;
    for (const auto& d : s->devices) {
        std::wstring label = d.DisplayName() + (d.isWireless ? L" (Wi-Fi)" : L" (USB)");
        if (!d.IsOnline()) {
            label += L" - " + d.state;
        }
        items.push_back(std::move(label));
    }
    return items;
}

void SyncDeviceCombo(AppState* s) {
    if (!s->hwndDeviceDrop) return;
    if (s->devices.empty()) {
        SetWindowTextW(s->hwndDeviceDrop, L"No devices");
        EnableWindow(s->hwndDeviceDrop, FALSE);
        InvalidateRect(s->hwndDeviceDrop, nullptr, FALSE);
        return;
    }

    auto items = BuildDeviceDropdownItems(s);
    int sel = (s->selectedDeviceIdx >= 0 && s->selectedDeviceIdx < static_cast<int>(items.size()))
        ? s->selectedDeviceIdx : 0;
    SetWindowTextW(s->hwndDeviceDrop, items[sel].c_str());
    EnableWindow(s->hwndDeviceDrop, s->session.IsRunning() ? FALSE : TRUE);
    InvalidateRect(s->hwndDeviceDrop, nullptr, FALSE);
}

void TriggerAsyncDeviceRefresh(AppState* s) {
    if (s->isRefreshingDevices.exchange(true)) return;
    HWND hwnd = s->hwndMain;
    AdbClient adbCopy = s->adb;

    std::thread([hwnd, adbCopy]() {
        auto devs = adbCopy.ListDevices();
        for (auto& d : devs) {
            if (d.IsOnline()) {
                adbCopy.PopulateDeviceDetails(d);
            }
        }
        auto* payload = new std::vector<AdbDevice>(std::move(devs));
        if (!PostMessageW(hwnd, WM_APP_DEVICES_UPDATED, 0, reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    }).detach();
}

void UpdateImmersiveButtonVisual(HWND hwnd, AppState* s) {
    if (!hwnd || !s) return;
    RECT rc = {};
    GetWindowRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    void* pvBits = nullptr;
    HBITMAP hbm = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pvBits, nullptr, 0);
    if (hbm && pvBits) {
        HBITMAP hbmOld = static_cast<HBITMAP>(SelectObject(hdcMem, hbm));
        std::memset(pvBits, 0, static_cast<size_t>(w) * static_cast<size_t>(h) * 4);

        {
            Gdiplus::Graphics g(hdcMem);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

            // Alpha=1 keeps the entire 46x30 hit area clickable while 100% transparent to Mica
            Gdiplus::Color bgCol(1, 0, 0, 0);
            if (s->isImmersivePressed) {
                bgCol = s->isDarkMode ? Gdiplus::Color(18, 255, 255, 255) : Gdiplus::Color(28, 0, 0, 0);
            } else if (s->isImmersiveHovered) {
                bgCol = s->isDarkMode ? Gdiplus::Color(28, 255, 255, 255) : Gdiplus::Color(16, 0, 0, 0);
            }
            Gdiplus::SolidBrush bgBr(bgCol);
            g.FillRectangle(&bgBr, 0, 0, w, h);

            UINT dpi = GetWindowDpi(s->hwndMain);
            float sz = static_cast<float>(ScaleDpi(10, dpi));
            float cx = w * 0.5f;
            float cy = h * 0.5f;
            float half = sz * 0.5f;

            COLORREF fg = s->isDarkMode ? RGB(255, 255, 255) : RGB(27, 27, 27);
            Gdiplus::Pen pen(ToGdipColor(fg), 1.2f);
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            pen.SetLineJoin(Gdiplus::LineJoinRound);

            float l = cx - half, r = cx + half, t = cy - half, b = cy + half;
            float gap = sz * 0.14f;
            float head = sz * 0.34f;
            if (!s->isImmersive) {
                // Outward diagonal arrows (top-right and bottom-left)
                g.DrawLine(&pen, cx + gap, cy - gap, r, t);
                g.DrawLine(&pen, r - head, t, r, t);
                g.DrawLine(&pen, r, t, r, t + head);

                g.DrawLine(&pen, cx - gap, cy + gap, l, b);
                g.DrawLine(&pen, l, b - head, l, b);
                g.DrawLine(&pen, l, b, l + head, b);
            } else {
                // Inward diagonal arrows pointing toward center
                float tipX1 = cx + gap * 0.8f, tipY1 = cy - gap * 0.8f;
                g.DrawLine(&pen, r, t, tipX1, tipY1);
                g.DrawLine(&pen, tipX1, tipY1 - head, tipX1, tipY1);
                g.DrawLine(&pen, tipX1, tipY1, tipX1 + head, tipY1);

                float tipX2 = cx - gap * 0.8f, tipY2 = cy + gap * 0.8f;
                g.DrawLine(&pen, l, b, tipX2, tipY2);
                g.DrawLine(&pen, tipX2 - head, tipY2, tipX2, tipY2);
                g.DrawLine(&pen, tipX2, tipY2, tipX2, tipY2 + head);
            }
        }

        POINT ptDst = { rc.left, rc.top };
        SIZE szWin = { w, h };
        POINT ptSrc = { 0, 0 };
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(hwnd, hdcScreen, &ptDst, &szWin, hdcMem, &ptSrc, 0, &bf, ULW_ALPHA);

        SelectObject(hdcMem, hbmOld);
        DeleteObject(hbm);
    }
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
}

void ResizeWindowToAspect(HWND hwnd, AppState* s) {
    if (!s || !s->isImmersive || s->isFullscreen || IsIconic(hwnd) || IsZoomed(hwnd)) return;
    int fw = (s->currentFrame && s->currentFrame->width > 0) ? s->currentFrame->width : 1080;
    int fh = (s->currentFrame && s->currentFrame->height > 0) ? s->currentFrame->height : 2340;
    s->lastImmFrameW = fw;
    s->lastImmFrameH = fh;

    RECT rcWork = {};
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        rcWork = mi.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
    }

    RECT rcClient = {};
    GetClientRect(hwnd, &rcClient);
    UINT dpi = GetWindowDpi(hwnd);

    // Size to fit monitor work area cleanly (like scrcpy), preserving exact frame aspect ratio
    RECT rcWin = {};
    GetWindowRect(hwnd, &rcWin);
    int ncW = (rcWin.right - rcWin.left) - (rcClient.right - rcClient.left);
    int ncH = (rcWin.bottom - rcWin.top) - (rcClient.bottom - rcClient.top);
    int maxClientH = std::max(ScaleDpi(320, dpi), static_cast<int>((rcWork.bottom - rcWork.top) - ncH - ScaleDpi(16, dpi)));
    int maxClientW = std::max(ScaleDpi(260, dpi), static_cast<int>((rcWork.right - rcWork.left) - ncW - ScaleDpi(16, dpi)));

    int targetH = maxClientH;
    int targetW = (targetH * fw) / fh;
    if (targetW > maxClientW) {
        targetW = maxClientW;
        targetH = (targetW * fh) / fw;
    }

    int winW = targetW + ncW;
    int winH = targetH + ncH;
    int cx = (rcWin.left + rcWin.right) / 2;
    int cy = (rcWin.top + rcWin.bottom) / 2;
    int newX = std::clamp<int>(cx - winW / 2, rcWork.left, std::max<int>(rcWork.left, rcWork.right - winW));
    int newY = std::clamp<int>(cy - winH / 2, rcWork.top, std::max<int>(rcWork.top, rcWork.bottom - winH));

    SetWindowPos(hwnd, nullptr, newX, newY, winW, winH, SWP_NOZORDER | SWP_NOACTIVATE);
}

void PositionImmersiveButton(HWND hwndMain, AppState* s) {
    if (!s || !s->hwndImmersiveBtn) return;
    if (IsIconic(hwndMain) || !IsWindowVisible(hwndMain) || s->isFullscreen) {
        ShowWindow(s->hwndImmersiveBtn, SW_HIDE);
        return;
    }

    RECT rcWin = {};
    GetWindowRect(hwndMain, &rcWin);

    RECT rcCap = {};
    HRESULT hr = DwmGetWindowAttribute(hwndMain, DWMWA_CAPTION_BUTTON_BOUNDS, &rcCap, sizeof(rcCap));
    UINT dpi = GetWindowDpi(hwndMain);
    int btnW = ScaleDpi(46, dpi);
    int btnH = ScaleDpi(30, dpi);
    int x = 0;
    int y = 0;

    if (SUCCEEDED(hr) && (rcCap.right > rcCap.left) && (rcCap.bottom > rcCap.top)) {
        btnW = std::max(btnW, static_cast<int>((rcCap.right - rcCap.left) / 3));
        btnH = rcCap.bottom - rcCap.top;
        x = rcWin.left + rcCap.left - btnW;
        y = rcWin.top + rcCap.top;
    } else {
        int borderX = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
        int borderY = GetSystemMetrics(SM_CYFRAME);
        x = rcWin.right - borderX - btnW * 4;
        y = rcWin.top + borderY;
    }

    if (IsZoomed(hwndMain)) {
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(MonitorFromWindow(hwndMain, MONITOR_DEFAULTTONEAREST), &mi)) {
            POINT ptClient = { 0, 0 };
            ClientToScreen(hwndMain, &ptClient);
            y = mi.rcWork.top;
            if (ptClient.y > y) {
                btnH = ptClient.y - y;
            }
        }
    }

    SetWindowPos(
        s->hwndImmersiveBtn,
        HWND_TOP,
        x, y, btnW, btnH,
        SWP_NOACTIVATE | SWP_SHOWWINDOW
    );
    UpdateImmersiveButtonVisual(s->hwndImmersiveBtn, s);

    if (s->isImmersive) {
        std::wstring fullTitle = L"Android Display";
        if (s->selectedDeviceIdx >= 0 && s->selectedDeviceIdx < static_cast<int>(s->devices.size())) {
            fullTitle = s->devices[s->selectedDeviceIdx].DisplayName();
        }
        int availW = (x - rcWin.left) - ScaleDpi(44, dpi);
        std::wstring targetTitle;
        if (availW >= ScaleDpi(36, dpi)) {
            wchar_t buf[256] = {};
            wcsncpy_s(buf, fullTitle.c_str(), _TRUNCATE);
            HDC hdc = GetDC(hwndMain);
            HFONT hCapFont = CreateModernFont(hwndMain, 9, FW_NORMAL);
            HFONT hOld = static_cast<HFONT>(SelectObject(hdc, hCapFont));
            RECT rcCalc = { 0, 0, availW, ScaleDpi(24, dpi) };
            DrawTextExW(hdc, buf, -1, &rcCalc, DT_SINGLELINE | DT_END_ELLIPSIS | DT_MODIFYSTRING | DT_NOPREFIX, nullptr);
            SelectObject(hdc, hOld);
            DeleteObject(hCapFont);
            ReleaseDC(hwndMain, hdc);
            targetTitle = buf;
        }
        wchar_t curBuf[256] = {};
        GetWindowTextW(hwndMain, curBuf, ARRAYSIZE(curBuf));
        if (targetTitle != curBuf) {
            SetWindowTextW(hwndMain, targetTitle.c_str());
        }
    }
}

LRESULT CALLBACK ImmersiveBtnWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    auto* s = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (uMsg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
        if (s && !s->isImmersiveHovered) {
            s->isImmersiveHovered = true;
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            UpdateImmersiveButtonVisual(hwnd, s);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (s && s->isImmersiveHovered) {
            s->isImmersiveHovered = false;
            s->isImmersivePressed = false;
            UpdateImmersiveButtonVisual(hwnd, s);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (s) {
            s->isImmersivePressed = true;
            SetCapture(hwnd);
            UpdateImmersiveButtonVisual(hwnd, s);
        }
        return 0;
    case WM_LBUTTONUP:
        if (s && s->isImmersivePressed) {
            s->isImmersivePressed = false;
            ReleaseCapture();
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (PtInRect(&rc, pt)) {
                SendMessageW(s->hwndMain, WM_COMMAND, MAKEWPARAM(9901, BN_CLICKED), 0);
            }
            UpdateImmersiveButtonVisual(hwnd, s);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

void LayoutControls(HWND hwnd, AppState* s) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    UINT dpi = GetWindowDpi(hwnd);

    int pad      = ScaleDpi(16, dpi);
    int gap      = ScaleDpi(8, dpi);
    bool imm     = s->isImmersive;
    int topBarH  = imm ? 0 : ScaleDpi(52, dpi);
    int ctrlH    = ScaleDpi(32, dpi);
    int topY     = (topBarH - ctrlH) / 2;
    bool mirroring = s->session.IsRunning();
    int botBarH  = (mirroring && !imm) ? ScaleDpi(44, dpi) : 0;

    s->rcTopBar    = { 0, 0, w, topBarH };
    s->rcBottomBar = { 0, h - botBarH, w, h };
    s->rcStage     = { 0, imm ? 0 : (topBarH + 1), w, h - botBarH - ((mirroring && !imm) ? 1 : 0) };

    HDWP hdwp = BeginDeferWindowPos(20);
    auto deferCtrl = [&](HWND hCtrl, int x, int y, int cw, int ch, bool show, bool move) {
        if (!hCtrl) return;
        UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
        if (!move) flags |= SWP_NOMOVE | SWP_NOSIZE;
        if (hdwp) {
            HDWP next = DeferWindowPos(hdwp, hCtrl, nullptr, x, y, cw, ch, flags);
            if (next) hdwp = next;
            else {
                SetWindowPos(hCtrl, nullptr, x, y, cw, ch, flags);
            }
        } else {
            SetWindowPos(hCtrl, nullptr, x, y, cw, ch, flags);
        }
    };

    // Top bar: Left group (Combo, Refresh, Wi-Fi), Right group (Settings, Start/Stop)
    if (!imm) {
        int startW    = ScaleDpi(88, dpi);
        int settingsW = ScaleDpi(80, dpi);
        int refreshW  = ScaleDpi(74, dpi);
        int wifiW     = ScaleDpi(68, dpi);

        int rightX = w - pad - startW;
        deferCtrl(s->buttons[IDC_BTN_START_STOP], rightX, topY, startW, ctrlH, true, true);
        rightX -= (gap + settingsW);
        deferCtrl(s->buttons[IDC_BTN_SETTINGS], rightX, topY, settingsW, ctrlH, true, true);

        int maxComboW = ScaleDpi(260, dpi);
        int availLeft = rightX - pad - gap - refreshW - gap - wifiW - gap;
        int comboW = std::max(ScaleDpi(130, dpi), std::min(maxComboW, availLeft));

        deferCtrl(s->hwndDeviceDrop, pad, topY, comboW, ctrlH, true, true);

        int x = pad + comboW + gap;
        deferCtrl(s->buttons[IDC_BTN_REFRESH], x, topY, refreshW, ctrlH, true, true);
        x += refreshW + gap;
        deferCtrl(s->buttons[IDC_BTN_WIFI], x, topY, wifiW, ctrlH, true, true);
    } else {
        deferCtrl(s->hwndDeviceDrop, 0, 0, 0, 0, false, false);
        deferCtrl(s->buttons[IDC_BTN_REFRESH], 0, 0, 0, 0, false, false);
        deferCtrl(s->buttons[IDC_BTN_WIFI], 0, 0, 0, 0, false, false);
        deferCtrl(s->buttons[IDC_BTN_SETTINGS], 0, 0, 0, 0, false, false);
        deferCtrl(s->buttons[IDC_BTN_START_STOP], 0, 0, 0, 0, false, false);
    }

    // Bottom bar (only visible while mirroring and not immersive)
    bool isCam = IsCameraVideoSource(s->settings.videoSourceIndex);
    struct NavBtn { int id; int wDip; };
    NavBtn navBtns[] = {
        { IDC_BTN_NAV_BACK, 58 },
        { IDC_BTN_NAV_HOME, 58 },
        { IDC_BTN_NAV_RECENTS, 68 },
        { IDC_BTN_VOL_DOWN, 52 },
        { IDC_BTN_VOL_UP, 52 },
        { IDC_BTN_POWER, 56 },
        { IDC_BTN_ROTATE, 58 },
        { IDC_BTN_SCREENSHOT, 82 },
        { IDC_BTN_RECORD, 68 }
    };
    int bx = pad;
    int bh = ScaleDpi(28, dpi);
    int by = s->rcBottomBar.top + (botBarH - bh) / 2;

    if (mirroring && !imm && isCam) {
        for (const auto& nb : navBtns) {
            if (nb.id != IDC_BTN_RECORD) {
                deferCtrl(s->buttons[nb.id], 0, 0, 0, 0, false, false);
            }
        }
        int recW = ScaleDpi(74, dpi);
        int capW = ScaleDpi(74, dpi);
        int focW = ScaleDpi(64, dpi);
        int zoomW = ScaleDpi(130, dpi);
        int camGap = ScaleDpi(8, dpi);

        int recX = w - pad - recW;
        int capX = recX - camGap - capW;
        int focX = capX - camGap - focW;
        int zoomX = focX - ScaleDpi(12, dpi) - zoomW;
        s->rcZoomSlider = { zoomX, by, zoomX + zoomW, by + bh };

        deferCtrl(s->buttons[IDC_BTN_FOCUS], focX, by, focW, bh, true, true);
        deferCtrl(s->buttons[IDC_BTN_CAPTURE], capX, by, capW, bh, true, true);
        deferCtrl(s->buttons[IDC_BTN_RECORD], recX, by, recW, bh, true, true);
    } else {
        s->rcZoomSlider = {};
        deferCtrl(s->buttons[IDC_BTN_FOCUS], 0, 0, 0, 0, false, false);
        deferCtrl(s->buttons[IDC_BTN_CAPTURE], 0, 0, 0, 0, false, false);
        for (const auto& nb : navBtns) {
            HWND hBtn = s->buttons[nb.id];
            bool showNav = (mirroring && !imm);
            if (showNav) {
                int bw = ScaleDpi(nb.wDip, dpi);
                deferCtrl(hBtn, bx, by, bw, bh, true, true);
                bx += bw + ScaleDpi(6, dpi);
            } else {
                deferCtrl(hBtn, 0, 0, 0, 0, false, false);
            }
        }
    }
    if (hdwp) EndDeferWindowPos(hdwp);
    PositionImmersiveButton(hwnd, s);
}

void UpdateButtonStates(AppState* s) {
    bool mirroring = s->session.IsRunning();
    bool hasOnline = (s->selectedDeviceIdx >= 0 &&
                      s->selectedDeviceIdx < static_cast<int>(s->devices.size()) &&
                      s->devices[s->selectedDeviceIdx].IsOnline());

    SetWindowTextW(s->buttons[IDC_BTN_START_STOP], mirroring ? L"Stop" : L"Start");
    EnableWindow(s->buttons[IDC_BTN_START_STOP], (mirroring || hasOnline) ? TRUE : FALSE);
    EnableWindow(s->buttons[IDC_BTN_WIFI], (!mirroring && s->adb.IsReady()) ? TRUE : FALSE);
    EnableWindow(s->buttons[IDC_BTN_REFRESH], (!mirroring && s->adb.IsReady()) ? TRUE : FALSE);
    EnableWindow(s->buttons[IDC_BTN_SETTINGS], !mirroring ? TRUE : FALSE);
    if (s->hwndDeviceDrop) {
        EnableWindow(s->hwndDeviceDrop, (!mirroring && !s->devices.empty()) ? TRUE : FALSE);
        InvalidateRect(s->hwndDeviceDrop, nullptr, FALSE);
    }
    InvalidateRect(s->buttons[IDC_BTN_START_STOP], nullptr, FALSE);
}

LRESULT CALLBACK VideoWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

void ReleaseD2DResources(AppState* s) {
    if (!s) return;
    if (s->d2dCtx)        { s->d2dCtx->SetTarget(nullptr); }
    if (s->d2dSrcBmp)     { s->d2dSrcBmp->Release(); s->d2dSrcBmp = nullptr; }
    if (s->d2dTargetBmp)  { s->d2dTargetBmp->Release(); s->d2dTargetBmp = nullptr; }
    if (s->dxgiSwap)      { s->dxgiSwap->Release(); s->dxgiSwap = nullptr; }
    if (s->d2dCtx)        { s->d2dCtx->Release(); s->d2dCtx = nullptr; }
    if (s->d2dDevice)     { s->d2dDevice->Release(); s->d2dDevice = nullptr; }
    if (s->d2dFactory)    { s->d2dFactory->Release(); s->d2dFactory = nullptr; }
    if (s->d3dDevice)     { s->d3dDevice->Release(); s->d3dDevice = nullptr; }
    s->d2dBmpW = s->d2dBmpH = 0;
    s->d2dDstW = s->d2dDstH = 0;
}

bool EnsureD2DContext(AppState* s) {
    if (s->d2dCtx && s->d3dDevice) return true;
    ReleaseD2DResources(s);

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &s->d3dDevice, nullptr, nullptr))) {
        return false;
    }

    IDXGIDevice1* dxgiDev = nullptr;
    if (FAILED(s->d3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDev))) || !dxgiDev) {
        ReleaseD2DResources(s);
        return false;
    }
    dxgiDev->SetMaximumFrameLatency(1);

    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, IID_PPV_ARGS(&s->d2dFactory));
    if (SUCCEEDED(hr) && s->d2dFactory) {
        hr = s->d2dFactory->CreateDevice(dxgiDev, &s->d2dDevice);
    }
    dxgiDev->Release();

    if (SUCCEEDED(hr) && s->d2dDevice) {
        hr = s->d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &s->d2dCtx);
    }
    if (FAILED(hr) || !s->d2dCtx) {
        ReleaseD2DResources(s);
        return false;
    }
    return true;
}

bool RenderVideoToSwapChain(AppState* s, const std::shared_ptr<const DecodedFrame>& frame, int drawW, int drawH) {
    if (!s || !s->hwndVideo || !frame || frame->width <= 0 || frame->height <= 0 || drawW <= 0 || drawH <= 0) return false;
    std::lock_guard<std::mutex> lock(s->renderMutex);
    int fw = frame->width;
    int fh = frame->height;
    if (!EnsureD2DContext(s)) return false;

    if (!s->dxgiSwap) {
        IDXGIDevice* dxgiDev = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory2* factory = nullptr;
        if (SUCCEEDED(s->d3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDev))) && dxgiDev) {
            if (SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && adapter) {
                if (SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))) && factory) {
                    DXGI_SWAP_CHAIN_DESC1 desc = {};
                    desc.Width = static_cast<UINT>(drawW);
                    desc.Height = static_cast<UINT>(drawH);
                    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    desc.SampleDesc.Count = 1;
                    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                    desc.BufferCount = 3;
                    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                    factory->CreateSwapChainForHwnd(s->d3dDevice, s->hwndVideo, &desc, nullptr, nullptr, &s->dxgiSwap);
                    factory->Release();
                }
                adapter->Release();
            }
            dxgiDev->Release();
        }
        if (!s->dxgiSwap) return false;
        s->d2dDstW = 0;
        s->d2dDstH = 0;
    }

    if (s->d2dDstW != drawW || s->d2dDstH != drawH || !s->d2dTargetBmp) {
        s->d2dCtx->SetTarget(nullptr);
        if (s->d2dTargetBmp) { s->d2dTargetBmp->Release(); s->d2dTargetBmp = nullptr; }
        if (s->d2dDstW != 0 && s->d2dDstH != 0) {
            if (FAILED(s->dxgiSwap->ResizeBuffers(0, static_cast<UINT>(drawW), static_cast<UINT>(drawH), DXGI_FORMAT_UNKNOWN, 0))) {
                ReleaseD2DResources(s);
                return false;
            }
        }
        IDXGISurface* surf = nullptr;
        if (SUCCEEDED(s->dxgiSwap->GetBuffer(0, IID_PPV_ARGS(&surf))) && surf) {
            D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                96.0f, 96.0f
            );
            s->d2dCtx->CreateBitmapFromDxgiSurface(surf, &bp, &s->d2dTargetBmp);
            surf->Release();
        }
        if (!s->d2dTargetBmp) return false;
        s->d2dCtx->SetTarget(s->d2dTargetBmp);
        s->d2dDstW = drawW;
        s->d2dDstH = drawH;
    }

    if (!s->d2dSrcBmp || s->d2dBmpW != fw || s->d2dBmpH != fh) {
        if (s->d2dSrcBmp) { s->d2dSrcBmp->Release(); s->d2dSrcBmp = nullptr; }
        D2D1_BITMAP_PROPERTIES1 srcProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            96.0f, 96.0f
        );
        if (SUCCEEDED(s->d2dCtx->CreateBitmap(D2D1::SizeU(fw, fh), nullptr, 0, &srcProps, &s->d2dSrcBmp))) {
            s->d2dBmpW = fw;
            s->d2dBmpH = fh;
        }
    }
    if (!s->d2dSrcBmp) return false;

    D2D1_RECT_U srcRect = D2D1::RectU(0, 0, fw, fh);
    s->d2dSrcBmp->CopyFromMemory(&srcRect, frame->bgra.data(), static_cast<UINT32>(fw * 4));

    s->d2dCtx->BeginDraw();
    D2D1_MATRIX_3X2_F scaleMat = D2D1::Matrix3x2F::Scale(
        static_cast<FLOAT>(drawW) / static_cast<FLOAT>(fw),
        static_cast<FLOAT>(drawH) / static_cast<FLOAT>(fh)
    );
    s->d2dCtx->SetTransform(scaleMat);
    D2D1_INTERPOLATION_MODE interp = (drawW == fw && drawH == fh)
        ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
        : D2D1_INTERPOLATION_MODE_LINEAR;
    s->d2dCtx->DrawImage(s->d2dSrcBmp, interp);
    s->d2dCtx->SetTransform(D2D1::Matrix3x2F::Identity());

    HRESULT hrEnd = s->d2dCtx->EndDraw();
    if (hrEnd == D2DERR_RECREATE_TARGET) {
        ReleaseD2DResources(s);
        return false;
    }
    if (FAILED(hrEnd)) return false;

    HRESULT hrPresent = s->dxgiSwap->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
    if (hrPresent == DXGI_ERROR_WAS_STILL_DRAWING) {
        return false;
    }
    return true;
}

void StartVideoRenderThread(AppState* s) {
    if (!s || s->hFrameEvent) return;
    s->hFrameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s->session.SetFrameEvent(s->hFrameEvent);
    s->renderStop.store(false);
    s->renderThread = std::thread([s]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        uint64_t lastSeq = 0;
        int lastW = 0, lastH = 0;
        while (!s->renderStop.load()) {
            WaitForSingleObject(s->hFrameEvent, 100);
            if (s->renderStop.load()) break;
            if (!s->videoVisible.load()) continue;
            int dw = s->targetDrawW.load();
            int dh = s->targetDrawH.load();
            uint64_t seq = s->session.GetFrameSeq();
            if (dw <= 0 || dh <= 0) continue;
            if (seq == lastSeq && dw == lastW && dh == lastH) continue;
            auto frame = s->session.GetLatestFrame();
            if (!frame) continue;
            if (RenderVideoToSwapChain(s, frame, dw, dh)) {
                lastSeq = seq;
                lastW = dw;
                lastH = dh;
            }
        }
        {
            std::lock_guard<std::mutex> lock(s->renderMutex);
            ReleaseD2DResources(s);
        }
        CoUninitialize();
    });
}

void PaintMainWindow(HWND hwnd, HDC hdc, AppState* s) {
    RECT rcClient;
    GetClientRect(hwnd, &rcClient);
    int w = rcClient.right - rcClient.left;
    int h = rcClient.bottom - rcClient.top;
    if (w <= 0 || h <= 0) return;

    if (!s->hdcBack) {
        s->hdcBack = CreateCompatibleDC(hdc);
    }
    if (!s->hbmBack || s->backW != w || s->backH != h) {
        if (s->hbmBack) DeleteObject(s->hbmBack);
        s->hbmBack = CreateCompatibleBitmap(hdc, w, h);
        s->backW = w;
        s->backH = h;
    }
    HDC hdcMem = s->hdcBack;
    HBITMAP hbmOld = static_cast<HBITMAP>(SelectObject(hdcMem, s->hbmBack));

    const ThemePalette& pal = s->pal;
    UINT dpi = GetWindowDpi(hwnd);
    bool mirroring = s->session.IsRunning();
    bool imm = s->isImmersive;

    HBRUSH hbrWin   = CreateSolidBrush(pal.windowBg);
    HBRUSH hbrStage = CreateSolidBrush(mirroring ? pal.stageBg : pal.windowBg);
    HBRUSH hbrDiv   = CreateSolidBrush(pal.divider);

    if (!imm) {
        FillRect(hdcMem, &s->rcTopBar, hbrWin);
        RECT rcTopDiv = { 0, s->rcTopBar.bottom, w, s->rcTopBar.bottom + 1 };
        FillRect(hdcMem, &rcTopDiv, hbrDiv);
    }
    FillRect(hdcMem, &s->rcStage, hbrStage);

    if (mirroring && !imm) {
        FillRect(hdcMem, &s->rcBottomBar, hbrWin);
        RECT rcBotDiv = { 0, s->rcBottomBar.top - 1, w, s->rcBottomBar.top };
        FillRect(hdcMem, &rcBotDiv, hbrDiv);
    }

    DeleteObject(hbrWin);
    DeleteObject(hbrStage);
    DeleteObject(hbrDiv);
    SetBkMode(hdcMem, TRANSPARENT);

    s->rcVideoFrame = {};
    if (mirroring && s->currentFrame && s->currentFrame->width > 0 && s->currentFrame->height > 0) {
        int stageW = s->rcStage.right - s->rcStage.left;
        int stageH = s->rcStage.bottom - s->rcStage.top;
        int margin = s->isImmersive ? 0 : ScaleDpi(14, dpi);
        int availW = std::max(1, stageW - margin * 2);
        int availH = std::max(1, stageH - margin * 2);

        double scaleX = static_cast<double>(availW) / s->currentFrame->width;
        double scaleY = static_cast<double>(availH) / s->currentFrame->height;
        double scale  = std::min(scaleX, scaleY);

        int drawW = s->isImmersive ? availW : std::max(1, static_cast<int>(s->currentFrame->width * scale));
        int drawH = s->isImmersive ? availH : std::max(1, static_cast<int>(s->currentFrame->height * scale));
        if (s->isImmersive) {
            drawW = std::max(1, static_cast<int>(s->currentFrame->width * scale));
            drawH = std::max(1, static_cast<int>(s->currentFrame->height * scale));
        }
        int drawX = s->rcStage.left + (stageW - drawW) / 2;
        int drawY = s->rcStage.top + (stageH - drawH) / 2;

        s->rcVideoFrame = { drawX, drawY, drawX + drawW, drawY + drawH };

        s->targetDrawW.store(drawW);
        s->targetDrawH.store(drawH);
        s->videoVisible.store(true);
        if (s->hwndVideo) {
            if (!EqualRect(&s->rcPlacedVideo, &s->rcVideoFrame) || !s->videoShown) {
                s->rcPlacedVideo = s->rcVideoFrame;
                s->videoShown = true;
                SetWindowPos(s->hwndVideo, HWND_TOP, drawX, drawY, drawW, drawH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                if (s->hFrameEvent) SetEvent(s->hFrameEvent);
            }
        }

        bool drewD2D = (s->dxgiSwap != nullptr);
        if (!drewD2D) {
            drewD2D = RenderVideoToSwapChain(s, s->currentFrame, drawW, drawH);
        }

        if (!drewD2D) {
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = s->currentFrame->width;
            bmi.bmiHeader.biHeight = -s->currentFrame->height;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            int prevMode = SetStretchBltMode(hdcMem, HALFTONE);
            SetBrushOrgEx(hdcMem, 0, 0, nullptr);
            StretchDIBits(
                hdcMem,
                drawX, drawY, drawW, drawH,
                0, 0, s->currentFrame->width, s->currentFrame->height,
                s->currentFrame->bgra.data(),
                &bmi,
                DIB_RGB_COLORS,
                SRCCOPY
            );
            SetStretchBltMode(hdcMem, prevMode);
        }
    } else {
        s->videoVisible.store(false);
        if (s->hwndVideo && s->videoShown) {
            s->videoShown = false;
            ShowWindow(s->hwndVideo, SW_HIDE);
        }
        // Clean centered typography
        int stageH = s->rcStage.bottom - s->rcStage.top;
        int centerY = s->rcStage.top + stageH / 2;

        std::wstring title = L"Android Display";
        std::wstring subtitle;
        if (mirroring) {
            subtitle = L"Connecting...";
        } else if (!s->adb.IsReady()) {
            subtitle = L"ADB unavailable";
        } else if (s->devices.empty()) {
            subtitle = L"No device connected";
        } else if (!s->transientStatus.empty()) {
            subtitle = s->transientStatus;
        } else if (s->selectedDeviceIdx >= 0 && s->selectedDeviceIdx < static_cast<int>(s->devices.size())) {
            const AdbDevice& dev = s->devices[s->selectedDeviceIdx];
            title = dev.DisplayName();
            if (!dev.IsOnline()) {
                subtitle = dev.state;
            } else {
                subtitle = dev.isWireless ? L"Wi-Fi" : L"USB";
                if (!dev.androidVer.empty()) subtitle += L"  \u2022  Android " + dev.androidVer;
                if (dev.screenWidth > 0 && dev.screenHeight > 0) {
                    subtitle += L"  \u2022  " + std::to_wstring(dev.screenWidth) + L"\u00D7" + std::to_wstring(dev.screenHeight);
                }
            }
        }

        int titleH = ScaleDpi(38, dpi);
        int subH   = ScaleDpi(26, dpi);
        RECT rcT = { s->rcStage.left, centerY - titleH, s->rcStage.right, centerY };
        RECT rcS = { s->rcStage.left, centerY + ScaleDpi(4, dpi), s->rcStage.right, centerY + ScaleDpi(4, dpi) + subH };

        SelectObject(hdcMem, s->hFontTitle);
        SetTextColor(hdcMem, pal.textPrimary);
        DrawTextW(hdcMem, title.c_str(), -1, &rcT, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        SelectObject(hdcMem, s->hFontSubtitle);
        SetTextColor(hdcMem, pal.textSecondary);
        DrawTextW(hdcMem, subtitle.c_str(), -1, &rcS, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    if (mirroring && !s->isImmersive) {
        StreamStats st = s->session.GetStats();
        std::wstring info;
        if (!s->transientStatus.empty()) {
            info = s->transientStatus;
        } else if (st.width > 0 && st.height > 0) {
            wchar_t buf[128] = {};
            StringCchPrintfW(buf, ARRAYSIZE(buf), L"%d \u00D7 %d  \u2022  %d fps  \u2022  Audio %s",
                st.width, st.height, st.fps, st.audioActive ? L"On" : L"Off");
            info = buf;
        }
        if (!info.empty()) {
            bool isCam = IsCameraVideoSource(s->settings.videoSourceIndex);
            SelectObject(hdcMem, s->hFontSmall);
            SetTextColor(hdcMem, pal.textSecondary);
            RECT rcInfo = isCam
                ? RECT{ s->rcBottomBar.left + ScaleDpi(16, dpi), s->rcBottomBar.top, s->rcZoomSlider.left - ScaleDpi(12, dpi), s->rcBottomBar.bottom }
                : RECT{ s->rcBottomBar.left + ScaleDpi(460, dpi), s->rcBottomBar.top, s->rcBottomBar.right - ScaleDpi(16, dpi), s->rcBottomBar.bottom };
            DrawTextW(hdcMem, info.c_str(), -1, &rcInfo, (isCam ? DT_LEFT : DT_RIGHT) | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        if (IsCameraVideoSource(s->settings.videoSourceIndex) &&
            (s->rcZoomSlider.right > s->rcZoomSlider.left)) {
            const RECT& zr = s->rcZoomSlider;
            int labelW = ScaleDpi(34, dpi);
            RECT rcZLbl = { zr.left, zr.top, zr.left + labelW, zr.bottom };
            wchar_t zTxt[16];
            StringCchPrintfW(zTxt, ARRAYSIZE(zTxt), L"%.1fx", s->camZoom);
            SelectObject(hdcMem, s->hFontSmall);
            SetTextColor(hdcMem, pal.textSecondary);
            DrawTextW(hdcMem, zTxt, -1, &rcZLbl, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            int trackL = zr.left + labelW + ScaleDpi(4, dpi);
            int trackR = zr.right - ScaleDpi(6, dpi);
            int trackY = (zr.top + zr.bottom) / 2;
            int trackH = ScaleDpi(4, dpi);
            float tNorm = std::clamp((s->camZoom - 1.0f) / 3.0f, 0.0f, 1.0f);
            int thumbX = trackL + static_cast<int>(tNorm * (trackR - trackL));

            Gdiplus::Graphics g(hdcMem);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::GraphicsPath trackPath;
            AddRoundedRectPath(trackPath, static_cast<float>(trackL), static_cast<float>(trackY - trackH / 2),
                static_cast<float>(trackR - trackL), static_cast<float>(trackH), static_cast<float>(trackH) * 0.5f);
            Gdiplus::SolidBrush trackBr(ToGdipColor(pal.border));
            g.FillPath(&trackBr, &trackPath);

            if (thumbX > trackL) {
                Gdiplus::GraphicsPath fillPath;
                AddRoundedRectPath(fillPath, static_cast<float>(trackL), static_cast<float>(trackY - trackH / 2),
                    static_cast<float>(thumbX - trackL), static_cast<float>(trackH), static_cast<float>(trackH) * 0.5f);
                Gdiplus::SolidBrush fillBr(ToGdipColor(pal.accent));
                g.FillPath(&fillBr, &fillPath);
            }

            int thumbR = ScaleDpi(6, dpi);
            Gdiplus::SolidBrush thumbBr(ToGdipColor(pal.accent));
            g.FillEllipse(&thumbBr, thumbX - thumbR, trackY - thumbR, thumbR * 2, thumbR * 2);
        }
    }

    BitBlt(hdc, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);
    SelectObject(hdcMem, hbmOld);
}

bool MapPointToDevice(const AppState* s, int clientX, int clientY, int& outX, int& outY, int& outW, int& outH) {
    if (!s->session.IsRunning() || !s->currentFrame) return false;
    if (IsCameraVideoSource(s->settings.videoSourceIndex)) return false; // Camera mode: do not forward pointer to screen
    const RECT& vf = s->rcVideoFrame;
    int vw = vf.right - vf.left;
    int vh = vf.bottom - vf.top;
    if (vw <= 0 || vh <= 0) return false;
    if (clientX < vf.left || clientX >= vf.right || clientY < vf.top || clientY >= vf.bottom) return false;

    outW = s->currentFrame->width;
    outH = s->currentFrame->height;
    outX = std::clamp<int>(((clientX - vf.left) * outW) / vw, 0, outW - 1);
    outY = std::clamp<int>(((clientY - vf.top) * outH) / vh, 0, outH - 1);
    return true;
}

void HandleStartStop(AppState* s) {
    if (s->session.IsRunning()) {
        s->session.Stop();
        s->currentFrame.reset();
        s->transientStatus.clear();
        LayoutControls(s->hwndMain, s);
        UpdateButtonStates(s);
        InvalidateRect(s->hwndMain, nullptr, FALSE);
        return;
    }

    if (s->selectedDeviceIdx < 0 || s->selectedDeviceIdx >= static_cast<int>(s->devices.size())) return;
    const AdbDevice& dev = s->devices[s->selectedDeviceIdx];
    if (!dev.IsOnline()) return;

    MirrorConfig cfg;
    cfg.serial = dev.serial;
    cfg.mirrorAudio = s->settings.mirrorAudio;
    cfg.turnScreenOff = s->settings.turnScreenOff;
    cfg.stayAwake = s->settings.stayAwake;
    cfg.showTouches = s->settings.showTouches;
    cfg.deviceWidth = dev.screenWidth;
    cfg.deviceHeight = dev.screenHeight;
    s->screenPowerOn = !s->settings.turnScreenOff;

    const int resMap[3] = { 0, 1080, 720 };
    const int brMap[6]  = { 2, 4, 8, 12, 16, 24 };
    const int fpsMap[7] = { 0, 120, 90, 60, 45, 30, 24 };
    cfg.maxSize = resMap[std::clamp(s->settings.resIndex, 0, 2)];
    cfg.bitrateMbps = brMap[std::clamp(s->settings.bitrateIndex, 0, 5)];
    cfg.maxFps = fpsMap[std::clamp(s->settings.maxFpsIndex, 0, 6)];
    cfg.lockOrient = s->settings.lockOrientIndex;
    cfg.videoSource = s->settings.videoSourceIndex;
    cfg.audioSource = s->settings.audioSourceIndex;

    if (!s->settings.customRes.empty()) {
        size_t xPos = s->settings.customRes.find_first_of(L"xX×");
        if (xPos != std::wstring::npos) {
            cfg.customW = _wtoi(s->settings.customRes.substr(0, xPos).c_str());
            cfg.customH = _wtoi(s->settings.customRes.substr(xPos + 1).c_str());
        }
    }
    if (!s->settings.customDpi.empty()) {
        cfg.customDpi = _wtoi(s->settings.customDpi.c_str());
    }

    s->currentFrame.reset();
    s->transientStatus.clear();
    s->camZoom = 1.0f;
    s->session.Start(s->hwndMain, s->adb, cfg);
    LayoutControls(s->hwndMain, s);
    UpdateButtonStates(s);
    InvalidateRect(s->hwndMain, nullptr, FALSE);
}

void HandleCameraCapture(AppState* s) {
    if (!s || !s->currentFrame || s->currentFrame->width <= 0 || s->currentFrame->height <= 0) return;
    auto frame = s->currentFrame;

    wchar_t picDir[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_MYPICTURES, nullptr, 0, picDir);
    std::wstring outDir = std::wstring(picDir) + L"\\Android Display";
    CreateDirectoryW(outDir.c_str(), nullptr);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t fileName[64];
    StringCchPrintfW(fileName, ARRAYSIZE(fileName), L"Camera_%04d%02d%02d_%02d%02d%02d.png",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring fullPath = outDir + L"\\" + fileName;

    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return;
    std::vector<uint8_t> encBuf(size);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(encBuf.data());
    Gdiplus::GetImageEncoders(num, size, encoders);
    CLSID pngClsid = {};
    bool found = false;
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(encoders[i].MimeType, L"image/png") == 0) {
            pngClsid = encoders[i].Clsid;
            found = true;
            break;
        }
    }
    if (!found) return;

    Gdiplus::Bitmap bmp(
        frame->width, frame->height, frame->width * 4,
        PixelFormat32bppARGB,
        const_cast<BYTE*>(frame->bgra.data())
    );
    if (bmp.Save(fullPath.c_str(), &pngClsid, nullptr) == Gdiplus::Ok) {
        s->transientStatus = std::wstring(L"Saved ") + fileName;
    } else {
        s->transientStatus = L"Failed to save capture";
    }
    InvalidateRect(s->hwndMain, &s->rcBottomBar, FALSE);
}

void HandleScreenshot(AppState* s) {
    if (s->selectedDeviceIdx < 0 || s->selectedDeviceIdx >= static_cast<int>(s->devices.size())) return;
    const AdbDevice& dev = s->devices[s->selectedDeviceIdx];
    if (!dev.IsOnline()) return;

    HWND hwnd = s->hwndMain;
    AdbClient adbCopy = s->adb;
    std::wstring serial = dev.serial;

    std::thread([hwnd, adbCopy, serial]() {
        std::vector<uint8_t> png;
        auto* msg = new std::wstring();
        if (adbCopy.TakeScreenshotPng(serial, png)) {
            wchar_t picDir[MAX_PATH] = {};
            SHGetFolderPathW(nullptr, CSIDL_MYPICTURES, nullptr, 0, picDir);
            std::wstring outDir = std::wstring(picDir) + L"\\Android Display";
            CreateDirectoryW(outDir.c_str(), nullptr);

            SYSTEMTIME st;
            GetLocalTime(&st);
            wchar_t fileName[64];
            StringCchPrintfW(fileName, ARRAYSIZE(fileName), L"Screenshot_%04d%02d%02d_%02d%02d%02d.png",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            std::wstring fullPath = outDir + L"\\" + fileName;

            HANDLE hFile = CreateFileW(fullPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(hFile, png.data(), static_cast<DWORD>(png.size()), &written, nullptr);
                CloseHandle(hFile);
                *msg = std::wstring(L"Saved ") + fileName;
            }
        }
        if (!PostMessageW(hwnd, WM_APP_STATUS_MSG, 0, reinterpret_cast<LPARAM>(msg))) {
            delete msg;
        }
    }).detach();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    auto* s = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (uMsg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        s = reinterpret_cast<AppState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        s->hwndMain = hwnd;

        s->hwndDeviceDrop = CreateWindowExW(
            0, WC_BUTTONW, L"No devices",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_DEVICES)),
            cs->hInstance, nullptr
        );
        SetWindowSubclass(s->hwndDeviceDrop, FluentBtnSubclass, static_cast<UINT_PTR>(IDC_BTN_DEVICES), reinterpret_cast<DWORD_PTR>(s));
        s->btnHovered[IDC_BTN_DEVICES] = false;

        struct BtnDef { int id; const wchar_t* text; };
        BtnDef defs[] = {
            { IDC_BTN_REFRESH,     L"Refresh" },
            { IDC_BTN_WIFI,        L"Wi-Fi" },
            { IDC_BTN_SETTINGS,    L"Settings" },
            { IDC_BTN_START_STOP,  L"Start" },
            { IDC_BTN_NAV_BACK,    L"Back" },
            { IDC_BTN_NAV_HOME,    L"Home" },
            { IDC_BTN_NAV_RECENTS, L"Recents" },
            { IDC_BTN_VOL_DOWN,    L"Vol -" },
            { IDC_BTN_VOL_UP,      L"Vol +" },
            { IDC_BTN_POWER,       L"Power" },
            { IDC_BTN_ROTATE,      L"Rotate" },
            { IDC_BTN_SCREENSHOT,  L"Screenshot" },
            { IDC_BTN_RECORD,      L"Record" },
            { IDC_BTN_CAPTURE,     L"Capture" },
            { IDC_BTN_FOCUS,       L"Focus" }
        };
        DragAcceptFiles(hwnd, TRUE);

        for (const auto& d : defs) {
            HWND hBtn = CreateWindowExW(
                0, WC_BUTTONW, d.text,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(d.id)),
                cs->hInstance, nullptr
            );
            SetWindowSubclass(hBtn, FluentBtnSubclass, static_cast<UINT_PTR>(d.id), reinterpret_cast<DWORD_PTR>(s));
            s->buttons[d.id] = hBtn;
            s->btnHovered[d.id] = false;
        }

        const wchar_t IMM_CLASS[] = L"AndroidDisplayImmersiveBtn";
        WNDCLASSEXW wci = { sizeof(wci) };
        if (!GetClassInfoExW(cs->hInstance, IMM_CLASS, &wci)) {
            wci.style = CS_HREDRAW | CS_VREDRAW;
            wci.lpfnWndProc = ImmersiveBtnWndProc;
            wci.hInstance = cs->hInstance;
            wci.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wci.lpszClassName = IMM_CLASS;
            RegisterClassExW(&wci);
        }
        s->hwndImmersiveBtn = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            IMM_CLASS, L"",
            WS_POPUP,
            0, 0, 0, 0,
            hwnd, nullptr, cs->hInstance, s
        );

        const wchar_t VID_CLASS[] = L"AndroidDisplayVideoHost";
        WNDCLASSEXW wcv = { sizeof(wcv) };
        if (!GetClassInfoExW(cs->hInstance, VID_CLASS, &wcv)) {
            wcv.style = 0;
            wcv.lpfnWndProc = VideoWndProc;
            wcv.hInstance = cs->hInstance;
            wcv.lpszClassName = VID_CLASS;
            RegisterClassExW(&wcv);
        }
        s->hwndVideo = CreateWindowExW(
            0, VID_CLASS, L"",
            WS_CHILD | WS_CLIPSIBLINGS,
            0, 0, 0, 0,
            hwnd, nullptr, cs->hInstance, nullptr
        );
        StartVideoRenderThread(s);

        ApplyTheme(hwnd, s);
        CreateFonts(hwnd, s);
        LoadAppSettings(s->settings);
        if (s->settings.alwaysOnTop) {
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }
        SyncDeviceCombo(s);
        LayoutControls(hwnd, s);
        UpdateButtonStates(s);
        TriggerAsyncDeviceRefresh(s);
        SetTimer(hwnd, TIMER_REFRESH_DEVS, 3000, nullptr);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        UINT dpi = GetWindowDpi(hwnd);
        if (s && s->isImmersive) {
            mmi->ptMinTrackSize.x = ScaleDpi(260, dpi);
            mmi->ptMinTrackSize.y = ScaleDpi(260, dpi);
        } else {
            mmi->ptMinTrackSize.x = ScaleDpi(620, dpi);
            mmi->ptMinTrackSize.y = ScaleDpi(440, dpi);
        }
        return 0;
    }

    case WM_WINDOWPOSCHANGED: {
        LRESULT res = DefWindowProcW(hwnd, uMsg, wParam, lParam);
        if (s) PositionImmersiveButton(hwnd, s);
        return res;
    }

    case WM_SIZE:
        if (s) {
            LayoutControls(hwnd, s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_DPICHANGED:
        if (s) {
            CreateFonts(hwnd, s);
            auto* prc = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, prc->left, prc->top, prc->right - prc->left, prc->bottom - prc->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            LayoutControls(hwnd, s);
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;

    case WM_SETTINGCHANGE:
        if (s) {
            ApplyTheme(hwnd, s);
            if (s->hwndImmersiveBtn) UpdateImmersiveButtonVisual(s->hwndImmersiveBtn, s);
        }
        return 0;

    case WM_TIMER:
        if (s && wParam == TIMER_REFRESH_DEVS && !s->session.IsRunning()) {
            TriggerAsyncDeviceRefresh(s);
        }
        return 0;

    case WM_APP_DEVICES_UPDATED: {
        auto* devs = reinterpret_cast<std::vector<AdbDevice>*>(lParam);
        if (s && devs) {
            std::wstring prevSerial;
            if (s->selectedDeviceIdx >= 0 && s->selectedDeviceIdx < static_cast<int>(s->devices.size())) {
                prevSerial = s->devices[s->selectedDeviceIdx].serial;
            }
            s->devices = std::move(*devs);
            s->selectedDeviceIdx = -1;
            for (size_t i = 0; i < s->devices.size(); ++i) {
                if (s->devices[i].serial == prevSerial) {
                    s->selectedDeviceIdx = static_cast<int>(i);
                    break;
                }
            }
            if (s->selectedDeviceIdx == -1 && !s->devices.empty()) {
                s->selectedDeviceIdx = 0;
            }
            s->isRefreshingDevices.store(false);
            SyncDeviceCombo(s);
            UpdateButtonStates(s);
            InvalidateRect(hwnd, &s->rcStage, FALSE);
        }
        delete devs;
        return 0;
    }

    case WM_APP_FRAME_READY:
        if (s) {
            MSG drain;
            while (PeekMessageW(&drain, hwnd, WM_APP_FRAME_READY, WM_APP_FRAME_READY, PM_REMOVE)) {}
            auto f = s->session.GetLatestFrame();
            if (f) {
                bool dimsChanged = (f->width != s->lastImmFrameW || f->height != s->lastImmFrameH);
                bool firstFrame = !s->currentFrame;
                s->currentFrame = std::move(f);
                if (s->isImmersive && dimsChanged) {
                    ResizeWindowToAspect(hwnd, s);
                }
                if (firstFrame || dimsChanged || !s->videoShown) {
                    InvalidateRect(hwnd, &s->rcStage, FALSE);
                }
                if (!s->isImmersive) {
                    StreamStats st = s->session.GetStats();
                    if (st.width != s->lastTelW || st.height != s->lastTelH ||
                        st.fps != s->lastTelFps || st.audioActive != s->lastTelAudio ||
                        s->transientStatus != s->lastTelStatus) {
                        s->lastTelW = st.width;
                        s->lastTelH = st.height;
                        s->lastTelFps = st.fps;
                        s->lastTelAudio = st.audioActive;
                        s->lastTelStatus = s->transientStatus;
                        UINT dpi = GetWindowDpi(hwnd);
                        bool isCam = IsCameraVideoSource(s->settings.videoSourceIndex);
                        RECT rcTelemetry = isCam
                            ? RECT{ s->rcBottomBar.left + ScaleDpi(16, dpi), s->rcBottomBar.top, s->rcBottomBar.right - ScaleDpi(180, dpi), s->rcBottomBar.bottom }
                            : RECT{ s->rcBottomBar.left + ScaleDpi(460, dpi), s->rcBottomBar.top, s->rcBottomBar.right, s->rcBottomBar.bottom };
                        InvalidateRect(hwnd, &rcTelemetry, FALSE);
                    }
                }
            }
        }
        return 0;

    case WM_APP_MIRROR_STATUS:
        if (s) InvalidateRect(hwnd, &s->rcBottomBar, FALSE);
        return 0;

    case WM_APP_MIRROR_STOPPED:
        if (s) {
            StreamStats st = s->session.GetStats();
            if (!st.statusText.empty() && st.statusText != L"Mirroring" && st.statusText != L"Connecting...") {
                s->transientStatus = st.statusText;
            }
            s->currentFrame.reset();
            LayoutControls(hwnd, s);
            UpdateButtonStates(s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_APP_STATUS_MSG: {
        auto* msg = reinterpret_cast<std::wstring*>(lParam);
        if (s && msg && !msg->empty()) {
            s->transientStatus = *msg;
            InvalidateRect(hwnd, &s->rcBottomBar, FALSE);
        }
        delete msg;
        return 0;
    }

    case WM_APP_CLIPBOARD_TEXT: {
        auto* txt = reinterpret_cast<std::wstring*>(lParam);
        if (txt && !txt->empty() && OpenClipboard(hwnd)) {
            EmptyClipboard();
            size_t bytes = (txt->size() + 1) * sizeof(wchar_t);
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (hMem) {
                void* p = GlobalLock(hMem);
                if (p) {
                    memcpy(p, txt->c_str(), bytes);
                    GlobalUnlock(hMem);
                    SetClipboardData(CF_UNICODETEXT, hMem);
                } else {
                    GlobalFree(hMem);
                }
            }
            CloseClipboard();
        }
        delete txt;
        return 0;
    }

    case WM_DRAWITEM: {
        auto* pDIS = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (!s || pDIS->CtlType != ODT_BUTTON) break;

        int id = static_cast<int>(pDIS->CtlID);
        wchar_t text[64] = {};
        GetWindowTextW(pDIS->hwndItem, text, ARRAYSIZE(text));

        bool pressed  = (pDIS->itemState & ODS_SELECTED) != 0;
        bool focused  = (pDIS->itemState & ODS_FOCUS) != 0;
        bool enabled  = IsWindowEnabled(pDIS->hwndItem) != FALSE;
        bool hovered  = s->btnHovered[id] && !pressed;
        bool isAccent = (id == IDC_BTN_START_STOP);

        UINT dpi = GetWindowDpi(hwnd);
        int radius = ScaleDpi(6, dpi);

        if (id == IDC_BTN_DEVICES) {
            RenderFluentDropdown(
                pDIS->hDC, pDIS->rcItem, text, s->hFontBody, s->pal,
                hovered, pressed, focused, enabled, s->pal.windowBg, radius, dpi
            );
            return TRUE;
        }

        HFONT font = (id >= IDC_BTN_NAV_BACK) ? s->hFontSmall : s->hFontBody;

        RenderFluentButton(
            pDIS->hDC, pDIS->rcItem, text, font, s->pal,
            hovered, pressed, focused, enabled, isAccent, s->pal.windowBg, radius
        );
        return TRUE;
    }

    case WM_COMMAND: {
        if (!s) break;
        int id = LOWORD(wParam);

        if (id == IDC_BTN_DEVICES && !s->devices.empty() && !s->session.IsRunning()) {
            RECT rcAnchor;
            GetWindowRect(s->hwndDeviceDrop, &rcAnchor);
            auto items = BuildDeviceDropdownItems(s);
            int chosen = ShowFluentDropdownMenu(hwnd, rcAnchor, items, s->selectedDeviceIdx, s->isDarkMode, s->hFontBody);
            if (chosen >= 0 && chosen < static_cast<int>(s->devices.size())) {
                s->selectedDeviceIdx = chosen;
                SyncDeviceCombo(s);
                UpdateButtonStates(s);
                InvalidateRect(hwnd, &s->rcStage, FALSE);
            }
            return 0;
        }

        switch (id) {
        case 9901:
            if (!s->isImmersive) {
                GetWindowRect(hwnd, &s->savedNormalRect);
                s->isImmersive = true;
                LayoutControls(hwnd, s);
                ResizeWindowToAspect(hwnd, s);
            } else {
                s->isImmersive = false;
                SetWindowTextW(hwnd, L"Android Display");
                if (s->savedNormalRect.right > s->savedNormalRect.left) {
                    SetWindowPos(hwnd, nullptr,
                        s->savedNormalRect.left, s->savedNormalRect.top,
                        s->savedNormalRect.right - s->savedNormalRect.left,
                        s->savedNormalRect.bottom - s->savedNormalRect.top,
                        SWP_NOZORDER | SWP_NOACTIVATE);
                }
                LayoutControls(hwnd, s);
            }
            PositionImmersiveButton(hwnd, s);
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            break;
        case IDC_BTN_REFRESH:
            TriggerAsyncDeviceRefresh(s);
            break;
        case IDC_BTN_WIFI:
            if (ShowWirelessAdbDialog(hwnd, s->isDarkMode, s->adb)) {
                TriggerAsyncDeviceRefresh(s);
            }
            break;
        case IDC_BTN_SETTINGS:
            if (ShowSettingsDialog(hwnd, s->isDarkMode, s->settings)) {
                HWND zOrder = s->settings.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST;
                SetWindowPos(hwnd, zOrder, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                if (s->hwndImmersiveBtn) {
                    SetWindowPos(s->hwndImmersiveBtn, zOrder, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
            }
            break;
        case IDC_BTN_START_STOP:
            HandleStartStop(s);
            break;
        case IDC_BTN_NAV_BACK:
        case IDC_BTN_NAV_HOME:
        case IDC_BTN_NAV_RECENTS:
        case IDC_BTN_VOL_DOWN:
        case IDC_BTN_VOL_UP:
        case IDC_BTN_POWER: {
            int key = AKEYCODE_BACK;
            if (id == IDC_BTN_NAV_HOME) key = AKEYCODE_HOME;
            else if (id == IDC_BTN_NAV_RECENTS) key = AKEYCODE_APP_SWITCH;
            else if (id == IDC_BTN_VOL_DOWN) key = AKEYCODE_VOLUME_DOWN;
            else if (id == IDC_BTN_VOL_UP) key = AKEYCODE_VOLUME_UP;
            else if (id == IDC_BTN_POWER) key = AKEYCODE_POWER;
            s->session.SendKey(key);
            break;
        }
        case IDC_BTN_ROTATE:
            s->session.RotateDevice();
            break;
        case IDC_BTN_CAPTURE:
            HandleCameraCapture(s);
            break;
        case IDC_BTN_FOCUS:
            s->session.TriggerCameraFocus();
            break;
        case IDC_BTN_SCREENSHOT:
            HandleScreenshot(s);
            break;
        case IDC_BTN_RECORD:
            if (s->session.IsRunning()) {
                if (s->session.IsRecording()) {
                    s->session.StopRecording();
                    SetWindowTextW(s->buttons[IDC_BTN_RECORD], L"Record");
                    s->transientStatus = L"Recording saved";
                } else {
                    wchar_t vidDir[MAX_PATH] = {};
                    SHGetFolderPathW(nullptr, CSIDL_MYVIDEO, nullptr, 0, vidDir);
                    std::wstring outDir = std::wstring(vidDir) + L"\\Android Display";
                    CreateDirectoryW(outDir.c_str(), nullptr);
                    SYSTEMTIME st;
                    GetLocalTime(&st);
                    wchar_t fileName[64];
                    StringCchPrintfW(fileName, ARRAYSIZE(fileName), L"Recording_%04d%02d%02d_%02d%02d%02d.mp4",
                        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                    if (s->session.StartRecording(outDir + L"\\" + fileName)) {
                        SetWindowTextW(s->buttons[IDC_BTN_RECORD], L"Stop Rec");
                        s->transientStatus = std::wstring(L"Recording ") + fileName;
                    } else {
                        s->transientStatus = L"Failed to start recording";
                    }
                }
                InvalidateRect(s->buttons[IDC_BTN_RECORD], nullptr, FALSE);
                InvalidateRect(hwnd, &s->rcBottomBar, FALSE);
            }
            break;
        }
        return 0;
    }

    case WM_DROPFILES: {
        if (!s || s->selectedDeviceIdx < 0 || s->selectedDeviceIdx >= static_cast<int>(s->devices.size())) break;
        const AdbDevice& dev = s->devices[s->selectedDeviceIdx];
        if (!dev.IsOnline()) break;

        HDROP hDrop = reinterpret_cast<HDROP>(wParam);
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            wchar_t filePath[MAX_PATH] = {};
            if (DragQueryFileW(hDrop, i, filePath, ARRAYSIZE(filePath)) > 0) {
                std::wstring path(filePath);
                std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
                bool isApk = (path.size() >= 4 && _wcsicmp(path.c_str() + path.size() - 4, L".apk") == 0);

                HWND hwndMain = s->hwndMain;
                AdbClient adbCopy = s->adb;
                std::wstring serial = dev.serial;
                std::thread([hwndMain, adbCopy, serial, path, name, isApk]() {
                    auto* msg = new std::wstring();
                    if (isApk) {
                        std::wstring out;
                        *msg = adbCopy.InstallApk(serial, path, out) ? (L"Installed " + name) : (L"Install failed: " + name);
                    } else {
                        *msg = adbCopy.PushFile(serial, path, L"/sdcard/Download/" + name)
                            ? (L"Pushed " + name + L" to Download")
                            : (L"Push failed: " + name);
                    }
                    if (!PostMessageW(hwndMain, WM_APP_STATUS_MSG, 0, reinterpret_cast<LPARAM>(msg))) {
                        delete msg;
                    }
                }).detach();
            }
        }
        DragFinish(hDrop);
        return 0;
    }

    case WM_RBUTTONUP: {
        if (s && s->session.IsRunning() && !IsCameraVideoSource(s->settings.videoSourceIndex)) {
            int dx = 0, dy = 0, dw = 0, dh = 0;
            if (MapPointToDevice(s, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), dx, dy, dw, dh)) {
                s->session.SendKey(AKEYCODE_BACK);
                return 0;
            }
        }
        break;
    }

    case WM_MBUTTONUP: {
        if (s && s->session.IsRunning() && !IsCameraVideoSource(s->settings.videoSourceIndex)) {
            int dx = 0, dy = 0, dw = 0, dh = 0;
            if (MapPointToDevice(s, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), dx, dy, dw, dh)) {
                s->session.SendKey(AKEYCODE_HOME);
                return 0;
            }
        }
        break;
    }

    case WM_MOUSEMOVE: {
        if (s && s->isZoomDragging) {
            int x = GET_X_LPARAM(lParam);
            UINT dpi = GetWindowDpi(hwnd);
            int trackL = s->rcZoomSlider.left + ScaleDpi(38, dpi);
            int trackR = s->rcZoomSlider.right - ScaleDpi(6, dpi);
            if (trackR > trackL) {
                float t = std::clamp(static_cast<float>(x - trackL) / static_cast<float>(trackR - trackL), 0.0f, 1.0f);
                s->camZoom = 1.0f + t * 3.0f;
                s->session.SetCameraZoom(s->camZoom);
                InvalidateRect(hwnd, &s->rcZoomSlider, FALSE);
            }
            return 0;
        }
        if (s && s->isPointerDown) {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            int dx = 0, dy = 0, dw = 0, dh = 0;
            if (MapPointToDevice(s, x, y, dx, dy, dw, dh)) {
                s->session.SendTouch(2, dx, dy, dw, dh);
            }
            return 0;
        }
        if (s && s->session.IsRunning() && !IsCameraVideoSource(s->settings.videoSourceIndex)) {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            int dx = 0, dy = 0, dw = 0, dh = 0;
            if (MapPointToDevice(s, x, y, dx, dy, dw, dh)) {
                if (dx != s->lastHoverX || dy != s->lastHoverY) {
                    s->lastHoverX = dx;
                    s->lastHoverY = dy;
                    s->session.SendTouch(3, dx, dy, dw, dh);
                }
                return 0;
            }
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        if (!s) break;
        SetFocus(hwnd);
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        POINT pt = { x, y };
        if (s->session.IsRunning() && !s->isImmersive &&
            IsCameraVideoSource(s->settings.videoSourceIndex) &&
            PtInRect(&s->rcZoomSlider, pt)) {
            s->isZoomDragging = true;
            SetCapture(hwnd);
            UINT dpi = GetWindowDpi(hwnd);
            int trackL = s->rcZoomSlider.left + ScaleDpi(38, dpi);
            int trackR = s->rcZoomSlider.right - ScaleDpi(6, dpi);
            if (trackR > trackL) {
                float t = std::clamp(static_cast<float>(x - trackL) / static_cast<float>(trackR - trackL), 0.0f, 1.0f);
                s->camZoom = 1.0f + t * 3.0f;
                s->session.SetCameraZoom(s->camZoom);
                InvalidateRect(hwnd, &s->rcZoomSlider, FALSE);
            }
            return 0;
        }
        int dx = 0, dy = 0, dw = 0, dh = 0;
        if (MapPointToDevice(s, x, y, dx, dy, dw, dh)) {
            s->isPointerDown = true;
            SetCapture(hwnd);
            s->session.SendTouch(0, dx, dy, dw, dh);
            return 0;
        }
        break;
    }

    case WM_LBUTTONUP: {
        if (s && s->isZoomDragging) {
            s->isZoomDragging = false;
            ReleaseCapture();
            return 0;
        }
        if (s && s->isPointerDown) {
            s->isPointerDown = false;
            ReleaseCapture();
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            int dx = 0, dy = 0, dw = 0, dh = 0;
            if (MapPointToDevice(s, x, y, dx, dy, dw, dh)) {
                s->session.SendTouch(1, dx, dy, dw, dh);
            } else if (s->currentFrame) {
                s->session.SendTouch(1, s->currentFrame->width / 2, s->currentFrame->height / 2,
                    s->currentFrame->width, s->currentFrame->height);
            }
            return 0;
        }
        break;
    }

    case WM_MOUSEWHEEL: {
        if (s && s->session.IsRunning() && !IsCameraVideoSource(s->settings.videoSourceIndex)) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            int dx = 0, dy = 0, dw = 0, dh = 0;
            if (MapPointToDevice(s, pt.x, pt.y, dx, dy, dw, dh)) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
                s->session.SendScroll(dx, dy, dw, dh, delta);
            }
        }
        return 0;
    }

    case WM_SYSKEYDOWN:
    case WM_KEYDOWN: {
        if (s && (wParam == VK_F11 || (wParam == 'F' && (GetKeyState(VK_MENU) & 0x8000)))) {
            DWORD style = GetWindowLongW(hwnd, GWL_STYLE);
            if (!s->isFullscreen) {
                GetWindowPlacement(hwnd, &s->savedPlacement);
                MONITORINFO mi = { sizeof(mi) };
                GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
                SetWindowLongW(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
                SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                    mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                    SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
                s->isFullscreen = true;
            } else {
                SetWindowLongW(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
                SetWindowPlacement(hwnd, &s->savedPlacement);
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
                s->isFullscreen = false;
            }
            return 0;
        }

        if (s && s->session.IsRunning() && !IsCameraVideoSource(s->settings.videoSourceIndex)) {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool alt  = (GetKeyState(VK_MENU) & 0x8000) != 0;

            if ((ctrl || alt) && wParam == 'V') {
                if (OpenClipboard(hwnd)) {
                    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                    if (hData) {
                        const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(hData));
                        if (p) {
                            s->session.SendClipboard(p, true);
                            GlobalUnlock(hData);
                        }
                    }
                    CloseClipboard();
                }
                return 0;
            }
            if ((ctrl || alt) && wParam == 'C') {
                s->session.SendKey(278); // KEYCODE_COPY
                return 0;
            }
            if ((ctrl || alt) && wParam == 'X') {
                s->session.SendKey(277); // KEYCODE_CUT
                return 0;
            }
            if (alt && wParam == 'O') {
                s->screenPowerOn = !s->screenPowerOn;
                s->session.SetScreenPower(s->screenPowerOn);
                return 0;
            }
            if (alt && wParam == 'R') {
                s->session.RotateDevice();
                return 0;
            }
            if (alt && wParam == 'N') {
                bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                s->session.ExpandNotifications(!shift);
                return 0;
            }

            int key = 0;
            switch (wParam) {
            case VK_ESCAPE: key = AKEYCODE_BACK; break;
            case VK_HOME:   key = AKEYCODE_HOME; break;
            case VK_RETURN: key = AKEYCODE_ENTER; break;
            case VK_BACK:   key = AKEYCODE_DEL; break;
            case VK_TAB:    key = 61; break;  // AKEYCODE_TAB
            case VK_DELETE: key = 112; break; // AKEYCODE_FORWARD_DEL
            case VK_PRIOR:  key = 92; break;  // AKEYCODE_PAGE_UP
            case VK_NEXT:   key = 93; break;  // AKEYCODE_PAGE_DOWN
            case VK_END:    key = 123; break; // AKEYCODE_MOVE_END
            case VK_UP:     key = AKEYCODE_DPAD_UP; break;
            case VK_DOWN:   key = AKEYCODE_DPAD_DOWN; break;
            case VK_LEFT:   key = AKEYCODE_DPAD_LEFT; break;
            case VK_RIGHT:  key = AKEYCODE_DPAD_RIGHT; break;
            }
            if (key != 0) {
                s->session.SendKey(key);
                return 0;
            }
        }
        break;
    }

    case WM_CHAR: {
        if (s && s->session.IsRunning() && !IsCameraVideoSource(s->settings.videoSourceIndex)) {
            wchar_t ch = static_cast<wchar_t>(wParam);
            if (ch >= 32 && ch != 127) {
                s->session.SendText(std::wstring(1, ch));
                return 0;
            }
        }
        break;
    }

    case WM_SETTEXT: {
        LRESULT res = DefWindowProcW(hwnd, uMsg, wParam, lParam);
        InvalidateRect(hwnd, nullptr, FALSE);
        return res;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (s) PaintMainWindow(hwnd, hdc, s);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_REFRESH_DEVS);
        if (s) {
            s->session.Stop();
            s->renderStop.store(true);
            if (s->hFrameEvent) SetEvent(s->hFrameEvent);
            if (s->renderThread.joinable()) s->renderThread.join();
            if (s->hFrameEvent) { CloseHandle(s->hFrameEvent); s->hFrameEvent = nullptr; }
            if (s->hwndImmersiveBtn) { DestroyWindow(s->hwndImmersiveBtn); s->hwndImmersiveBtn = nullptr; }
            ReleaseD2DResources(s);
            if (s->hwndVideo)        { DestroyWindow(s->hwndVideo); s->hwndVideo = nullptr; }
            if (s->hbmBack)       DeleteObject(s->hbmBack);
            if (s->hdcBack)       DeleteDC(s->hdcBack);
            if (s->hFontTitle)    DeleteObject(s->hFontTitle);
            if (s->hFontSubtitle) DeleteObject(s->hFontSubtitle);
            if (s->hFontBody)     DeleteObject(s->hFontBody);
            if (s->hFontSmall)    DeleteObject(s->hFontSmall);
        }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    using SetDpiCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (HMODULE hUser32 = GetModuleHandleW(L"user32.dll")) {
        auto fn = reinterpret_cast<SetDpiCtxFn>(GetProcAddress(hUser32, "SetProcessDpiAwarenessContext"));
        if (fn) {
            fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icex);

    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    const wchar_t CLASS_NAME[] = L"AndroidDisplayWindowClass";
    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(wcex);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hIcon = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wcex.hIconSm = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wcex.lpszClassName = CLASS_NAME;

    if (!RegisterClassExW(&wcex)) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 0;
    }

    auto state = std::make_unique<AppState>();

    HDC screenDc = GetDC(nullptr);
    int systemDpi = GetDeviceCaps(screenDc, LOGPIXELSY);
    ReleaseDC(nullptr, screenDc);
    if (systemDpi <= 0) systemDpi = 96;

    int initW = ScaleDpi(920, static_cast<UINT>(systemDpi));
    int initH = ScaleDpi(640, static_cast<UINT>(systemDpi));
    int posX = (GetSystemMetrics(SM_CXSCREEN) - initW) / 2;
    int posY = (GetSystemMetrics(SM_CYSCREEN) - initH) / 2;

    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, L"Android Display",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        posX, posY, initW, initH,
        nullptr, nullptr, hInstance, state.get()
    );

    if (!hwnd) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 0;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Gdiplus::GdiplusShutdown(gdiplusToken);
    return static_cast<int>(msg.wParam);
}
