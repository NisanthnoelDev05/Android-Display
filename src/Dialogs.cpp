#include "Dialogs.h"
#include "QrCode.h"
#include "ResourceIds.h"
#include <commctrl.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <strsafe.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

namespace {

constexpr UINT WM_APP_QR_STATUS = WM_APP + 51;

constexpr int IDC_DLG_PRIMARY_BTN   = 3001;
constexpr int IDC_DLG_CANCEL_BTN    = 3002;
constexpr int IDC_DLG_EDIT_ADDR     = 3003;
constexpr int IDC_DLG_EDIT_PAIR     = 3004;
constexpr int IDC_DLG_COMBO_RES     = 3009;
constexpr int IDC_DLG_COMBO_BITRATE = 3010;
constexpr int IDC_DLG_CHK_AUDIO     = 3011;
constexpr int IDC_DLG_CHK_TOPMOST   = 3012;
constexpr int IDC_DLG_CHK_SCREENOFF = 3013;
constexpr int IDC_DLG_CHK_STAYAWAKE = 3014;
constexpr int IDC_DLG_CHK_TOUCHES   = 3015;
constexpr int IDC_DLG_COMBO_VSRC    = 3016;
constexpr int IDC_DLG_COMBO_ASRC    = 3017;
constexpr int IDC_DLG_COMBO_FPS     = 3018;
constexpr int IDC_DLG_COMBO_ORIENT  = 3019;
constexpr int IDC_DLG_EDIT_CUST_RES = 3020;
constexpr int IDC_DLG_EDIT_CUST_DPI = 3021;
constexpr int IDC_DLG_BTN_CLEAR_CUST= 3022;
constexpr int IDC_DLG_BTN_QR        = 3023;

struct ModalBtnState {
    bool hoveredPrimary = false;
    bool hoveredCancel = false;
    bool hoveredQr = false;
    bool hoveredRes = false;
    bool hoveredBitrate = false;
    bool hoveredVSrc = false;
    bool hoveredASrc = false;
    bool hoveredFps = false;
    bool hoveredOrient = false;
    bool hoveredClear = false;
};

void CloseModalDialog(HWND hwnd) {
    if (HWND owner = GetWindow(hwnd, GW_OWNER)) {
        EnableWindow(owner, TRUE);
    }
    DestroyWindow(hwnd);
}

LRESULT CALLBACK DlgBtnSubclass(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    auto* hovered = reinterpret_cast<bool*>(dwRefData);
    switch (uMsg) {
    case WM_MOUSEMOVE:
        if (hovered && !*hovered) {
            *hovered = true;
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        if (hovered && *hovered) {
            *hovered = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, DlgBtnSubclass, uIdSubclass);
        break;
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// ============================================================================
// Wireless ADB Dialog
// ============================================================================
struct QrWatchState {
    std::atomic<bool> stop{ false };
    std::atomic<bool> paired{ false };
    std::mutex mutex;
    std::wstring statusText;
    bool isError = false;
};

struct WirelessDlgData {
    bool isDarkMode = false;
    const AdbClient* adb = nullptr;
    bool succeeded = false;
    std::wstring errorMsg;

    bool qrMode = false;
    QrCodeMatrix qrMatrix;
    std::shared_ptr<QrWatchState> qrWatch;

    HWND hwndAddr = nullptr;
    HWND hwndPair = nullptr;
    HWND hwndBtnConnect = nullptr;
    HWND hwndBtnCancel = nullptr;
    HWND hwndBtnQr = nullptr;
    RECT rcFrameAddr = {};
    RECT rcFramePair = {};

    HFONT hFontTitle = nullptr;
    HFONT hFontLabel = nullptr;
    HFONT hFontSmall = nullptr;
    HBRUSH hbrBg = nullptr;
    HBRUSH hbrEdit = nullptr;
    ModalBtnState btnState;
};

void ResizeWirelessDialog(HWND hwnd, int wDip, int hDip) {
    UINT dpi = GetWindowDpi(hwnd);
    RECT rcDesired = { 0, 0, ScaleDpi(wDip, dpi), ScaleDpi(hDip, dpi) };
    AdjustWindowRectEx(&rcDesired, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int w = rcDesired.right - rcDesired.left;
    int h = rcDesired.bottom - rcDesired.top;

    RECT rcCur = {};
    GetWindowRect(hwnd, &rcCur);
    int cx = (rcCur.left + rcCur.right) / 2;
    int cy = (rcCur.top + rcCur.bottom) / 2;
    SetWindowPos(hwnd, nullptr, cx - w / 2, cy - h / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void StopQrWatcher(WirelessDlgData* data) {
    if (data && data->qrWatch) {
        data->qrWatch->stop.store(true);
        data->qrWatch.reset();
    }
}

void StartQrPairingSession(HWND hwnd, WirelessDlgData* data) {
    StopQrWatcher(data);
    if (!data || !data->adb) return;

    std::random_device rd;
    std::mt19937 rng(rd());
    static const char kHex[] = "0123456789abcdef";
    static const char kAlnum[] = "0123456789abcdefghjkmnpqrstuvwxyzABCDEFGHJKMNPQRSTUVWXYZ";

    std::string service = "studio-";
    for (int i = 0; i < 8; ++i) service.push_back(kHex[rng() % 16]);
    std::string pass;
    for (int i = 0; i < 12; ++i) pass.push_back(kAlnum[rng() % (sizeof(kAlnum) - 1)]);

    std::string payload = "WIFI:T:ADB;S:" + service + ";P:" + pass + ";;";
    EncodeQrCode(payload, data->qrMatrix);

    auto watch = std::make_shared<QrWatchState>();
    watch->statusText = L"Waiting for device to scan QR code...";
    data->qrWatch = watch;

    const AdbClient adbCopy = *data->adb;
    std::wstring wService(service.begin(), service.end());
    std::wstring wPass(pass.begin(), pass.end());

    std::thread([hwnd, adbCopy, watch, wService, wPass]() {
        std::wstring pairedIp;
        while (!watch->stop.load()) {
            auto services = adbCopy.ListMdnsServices();
            if (watch->stop.load()) break;

            if (!watch->paired.load()) {
                for (const auto& svc : services) {
                    if (svc.serviceType.find(L"_adb-tls-pairing") != std::wstring::npos &&
                        svc.instanceName == wService) {
                        {
                            std::lock_guard<std::mutex> lock(watch->mutex);
                            watch->statusText = L"Pairing with " + svc.ipPort + L"...";
                            watch->isError = false;
                        }
                        PostMessageW(hwnd, WM_APP_QR_STATUS, 0, 0);

                        std::wstring pairMsg;
                        if (adbCopy.PairWireless(svc.ipPort, wPass, pairMsg)) {
                            watch->paired.store(true);
                            size_t colon = svc.ipPort.rfind(L':');
                            pairedIp = (colon != std::wstring::npos) ? svc.ipPort.substr(0, colon) : L"";
                            {
                                std::lock_guard<std::mutex> lock(watch->mutex);
                                watch->statusText = L"Paired! Connecting...";
                                watch->isError = false;
                            }
                            PostMessageW(hwnd, WM_APP_QR_STATUS, 0, 0);
                        } else if (!watch->stop.load()) {
                            std::lock_guard<std::mutex> lock(watch->mutex);
                            watch->statusText = pairMsg.empty() ? L"Pairing failed. Try scanning again." : pairMsg;
                            watch->isError = true;
                            PostMessageW(hwnd, WM_APP_QR_STATUS, 0, 0);
                        }
                        break;
                    }
                }
            } else {
                // Check if ADB already auto-connected via mDNS
                auto devs = adbCopy.ListDevices();
                for (const auto& d : devs) {
                    if (d.IsOnline() && d.isWireless &&
                        (pairedIp.empty() || d.serial.find(pairedIp) != std::wstring::npos)) {
                        PostMessageW(hwnd, WM_APP_QR_STATUS, 1, 0);
                        return;
                    }
                }
                // Otherwise look up _adb-tls-connect._tcp and connect explicitly
                for (const auto& svc : services) {
                    if (svc.serviceType.find(L"_adb-tls-connect") != std::wstring::npos &&
                        (pairedIp.empty() || svc.ipPort.find(pairedIp) == 0)) {
                        std::wstring connMsg;
                        if (adbCopy.ConnectWireless(svc.ipPort, connMsg)) {
                            PostMessageW(hwnd, WM_APP_QR_STATUS, 1, 0);
                            return;
                        }
                    }
                }
            }

            for (int i = 0; i < 7 && !watch->stop.load(); ++i) {
                Sleep(100);
            }
        }
    }).detach();
}

void DrawQrButtonIcon(HDC hdc, const RECT& rc, const ThemePalette& pal, bool hovered, bool pressed, bool focused, bool active, int radius, UINT dpi, HFONT hFont) {
    int iw = rc.right - rc.left;
    int ih = rc.bottom - rc.top;
    if (iw <= 0 || ih <= 0) return;

    if (!active) {
        RenderFluentButton(hdc, rc, L"", hFont, pal, hovered, pressed, focused, true, false, pal.windowBg, radius);
    } else {
        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hbm = CreateCompatibleBitmap(hdc, iw, ih);
        HBITMAP hbmOld = static_cast<HBITMAP>(SelectObject(hdcMem, hbm));

        HBRUSH hbrParent = CreateSolidBrush(pal.windowBg);
        RECT rcLocal = { 0, 0, iw, ih };
        FillRect(hdcMem, &rcLocal, hbrParent);
        DeleteObject(hbrParent);

        {
            Gdiplus::Graphics gBuf(hdcMem);
            gBuf.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            gBuf.SetPixelOffsetMode(Gdiplus::PixelOffsetModeDefault);

            Gdiplus::Color fillCol = pal.isDark
                ? (pressed ? Gdiplus::Color(255, 38, 38, 38) : (hovered ? Gdiplus::Color(255, 56, 56, 56) : Gdiplus::Color(255, 45, 45, 45)))
                : (pressed ? Gdiplus::Color(255, 235, 235, 235) : (hovered ? Gdiplus::Color(255, 246, 246, 246) : Gdiplus::Color(255, 251, 251, 251)));

            Gdiplus::GraphicsPath borderPath;
            AddRoundedRectPath(borderPath, 0.5f, 0.5f, static_cast<float>(iw) - 1.5f, static_cast<float>(ih) - 1.5f, static_cast<float>(radius));
            Gdiplus::SolidBrush fillBr(fillCol);
            Gdiplus::Pen accentPen(ToGdipColor(pal.accent), 1.0f);
            gBuf.FillPath(&fillBr, &borderPath);
            gBuf.DrawPath(&accentPen, &borderPath);
        }

        BitBlt(hdc, rc.left, rc.top, iw, ih, hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, hbmOld);
        DeleteObject(hbm);
        DeleteDC(hdcMem);
    }

    Gdiplus::Graphics g(hdc);

    COLORREF fg = (pressed && !pal.isDark) ? RGB(80, 80, 80) : pal.textPrimary;
    float s = static_cast<float>(dpi) / 96.0f;
    float tot = 16.0f * s;
    float ox = static_cast<float>(rc.left) + (static_cast<float>(iw) - tot) * 0.5f;
    float oy = static_cast<float>(rc.top) + (static_cast<float>(ih) - tot) * 0.5f;

    // 4 outer corner scanner brackets
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeDefault);
    float stroke = std::max(1.3f, 1.45f * s);
    float arm = 3.8f * s;
    Gdiplus::Pen brkPen(ToGdipColor(fg), stroke);
    brkPen.SetStartCap(Gdiplus::LineCapRound);
    brkPen.SetEndCap(Gdiplus::LineCapRound);
    brkPen.SetLineJoin(Gdiplus::LineJoinRound);

    float l = ox + 0.5f, r = ox + tot - 0.5f;
    float t = oy + 0.5f, b = oy + tot - 0.5f;
    // Top-left
    g.DrawLine(&brkPen, l, t + arm, l, t);
    g.DrawLine(&brkPen, l, t, l + arm, t);
    // Top-right
    g.DrawLine(&brkPen, r - arm, t, r, t);
    g.DrawLine(&brkPen, r, t, r, t + arm);
    // Bottom-left
    g.DrawLine(&brkPen, l, b - arm, l, b);
    g.DrawLine(&brkPen, l, b, l + arm, b);
    // Bottom-right
    g.DrawLine(&brkPen, r - arm, b, r, b);
    g.DrawLine(&brkPen, r, b, r, b - arm);

    // Inner 9x9 DIP QR code (3 crisp finder squares + bottom-right modules)
    g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeDefault);
    Gdiplus::SolidBrush fgBr(ToGdipColor(fg));
    int inner = ScaleDpi(10, dpi);
    int f = std::max(3, ScaleDpi(4, dpi));
    int ft = std::max(1, ScaleDpi(1, dpi));
    int ix = rc.left + (iw - inner) / 2;
    int iy = rc.top + (ih - inner) / 2;
    int gap = inner - f;

    auto drawMiniFinder = [&](int fx, int fy) {
        g.FillRectangle(&fgBr, ix + fx, iy + fy, f, ft);
        g.FillRectangle(&fgBr, ix + fx, iy + fy + f - ft, f, ft);
        g.FillRectangle(&fgBr, ix + fx, iy + fy, ft, f);
        g.FillRectangle(&fgBr, ix + fx + f - ft, iy + fy, ft, f);
    };
    drawMiniFinder(0, 0);
    drawMiniFinder(gap, 0);
    drawMiniFinder(0, gap);

    int m = std::max(1, f / 2);
    g.FillRectangle(&fgBr, ix + gap, iy + gap, m, m);
    g.FillRectangle(&fgBr, ix + gap + f - m, iy + gap + f - m, m, m);
    g.FillRectangle(&fgBr, ix + gap, iy + gap + f - m, m, m);
}

LRESULT CALLBACK WirelessDlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    auto* data = reinterpret_cast<WirelessDlgData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (uMsg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        data = reinterpret_cast<WirelessDlgData*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));

        ApplyDwmWindowTheme(hwnd, data->isDarkMode);
        ThemePalette pal = GetThemePalette(data->isDarkMode);
        data->hbrBg = CreateSolidBrush(pal.windowBg);
        data->hbrEdit = CreateSolidBrush(pal.surfaceBg);

        data->hFontTitle = CreateModernFont(hwnd, 14, FW_NORMAL);
        data->hFontLabel = CreateModernFont(hwnd, 10, FW_NORMAL);
        data->hFontSmall = CreateModernFont(hwnd, 9, FW_NORMAL);

        data->hwndAddr = CreateWindowExW(
            0, WC_EDITW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_EDIT_ADDR)),
            cs->hInstance, nullptr
        );
        SendMessageW(data->hwndAddr, WM_SETFONT, reinterpret_cast<WPARAM>(data->hFontLabel), TRUE);
        SendMessageW(data->hwndAddr, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"192.168.1.50:5555"));

        data->hwndPair = CreateWindowExW(
            0, WC_EDITW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_EDIT_PAIR)),
            cs->hInstance, nullptr
        );
        SendMessageW(data->hwndPair, WM_SETFONT, reinterpret_cast<WPARAM>(data->hFontLabel), TRUE);
        SendMessageW(data->hwndPair, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Optional"));

        data->hwndBtnConnect = CreateWindowExW(
            0, WC_BUTTONW, L"Connect",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_PRIMARY_BTN)),
            cs->hInstance, nullptr
        );
        SetWindowSubclass(data->hwndBtnConnect, DlgBtnSubclass, 1, reinterpret_cast<DWORD_PTR>(&data->btnState.hoveredPrimary));

        data->hwndBtnCancel = CreateWindowExW(
            0, WC_BUTTONW, L"Cancel",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_CANCEL_BTN)),
            cs->hInstance, nullptr
        );
        SetWindowSubclass(data->hwndBtnCancel, DlgBtnSubclass, 2, reinterpret_cast<DWORD_PTR>(&data->btnState.hoveredCancel));

        data->hwndBtnQr = CreateWindowExW(
            0, WC_BUTTONW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_BTN_QR)),
            cs->hInstance, nullptr
        );
        SetWindowSubclass(data->hwndBtnQr, DlgBtnSubclass, 3, reinterpret_cast<DWORD_PTR>(&data->btnState.hoveredQr));

        SetFocus(data->hwndAddr);
        return 0;
    }

    case WM_SIZE: {
        if (!data) return 0;
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        UINT dpi = GetWindowDpi(hwnd);

        int padX = ScaleDpi(24, dpi);
        int boxW = w - padX * 2;
        int boxH = ScaleDpi(32, dpi);
        int insetX = ScaleDpi(10, dpi);
        int insetY = ScaleDpi(7, dpi);

        data->rcFrameAddr = { padX, ScaleDpi(80, dpi), padX + boxW, ScaleDpi(80, dpi) + boxH };
        data->rcFramePair = { padX, ScaleDpi(146, dpi), padX + boxW, ScaleDpi(146, dpi) + boxH };

        SetWindowPos(data->hwndAddr, nullptr, data->rcFrameAddr.left + insetX, data->rcFrameAddr.top + insetY,
            boxW - insetX * 2, boxH - insetY * 2, SWP_NOZORDER);
        SetWindowPos(data->hwndPair, nullptr, data->rcFramePair.left + insetX, data->rcFramePair.top + insetY,
            boxW - insetX * 2, boxH - insetY * 2, SWP_NOZORDER);

        int btnW = ScaleDpi(92, dpi);
        int btnH = ScaleDpi(32, dpi);
        int gap  = ScaleDpi(8, dpi);
        int btnY = h - btnH - ScaleDpi(20, dpi);
        SetWindowPos(data->hwndBtnCancel, nullptr, w - padX - btnW, btnY, btnW, btnH, SWP_NOZORDER);
        SetWindowPos(data->hwndBtnConnect, nullptr, w - padX - btnW * 2 - gap, btnY, btnW, btnH, SWP_NOZORDER);

        int qrSz = ScaleDpi(32, dpi);
        SetWindowPos(data->hwndBtnQr, nullptr, w - padX - qrSz, ScaleDpi(16, dpi), qrSz, qrSz, SWP_NOZORDER);

        int showManual = data->qrMode ? SW_HIDE : SW_SHOW;
        ShowWindow(data->hwndAddr, showManual);
        ShowWindow(data->hwndPair, showManual);
        SetWindowTextW(data->hwndBtnConnect, data->qrMode ? L"New QR" : L"Connect");
        return 0;
    }

    case WM_CTLCOLOREDIT: {
        if (data) {
            ThemePalette pal = GetThemePalette(data->isDarkMode);
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, pal.textPrimary);
            SetBkColor(hdc, pal.surfaceBg);
            return reinterpret_cast<INT_PTR>(data->hbrEdit);
        }
        break;
    }


    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hbm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP hbmOld = static_cast<HBITMAP>(SelectObject(hdcMem, hbm));

        ThemePalette pal = GetThemePalette(data->isDarkMode);
        FillRect(hdcMem, &rc, data->hbrBg);
        SetBkMode(hdcMem, TRANSPARENT);

        UINT dpi = GetWindowDpi(hwnd);
        int padX = ScaleDpi(24, dpi);
        int radius = ScaleDpi(5, dpi);

        SelectObject(hdcMem, data->hFontTitle);
        SetTextColor(hdcMem, pal.textPrimary);
        int qrBtnSz = ScaleDpi(32, dpi);
        RECT rcTitle = { padX, ScaleDpi(20, dpi), rc.right - padX - qrBtnSz - ScaleDpi(8, dpi), ScaleDpi(48, dpi) };
        DrawTextW(hdcMem, data->qrMode ? L"Pair via QR Code" : L"Connect Wireless ADB", -1, &rcTitle, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

        if (data->qrMode) {
            int cardSz = ScaleDpi(188, dpi);
            int cardX = (rc.right - cardSz) / 2;
            int cardY = ScaleDpi(60, dpi);

            Gdiplus::Graphics g(hdcMem);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::GraphicsPath cardPath;
            AddRoundedRectPath(cardPath, static_cast<float>(cardX), static_cast<float>(cardY),
                static_cast<float>(cardSz), static_cast<float>(cardSz), static_cast<float>(ScaleDpi(8, dpi)));
            Gdiplus::SolidBrush whiteBr(Gdiplus::Color(255, 255, 255, 255));
            Gdiplus::Pen borderPen(ToGdipColor(pal.border), 1.0f);
            g.FillPath(&whiteBr, &cardPath);
            g.DrawPath(&borderPen, &cardPath);

            if (data->qrMatrix.size > 0) {
                g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
                int padQr = ScaleDpi(12, dpi);
                int avail = cardSz - padQr * 2;
                int modPx = std::max(1, avail / data->qrMatrix.size);
                int drawSz = modPx * data->qrMatrix.size;
                int ox = cardX + (cardSz - drawSz) / 2;
                int oy = cardY + (cardSz - drawSz) / 2;
                Gdiplus::SolidBrush blackBr(Gdiplus::Color(255, 0, 0, 0));
                for (int my = 0; my < data->qrMatrix.size; ++my) {
                    for (int mx = 0; mx < data->qrMatrix.size; ++mx) {
                        if (data->qrMatrix.Get(mx, my)) {
                            g.FillRectangle(&blackBr, ox + mx * modPx, oy + my * modPx, modPx, modPx);
                        }
                    }
                }
            }

            SelectObject(hdcMem, data->hFontSmall);
            SetTextColor(hdcMem, pal.textSecondary);
            RECT rcHint = { padX, cardY + cardSz + ScaleDpi(10, dpi), rc.right - padX, cardY + cardSz + ScaleDpi(32, dpi) };
            DrawTextW(hdcMem, L"Developer options \u2192 Wireless debugging \u2192 Pair with QR code",
                -1, &rcHint, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);

            std::wstring status;
            bool isErr = false;
            if (data->qrWatch) {
                std::lock_guard<std::mutex> lock(data->qrWatch->mutex);
                status = data->qrWatch->statusText;
                isErr = data->qrWatch->isError;
            }
            if (!status.empty()) {
                SetTextColor(hdcMem, isErr ? pal.statusErr : pal.accent);
                RECT rcSt = { padX, cardY + cardSz + ScaleDpi(32, dpi), rc.right - padX, cardY + cardSz + ScaleDpi(54, dpi) };
                DrawTextW(hdcMem, status.c_str(), -1, &rcSt, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        } else {
            SelectObject(hdcMem, data->hFontLabel);
            SetTextColor(hdcMem, pal.textSecondary);
            RECT rcLbl1 = { padX, ScaleDpi(56, dpi), rc.right - padX, ScaleDpi(76, dpi) };
            DrawTextW(hdcMem, L"IP address and port", -1, &rcLbl1, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

            RECT rcLbl2 = { padX, ScaleDpi(122, dpi), rc.right - padX, ScaleDpi(142, dpi) };
            DrawTextW(hdcMem, L"Pairing code", -1, &rcLbl2, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

            HWND focused = GetFocus();
            RenderFluentInputFrame(hdcMem, data->rcFrameAddr, pal, focused == data->hwndAddr, pal.windowBg, radius);
            RenderFluentInputFrame(hdcMem, data->rcFramePair, pal, focused == data->hwndPair, pal.windowBg, radius);

            if (!data->errorMsg.empty()) {
                SelectObject(hdcMem, data->hFontSmall);
                SetTextColor(hdcMem, pal.statusErr);
                RECT rcErr = { padX, ScaleDpi(186, dpi), rc.right - padX, ScaleDpi(216, dpi) };
                DrawTextW(hdcMem, data->errorMsg.c_str(), -1, &rcErr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            }
        }

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, hbmOld);
        DeleteObject(hbm);
        DeleteDC(hdcMem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DRAWITEM: {
        auto* pDIS = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (!data || pDIS->CtlType != ODT_BUTTON) break;
        ThemePalette pal = GetThemePalette(data->isDarkMode);
        UINT dpi = GetWindowDpi(hwnd);
        int radius = ScaleDpi(6, dpi);
        bool pressed = (pDIS->itemState & ODS_SELECTED) != 0;
        bool focused = (pDIS->itemState & ODS_FOCUS) != 0;

        if (pDIS->CtlID == IDC_DLG_PRIMARY_BTN) {
            const wchar_t* label = data->qrMode ? L"New QR" : L"Connect";
            RenderFluentButton(pDIS->hDC, pDIS->rcItem, label, data->hFontLabel, pal,
                data->btnState.hoveredPrimary && !pressed, pressed, focused, true, true, pal.windowBg, radius);
            return TRUE;
        }
        if (pDIS->CtlID == IDC_DLG_CANCEL_BTN) {
            RenderFluentButton(pDIS->hDC, pDIS->rcItem, L"Cancel", data->hFontLabel, pal,
                data->btnState.hoveredCancel && !pressed, pressed, focused, true, false, pal.windowBg, radius);
            return TRUE;
        }
        if (pDIS->CtlID == IDC_DLG_BTN_QR) {
            DrawQrButtonIcon(pDIS->hDC, pDIS->rcItem, pal,
                data->btnState.hoveredQr && !pressed, pressed, focused, data->qrMode, radius, dpi, data->hFontLabel);
            return TRUE;
        }
        break;
    }

    case WM_APP_QR_STATUS: {
        if (!data) return 0;
        if (wParam == 1) {
            StopQrWatcher(data);
            data->succeeded = true;
            CloseModalDialog(hwnd);
        } else {
            RECT rc;
            GetClientRect(hwnd, &rc);
            UINT dpi = GetWindowDpi(hwnd);
            RECT rcBody = { 0, ScaleDpi(54, dpi), rc.right, rc.bottom - ScaleDpi(56, dpi) };
            InvalidateRect(hwnd, &rcBody, FALSE);
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);
        if ((id == IDC_DLG_EDIT_ADDR || id == IDC_DLG_EDIT_PAIR) && (code == EN_SETFOCUS || code == EN_KILLFOCUS)) {
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (id == IDC_DLG_BTN_QR) {
            data->qrMode = !data->qrMode;
            data->errorMsg.clear();
            if (data->qrMode) {
                ResizeWirelessDialog(hwnd, 420, 368);
                StartQrPairingSession(hwnd, data);
            } else {
                StopQrWatcher(data);
                ResizeWirelessDialog(hwnd, 420, 268);
                SetFocus(data->hwndAddr);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (id == IDC_DLG_CANCEL_BTN) {
            StopQrWatcher(data);
            CloseModalDialog(hwnd);
            return 0;
        }
        if (id == IDC_DLG_PRIMARY_BTN) {
            if (data->qrMode) {
                StartQrPairingSession(hwnd, data);
                RECT rc;
                GetClientRect(hwnd, &rc);
                UINT dpi = GetWindowDpi(hwnd);
                RECT rcBody = { 0, ScaleDpi(54, dpi), rc.right, rc.bottom - ScaleDpi(56, dpi) };
                InvalidateRect(hwnd, &rcBody, FALSE);
                return 0;
            }
            wchar_t addrBuf[128] = {};
            wchar_t pairBuf[64] = {};
            GetWindowTextW(data->hwndAddr, addrBuf, ARRAYSIZE(addrBuf));
            GetWindowTextW(data->hwndPair, pairBuf, ARRAYSIZE(pairBuf));

            std::wstring addr = addrBuf;
            std::wstring pair = pairBuf;
            if (addr.empty()) {
                data->errorMsg = L"Enter an IP address and port.";
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }

            std::wstring msg;
            bool ok = pair.empty()
                ? data->adb->ConnectWireless(addr, msg)
                : data->adb->PairWireless(addr, pair, msg);

            if (ok) {
                data->succeeded = true;
                CloseModalDialog(hwnd);
            } else {
                data->errorMsg = msg;
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        StopQrWatcher(data);
        CloseModalDialog(hwnd);
        return 0;

    case WM_DESTROY:
        if (data) {
            StopQrWatcher(data);
            if (data->hFontTitle) DeleteObject(data->hFontTitle);
            if (data->hFontLabel) DeleteObject(data->hFontLabel);
            if (data->hFontSmall) DeleteObject(data->hFontSmall);
            if (data->hbrBg)      DeleteObject(data->hbrBg);
            if (data->hbrEdit)    DeleteObject(data->hbrEdit);
        }
        return 0;
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

// ============================================================================
// Settings Dialog
// ============================================================================
struct SettingsDlgData {
    bool isDarkMode = false;
    AppSettings* settings = nullptr;
    bool saved = false;
    int resIndex = 0;
    int bitrateIndex = 1;
    int vSrcIndex = 0;
    int aSrcIndex = 0;
    int fpsIndex = 0;
    int orientIndex = 0;

    HWND hwndDropRes = nullptr;
    HWND hwndDropBitrate = nullptr;
    HWND hwndDropVSrc = nullptr;
    HWND hwndDropASrc = nullptr;
    HWND hwndDropFps = nullptr;
    HWND hwndDropOrient = nullptr;
    HWND hwndEditCustRes = nullptr;
    HWND hwndEditCustDpi = nullptr;
    HWND hwndBtnClearCust = nullptr;
    RECT rcFrameCustRes = {};
    RECT rcFrameCustDpi = {};
    HWND hwndChkAudio = nullptr;
    HWND hwndChkTopmost = nullptr;
    HWND hwndChkScreenOff = nullptr;
    HWND hwndChkStayAwake = nullptr;
    HWND hwndChkTouches = nullptr;
    HWND hwndBtnSave = nullptr;
    HWND hwndBtnCancel = nullptr;

    HFONT hFontTitle = nullptr;
    HFONT hFontLabel = nullptr;
    HBRUSH hbrBg = nullptr;
    HBRUSH hbrEdit = nullptr;
    ModalBtnState btnState;
};

static const std::vector<std::wstring> kResItems     = { L"Native", L"1080p", L"720p" };
static const std::vector<std::wstring> kBitrateItems = { L"2 Mbps", L"4 Mbps", L"8 Mbps", L"12 Mbps", L"16 Mbps", L"24 Mbps" };
static const std::vector<std::wstring> kVSrcItems    = { L"Display", L"New Display (Desktop)", L"Simulate Secondary Displays", L"Back Camera", L"Front Camera" };
static const std::vector<std::wstring> kASrcItems    = { L"System Output", L"Microphone" };
static const std::vector<std::wstring> kFpsItems     = { L"Auto", L"120 fps", L"90 fps", L"60 fps", L"45 fps", L"30 fps", L"24 fps" };
static const std::vector<std::wstring> kOrientItems  = { L"Auto", L"Portrait", L"Landscape" };

LRESULT CALLBACK SettingsDlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    auto* data = reinterpret_cast<SettingsDlgData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (uMsg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        data = reinterpret_cast<SettingsDlgData*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));

        ApplyDwmWindowTheme(hwnd, data->isDarkMode);
        ThemePalette pal = GetThemePalette(data->isDarkMode);
        data->hbrBg = CreateSolidBrush(pal.windowBg);
        data->hbrEdit = CreateSolidBrush(pal.surfaceBg);

        data->hFontTitle = CreateModernFont(hwnd, 14, FW_NORMAL);
        data->hFontLabel = CreateModernFont(hwnd, 10, FW_NORMAL);

        data->resIndex     = std::clamp(data->settings->resIndex, 0, static_cast<int>(kResItems.size()) - 1);
        data->bitrateIndex = std::clamp(data->settings->bitrateIndex, 0, static_cast<int>(kBitrateItems.size()) - 1);
        data->vSrcIndex    = std::clamp(data->settings->videoSourceIndex, 0, static_cast<int>(kVSrcItems.size()) - 1);
        data->aSrcIndex    = std::clamp(data->settings->audioSourceIndex, 0, static_cast<int>(kASrcItems.size()) - 1);
        data->fpsIndex     = std::clamp(data->settings->maxFpsIndex, 0, static_cast<int>(kFpsItems.size()) - 1);
        data->orientIndex  = std::clamp(data->settings->lockOrientIndex, 0, static_cast<int>(kOrientItems.size()) - 1);

        auto makeDrop = [&](int id, const wchar_t* txt, bool* hov, UINT_PTR subId) {
            HWND h = CreateWindowExW(0, WC_BUTTONW, txt, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), cs->hInstance, nullptr);
            SetWindowSubclass(h, DlgBtnSubclass, subId, reinterpret_cast<DWORD_PTR>(hov));
            return h;
        };

        data->hwndDropRes     = makeDrop(IDC_DLG_COMBO_RES, kResItems[data->resIndex].c_str(), &data->btnState.hoveredRes, 3);
        data->hwndDropBitrate = makeDrop(IDC_DLG_COMBO_BITRATE, kBitrateItems[data->bitrateIndex].c_str(), &data->btnState.hoveredBitrate, 4);
        data->hwndDropVSrc    = makeDrop(IDC_DLG_COMBO_VSRC, kVSrcItems[data->vSrcIndex].c_str(), &data->btnState.hoveredVSrc, 5);
        data->hwndDropASrc    = makeDrop(IDC_DLG_COMBO_ASRC, kASrcItems[data->aSrcIndex].c_str(), &data->btnState.hoveredASrc, 6);
        data->hwndDropFps     = makeDrop(IDC_DLG_COMBO_FPS, kFpsItems[data->fpsIndex].c_str(), &data->btnState.hoveredFps, 7);
        data->hwndDropOrient  = makeDrop(IDC_DLG_COMBO_ORIENT, kOrientItems[data->orientIndex].c_str(), &data->btnState.hoveredOrient, 8);

        data->hwndEditCustRes = CreateWindowExW(
            0, WC_EDITW, data->settings->customRes.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_EDIT_CUST_RES)),
            cs->hInstance, nullptr
        );
        SendMessageW(data->hwndEditCustRes, WM_SETFONT, reinterpret_cast<WPARAM>(data->hFontLabel), TRUE);
        SendMessageW(data->hwndEditCustRes, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"1920x1080"));

        data->hwndEditCustDpi = CreateWindowExW(
            0, WC_EDITW, data->settings->customDpi.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_EDIT_CUST_DPI)),
            cs->hInstance, nullptr
        );
        SendMessageW(data->hwndEditCustDpi, WM_SETFONT, reinterpret_cast<WPARAM>(data->hFontLabel), TRUE);
        SendMessageW(data->hwndEditCustDpi, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"240"));

        data->hwndBtnClearCust = CreateWindowExW(
            0, WC_BUTTONW, L"Remove",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_BTN_CLEAR_CUST)),
            cs->hInstance, nullptr
        );
        SetWindowSubclass(data->hwndBtnClearCust, DlgBtnSubclass, 9, reinterpret_cast<DWORD_PTR>(&data->btnState.hoveredClear));

        BOOL isDesktop = IsCustomDisplayVideoSource(data->vSrcIndex) ? TRUE : FALSE;
        EnableWindow(data->hwndEditCustRes, isDesktop);
        EnableWindow(data->hwndEditCustDpi, isDesktop);
        EnableWindow(data->hwndBtnClearCust, isDesktop);

        data->hwndChkAudio = CreateWindowExW(
            0, WC_BUTTONW, L"Mirror audio",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_CHK_AUDIO)),
            cs->hInstance, nullptr
        );
        SetWindowTheme(data->hwndChkAudio, data->isDarkMode ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        SendMessageW(data->hwndChkAudio, WM_SETFONT, reinterpret_cast<WPARAM>(data->hFontLabel), TRUE);
        SendMessageW(data->hwndChkAudio, BM_SETCHECK, data->settings->mirrorAudio ? BST_CHECKED : BST_UNCHECKED, 0);

        data->hwndChkTopmost = CreateWindowExW(
            0, WC_BUTTONW, L"Always on top",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_CHK_TOPMOST)),
            cs->hInstance, nullptr
        );
        SetWindowTheme(data->hwndChkTopmost, data->isDarkMode ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        SendMessageW(data->hwndChkTopmost, WM_SETFONT, reinterpret_cast<WPARAM>(data->hFontLabel), TRUE);
        SendMessageW(data->hwndChkTopmost, BM_SETCHECK, data->settings->alwaysOnTop ? BST_CHECKED : BST_UNCHECKED, 0);

        data->hwndChkScreenOff = CreateWindowExW(
            0, WC_BUTTONW, L"Turn screen off",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_CHK_SCREENOFF)),
            cs->hInstance, nullptr
        );
        SetWindowTheme(data->hwndChkScreenOff, data->isDarkMode ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        SendMessageW(data->hwndChkScreenOff, WM_SETFONT, reinterpret_cast<WPARAM>(data->hFontLabel), TRUE);
        SendMessageW(data->hwndChkScreenOff, BM_SETCHECK, data->settings->turnScreenOff ? BST_CHECKED : BST_UNCHECKED, 0);

        data->hwndChkStayAwake = CreateWindowExW(
            0, WC_BUTTONW, L"Stay awake",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_CHK_STAYAWAKE)),
            cs->hInstance, nullptr
        );
        SetWindowTheme(data->hwndChkStayAwake, data->isDarkMode ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        SendMessageW(data->hwndChkStayAwake, WM_SETFONT, reinterpret_cast<WPARAM>(data->hFontLabel), TRUE);
        SendMessageW(data->hwndChkStayAwake, BM_SETCHECK, data->settings->stayAwake ? BST_CHECKED : BST_UNCHECKED, 0);

        data->hwndChkTouches = CreateWindowExW(
            0, WC_BUTTONW, L"Show touches",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_CHK_TOUCHES)),
            cs->hInstance, nullptr
        );
        SetWindowTheme(data->hwndChkTouches, data->isDarkMode ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        SendMessageW(data->hwndChkTouches, WM_SETFONT, reinterpret_cast<WPARAM>(data->hFontLabel), TRUE);
        SendMessageW(data->hwndChkTouches, BM_SETCHECK, data->settings->showTouches ? BST_CHECKED : BST_UNCHECKED, 0);

        data->hwndBtnSave = CreateWindowExW(
            0, WC_BUTTONW, L"OK",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_PRIMARY_BTN)),
            cs->hInstance, nullptr
        );
        SetWindowSubclass(data->hwndBtnSave, DlgBtnSubclass, 1, reinterpret_cast<DWORD_PTR>(&data->btnState.hoveredPrimary));

        data->hwndBtnCancel = CreateWindowExW(
            0, WC_BUTTONW, L"Cancel",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_CANCEL_BTN)),
            cs->hInstance, nullptr
        );
        SetWindowSubclass(data->hwndBtnCancel, DlgBtnSubclass, 2, reinterpret_cast<DWORD_PTR>(&data->btnState.hoveredCancel));
        return 0;
    }

    case WM_SIZE: {
        if (!data) return 0;
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        UINT dpi = GetWindowDpi(hwnd);

        int padX = ScaleDpi(24, dpi);
        int gap = ScaleDpi(12, dpi);
        int halfW = (w - padX * 2 - gap) / 2;

        SetWindowPos(data->hwndDropRes, nullptr, padX, ScaleDpi(82, dpi), halfW, ScaleDpi(32, dpi), SWP_NOZORDER);
        SetWindowPos(data->hwndDropBitrate, nullptr, padX + halfW + gap, ScaleDpi(82, dpi), halfW, ScaleDpi(32, dpi), SWP_NOZORDER);
        SetWindowPos(data->hwndDropVSrc, nullptr, padX, ScaleDpi(146, dpi), halfW, ScaleDpi(32, dpi), SWP_NOZORDER);
        SetWindowPos(data->hwndDropASrc, nullptr, padX + halfW + gap, ScaleDpi(146, dpi), halfW, ScaleDpi(32, dpi), SWP_NOZORDER);
        SetWindowPos(data->hwndDropFps, nullptr, padX, ScaleDpi(210, dpi), halfW, ScaleDpi(32, dpi), SWP_NOZORDER);
        SetWindowPos(data->hwndDropOrient, nullptr, padX + halfW + gap, ScaleDpi(210, dpi), halfW, ScaleDpi(32, dpi), SWP_NOZORDER);

        int boxH = ScaleDpi(32, dpi);
        int clearW = ScaleDpi(76, dpi);
        int dpiW = halfW - clearW - ScaleDpi(8, dpi);
        int insetX = ScaleDpi(10, dpi);
        int insetY = ScaleDpi(7, dpi);
        data->rcFrameCustRes = { padX, ScaleDpi(274, dpi), padX + halfW, ScaleDpi(274, dpi) + boxH };
        data->rcFrameCustDpi = { padX + halfW + gap, ScaleDpi(274, dpi), padX + halfW + gap + dpiW, ScaleDpi(274, dpi) + boxH };

        SetWindowPos(data->hwndEditCustRes, nullptr, data->rcFrameCustRes.left + insetX, data->rcFrameCustRes.top + insetY,
            halfW - insetX * 2, boxH - insetY * 2, SWP_NOZORDER);
        SetWindowPos(data->hwndEditCustDpi, nullptr, data->rcFrameCustDpi.left + insetX, data->rcFrameCustDpi.top + insetY,
            dpiW - insetX * 2, boxH - insetY * 2, SWP_NOZORDER);
        SetWindowPos(data->hwndBtnClearCust, nullptr, data->rcFrameCustDpi.right + ScaleDpi(8, dpi), data->rcFrameCustDpi.top,
            clearW, boxH, SWP_NOZORDER);

        SetWindowPos(data->hwndChkAudio, nullptr, padX, ScaleDpi(322, dpi), halfW, ScaleDpi(24, dpi), SWP_NOZORDER);
        SetWindowPos(data->hwndChkTopmost, nullptr, padX + halfW + gap, ScaleDpi(322, dpi), halfW, ScaleDpi(24, dpi), SWP_NOZORDER);
        SetWindowPos(data->hwndChkScreenOff, nullptr, padX, ScaleDpi(352, dpi), halfW, ScaleDpi(24, dpi), SWP_NOZORDER);
        SetWindowPos(data->hwndChkStayAwake, nullptr, padX + halfW + gap, ScaleDpi(352, dpi), halfW, ScaleDpi(24, dpi), SWP_NOZORDER);
        SetWindowPos(data->hwndChkTouches, nullptr, padX, ScaleDpi(382, dpi), halfW, ScaleDpi(24, dpi), SWP_NOZORDER);

        int btnW = ScaleDpi(92, dpi);
        int btnH = ScaleDpi(32, dpi);
        int btnY = h - btnH - ScaleDpi(20, dpi);
        SetWindowPos(data->hwndBtnCancel, nullptr, w - padX - btnW, btnY, btnW, btnH, SWP_NOZORDER);
        SetWindowPos(data->hwndBtnSave, nullptr, w - padX - btnW * 2 - ScaleDpi(8, dpi), btnY, btnW, btnH, SWP_NOZORDER);
        return 0;
    }

    case WM_CTLCOLOREDIT: {
        if (data) {
            ThemePalette pal = GetThemePalette(data->isDarkMode);
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, pal.textPrimary);
            SetBkColor(hdc, pal.surfaceBg);
            return reinterpret_cast<INT_PTR>(data->hbrEdit);
        }
        break;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        if (data) {
            ThemePalette pal = GetThemePalette(data->isDarkMode);
            HDC hdc = reinterpret_cast<HDC>(wParam);
            HWND hCtrl = reinterpret_cast<HWND>(lParam);
            if (uMsg == WM_CTLCOLORSTATIC &&
                (hCtrl == data->hwndEditCustRes || hCtrl == data->hwndEditCustDpi)) {
                SetTextColor(hdc, pal.textDisabled);
                SetBkColor(hdc, pal.surfaceBg);
                return reinterpret_cast<INT_PTR>(data->hbrEdit);
            }
            SetTextColor(hdc, pal.textPrimary);
            SetBkColor(hdc, pal.windowBg);
            return reinterpret_cast<INT_PTR>(data->hbrBg);
        }
        break;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hbm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP hbmOld = static_cast<HBITMAP>(SelectObject(hdcMem, hbm));

        ThemePalette pal = GetThemePalette(data->isDarkMode);
        FillRect(hdcMem, &rc, data->hbrBg);
        SetBkMode(hdcMem, TRANSPARENT);

        UINT dpi = GetWindowDpi(hwnd);
        int padX = ScaleDpi(24, dpi);
        int gap = ScaleDpi(12, dpi);
        int halfW = ((rc.right - rc.left) - padX * 2 - gap) / 2;

        SelectObject(hdcMem, data->hFontTitle);
        SetTextColor(hdcMem, pal.textPrimary);
        RECT rcTitle = { padX, ScaleDpi(20, dpi), rc.right - padX, ScaleDpi(48, dpi) };
        DrawTextW(hdcMem, L"Settings", -1, &rcTitle, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

        SelectObject(hdcMem, data->hFontLabel);
        SetTextColor(hdcMem, pal.textSecondary);

        RECT rcL1 = { padX, ScaleDpi(58, dpi), padX + halfW, ScaleDpi(78, dpi) };
        DrawTextW(hdcMem, L"Resolution", -1, &rcL1, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

        RECT rcL2 = { padX + halfW + gap, ScaleDpi(58, dpi), rc.right - padX, ScaleDpi(78, dpi) };
        DrawTextW(hdcMem, L"Bitrate", -1, &rcL2, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

        RECT rcL3 = { padX, ScaleDpi(122, dpi), padX + halfW, ScaleDpi(142, dpi) };
        DrawTextW(hdcMem, L"Video source", -1, &rcL3, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

        RECT rcL4 = { padX + halfW + gap, ScaleDpi(122, dpi), rc.right - padX, ScaleDpi(142, dpi) };
        DrawTextW(hdcMem, L"Audio source", -1, &rcL4, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

        RECT rcL5 = { padX, ScaleDpi(186, dpi), padX + halfW, ScaleDpi(206, dpi) };
        DrawTextW(hdcMem, L"Frame rate", -1, &rcL5, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

        RECT rcL6 = { padX + halfW + gap, ScaleDpi(186, dpi), rc.right - padX, ScaleDpi(206, dpi) };
        DrawTextW(hdcMem, L"Orientation", -1, &rcL6, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

        RECT rcL7 = { padX, ScaleDpi(250, dpi), padX + halfW, ScaleDpi(270, dpi) };
        DrawTextW(hdcMem, L"New display resolution", -1, &rcL7, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

        RECT rcL8 = { padX + halfW + gap, ScaleDpi(250, dpi), rc.right - padX, ScaleDpi(270, dpi) };
        DrawTextW(hdcMem, L"New display DPI", -1, &rcL8, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

        int radius = ScaleDpi(5, dpi);
        HWND focused = GetFocus();
        RenderFluentInputFrame(hdcMem, data->rcFrameCustRes, pal, focused == data->hwndEditCustRes, pal.windowBg, radius);
        RenderFluentInputFrame(hdcMem, data->rcFrameCustDpi, pal, focused == data->hwndEditCustDpi, pal.windowBg, radius);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, hbmOld);
        DeleteObject(hbm);
        DeleteDC(hdcMem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DRAWITEM: {
        auto* pDIS = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (!data || pDIS->CtlType != ODT_BUTTON) break;
        ThemePalette pal = GetThemePalette(data->isDarkMode);
        UINT dpi = GetWindowDpi(hwnd);
        int radius = ScaleDpi(6, dpi);
        bool pressed = (pDIS->itemState & ODS_SELECTED) != 0;
        bool focused = (pDIS->itemState & ODS_FOCUS) != 0;

        if (pDIS->CtlID == IDC_DLG_COMBO_RES) {
            RenderFluentDropdown(pDIS->hDC, pDIS->rcItem, kResItems[data->resIndex].c_str(), data->hFontLabel, pal,
                data->btnState.hoveredRes && !pressed, pressed, focused, true, pal.windowBg, radius, dpi);
            return TRUE;
        }
        if (pDIS->CtlID == IDC_DLG_COMBO_BITRATE) {
            RenderFluentDropdown(pDIS->hDC, pDIS->rcItem, kBitrateItems[data->bitrateIndex].c_str(), data->hFontLabel, pal,
                data->btnState.hoveredBitrate && !pressed, pressed, focused, true, pal.windowBg, radius, dpi);
            return TRUE;
        }
        if (pDIS->CtlID == IDC_DLG_COMBO_VSRC) {
            RenderFluentDropdown(pDIS->hDC, pDIS->rcItem, kVSrcItems[data->vSrcIndex].c_str(), data->hFontLabel, pal,
                data->btnState.hoveredVSrc && !pressed, pressed, focused, true, pal.windowBg, radius, dpi);
            return TRUE;
        }
        if (pDIS->CtlID == IDC_DLG_COMBO_ASRC) {
            RenderFluentDropdown(pDIS->hDC, pDIS->rcItem, kASrcItems[data->aSrcIndex].c_str(), data->hFontLabel, pal,
                data->btnState.hoveredASrc && !pressed, pressed, focused, true, pal.windowBg, radius, dpi);
            return TRUE;
        }
        if (pDIS->CtlID == IDC_DLG_COMBO_FPS) {
            RenderFluentDropdown(pDIS->hDC, pDIS->rcItem, kFpsItems[data->fpsIndex].c_str(), data->hFontLabel, pal,
                data->btnState.hoveredFps && !pressed, pressed, focused, true, pal.windowBg, radius, dpi);
            return TRUE;
        }
        if (pDIS->CtlID == IDC_DLG_COMBO_ORIENT) {
            RenderFluentDropdown(pDIS->hDC, pDIS->rcItem, kOrientItems[data->orientIndex].c_str(), data->hFontLabel, pal,
                data->btnState.hoveredOrient && !pressed, pressed, focused, true, pal.windowBg, radius, dpi);
            return TRUE;
        }
        if (pDIS->CtlID == IDC_DLG_BTN_CLEAR_CUST) {
            bool en = IsWindowEnabled(pDIS->hwndItem) != FALSE;
            RenderFluentButton(pDIS->hDC, pDIS->rcItem, L"Remove", data->hFontLabel, pal,
                data->btnState.hoveredClear && !pressed, pressed, focused, en, false, pal.windowBg, radius);
            return TRUE;
        }
        if (pDIS->CtlID == IDC_DLG_PRIMARY_BTN) {
            RenderFluentButton(pDIS->hDC, pDIS->rcItem, L"Save", data->hFontLabel, pal,
                data->btnState.hoveredPrimary && !pressed, pressed, focused, true, true, pal.windowBg, radius);
            return TRUE;
        }
        if (pDIS->CtlID == IDC_DLG_CANCEL_BTN) {
            RenderFluentButton(pDIS->hDC, pDIS->rcItem, L"Cancel", data->hFontLabel, pal,
                data->btnState.hoveredCancel && !pressed, pressed, focused, true, false, pal.windowBg, radius);
            return TRUE;
        }
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);
        if ((id == IDC_DLG_EDIT_CUST_RES || id == IDC_DLG_EDIT_CUST_DPI) && (code == EN_SETFOCUS || code == EN_KILLFOCUS)) {
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (id == IDC_DLG_BTN_CLEAR_CUST) {
            SetWindowTextW(data->hwndEditCustRes, L"");
            SetWindowTextW(data->hwndEditCustDpi, L"");
            data->settings->customRes.clear();
            data->settings->customDpi.clear();
            SaveAppSettings(*data->settings);
            return 0;
        }
        auto handleDrop = [&](HWND hDrop, const std::vector<std::wstring>& items, int& idx) {
            RECT rcA;
            GetWindowRect(hDrop, &rcA);
            int c = ShowFluentDropdownMenu(hwnd, rcA, items, idx, data->isDarkMode, data->hFontLabel);
            if (c >= 0 && c < static_cast<int>(items.size())) {
                idx = c;
                SetWindowTextW(hDrop, items[c].c_str());
                InvalidateRect(hDrop, nullptr, FALSE);
            }
        };
        if (id == IDC_DLG_COMBO_RES)     { handleDrop(data->hwndDropRes, kResItems, data->resIndex); return 0; }
        if (id == IDC_DLG_COMBO_BITRATE) { handleDrop(data->hwndDropBitrate, kBitrateItems, data->bitrateIndex); return 0; }
        if (id == IDC_DLG_COMBO_VSRC) {
            handleDrop(data->hwndDropVSrc, kVSrcItems, data->vSrcIndex);
            BOOL isDesktop = IsCustomDisplayVideoSource(data->vSrcIndex) ? TRUE : FALSE;
            EnableWindow(data->hwndEditCustRes, isDesktop);
            EnableWindow(data->hwndEditCustDpi, isDesktop);
            EnableWindow(data->hwndBtnClearCust, isDesktop);
            InvalidateRect(data->hwndBtnClearCust, nullptr, FALSE);
            return 0;
        }
        if (id == IDC_DLG_COMBO_ASRC)    { handleDrop(data->hwndDropASrc, kASrcItems, data->aSrcIndex); return 0; }
        if (id == IDC_DLG_COMBO_FPS)     { handleDrop(data->hwndDropFps, kFpsItems, data->fpsIndex); return 0; }
        if (id == IDC_DLG_COMBO_ORIENT)  { handleDrop(data->hwndDropOrient, kOrientItems, data->orientIndex); return 0; }
        if (id == IDC_DLG_CANCEL_BTN) {
            CloseModalDialog(hwnd);
            return 0;
        }
        if (id == IDC_DLG_PRIMARY_BTN) {
            data->settings->resIndex = data->resIndex;
            data->settings->bitrateIndex = data->bitrateIndex;
            data->settings->videoSourceIndex = data->vSrcIndex;
            data->settings->audioSourceIndex = data->aSrcIndex;
            data->settings->maxFpsIndex = data->fpsIndex;
            data->settings->lockOrientIndex = data->orientIndex;
            data->settings->mirrorAudio = (SendMessageW(data->hwndChkAudio, BM_GETCHECK, 0, 0) == BST_CHECKED);
            data->settings->alwaysOnTop = (SendMessageW(data->hwndChkTopmost, BM_GETCHECK, 0, 0) == BST_CHECKED);
            data->settings->turnScreenOff = (SendMessageW(data->hwndChkScreenOff, BM_GETCHECK, 0, 0) == BST_CHECKED);
            data->settings->stayAwake = (SendMessageW(data->hwndChkStayAwake, BM_GETCHECK, 0, 0) == BST_CHECKED);
            data->settings->showTouches = (SendMessageW(data->hwndChkTouches, BM_GETCHECK, 0, 0) == BST_CHECKED);

            wchar_t resBuf[64] = {}, dpiBuf[32] = {};
            GetWindowTextW(data->hwndEditCustRes, resBuf, ARRAYSIZE(resBuf));
            GetWindowTextW(data->hwndEditCustDpi, dpiBuf, ARRAYSIZE(dpiBuf));
            data->settings->customRes = resBuf;
            data->settings->customDpi = dpiBuf;
            SaveAppSettings(*data->settings);

            data->saved = true;
            CloseModalDialog(hwnd);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        CloseModalDialog(hwnd);
        return 0;

    case WM_DESTROY:
        if (data) {
            if (data->hFontTitle) DeleteObject(data->hFontTitle);
            if (data->hFontLabel) DeleteObject(data->hFontLabel);
            if (data->hbrBg)      DeleteObject(data->hbrBg);
            if (data->hbrEdit)    DeleteObject(data->hbrEdit);
        }
        return 0;
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

template <typename DataT>
void RunModalDialog(HWND hwndParent, const wchar_t* className, WNDPROC proc, const wchar_t* title, int wDip, int hDip, DataT& data) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = { sizeof(wc) };
    if (!GetClassInfoExW(hInst, className, &wc)) {
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = proc;
        wc.hInstance = hInst;
        wc.hIcon = static_cast<HICON>(LoadImageW(
            hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
        wc.hIconSm = static_cast<HICON>(LoadImageW(
            hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = className;
        RegisterClassExW(&wc);
    }

    UINT dpi = GetWindowDpi(hwndParent);
    RECT rcDesired = { 0, 0, ScaleDpi(wDip, dpi), ScaleDpi(hDip, dpi) };
    AdjustWindowRectEx(&rcDesired, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int w = rcDesired.right - rcDesired.left;
    int h = rcDesired.bottom - rcDesired.top;

    RECT rcParent;
    GetWindowRect(hwndParent, &rcParent);
    int x = rcParent.left + ((rcParent.right - rcParent.left) - w) / 2;
    int y = rcParent.top + ((rcParent.bottom - rcParent.top) - h) / 2;

    EnableWindow(hwndParent, FALSE);
    HWND hwndDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME, className, title,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | WS_VISIBLE,
        x, y, w, h, hwndParent, nullptr, hInst, &data
    );

    if (hwndDlg) {
        ShowWindow(hwndDlg, SW_SHOW);
        UpdateWindow(hwndDlg);
        MSG msg;
        while (IsWindow(hwndDlg) && GetMessageW(&msg, nullptr, 0, 0)) {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
                CloseModalDialog(hwndDlg);
                break;
            }
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
                SendMessageW(hwndDlg, WM_COMMAND, MAKEWPARAM(IDC_DLG_PRIMARY_BTN, BN_CLICKED), 0);
                continue;
            }
            if (!IsDialogMessageW(hwndDlg, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    if (!IsWindowEnabled(hwndParent)) {
        EnableWindow(hwndParent, TRUE);
    }
}

} // namespace

bool ShowWirelessAdbDialog(HWND hwndParent, bool isDarkMode, const AdbClient& adb) {
    WirelessDlgData data;
    data.isDarkMode = isDarkMode;
    data.adb = &adb;
    RunModalDialog(hwndParent, L"Win11WirelessAdbDlg", WirelessDlgProc, L"Connect Wireless ADB", 420, 268, data);
    return data.succeeded;
}

bool ShowSettingsDialog(HWND hwndParent, bool isDarkMode, AppSettings& settings) {
    SettingsDlgData data;
    data.isDarkMode = isDarkMode;
    data.settings = &settings;
    RunModalDialog(hwndParent, L"Win11SettingsDlg", SettingsDlgProc, L"Settings", 480, 485, data);
    return data.saved;
}

static std::wstring GetSettingsIniPath() {
    wchar_t baseDir[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, baseDir))) {
        GetTempPathW(MAX_PATH, baseDir);
    }
    std::wstring appDir = std::wstring(baseDir) + L"\\AndroidDisplay";
    CreateDirectoryW(appDir.c_str(), nullptr);
    return appDir + L"\\settings.ini";
}

void LoadAppSettings(AppSettings& s) {
    std::wstring ini = GetSettingsIniPath();
    const wchar_t* sec = L"Settings";
    s.mirrorAudio      = GetPrivateProfileIntW(sec, L"MirrorAudio", 1, ini.c_str()) != 0;
    s.alwaysOnTop      = GetPrivateProfileIntW(sec, L"AlwaysOnTop", 0, ini.c_str()) != 0;
    s.turnScreenOff    = GetPrivateProfileIntW(sec, L"TurnScreenOff", 0, ini.c_str()) != 0;
    s.stayAwake        = GetPrivateProfileIntW(sec, L"StayAwake", 0, ini.c_str()) != 0;
    s.showTouches      = GetPrivateProfileIntW(sec, L"ShowTouches", 0, ini.c_str()) != 0;
    s.resIndex         = std::clamp<int>(GetPrivateProfileIntW(sec, L"ResIndex", 0, ini.c_str()), 0, 2);
    s.bitrateIndex     = std::clamp<int>(GetPrivateProfileIntW(sec, L"BitrateIndex", 2, ini.c_str()), 0, 5);
    s.videoSourceIndex = std::clamp<int>(GetPrivateProfileIntW(sec, L"VideoSource", 0, ini.c_str()), 0, 4);
    s.audioSourceIndex = std::clamp<int>(GetPrivateProfileIntW(sec, L"AudioSource", 0, ini.c_str()), 0, 1);
    s.maxFpsIndex      = std::clamp<int>(GetPrivateProfileIntW(sec, L"MaxFps", 0, ini.c_str()), 0, 6);
    s.lockOrientIndex  = std::clamp<int>(GetPrivateProfileIntW(sec, L"LockOrient", 0, ini.c_str()), 0, 2);

    wchar_t buf[64] = {};
    GetPrivateProfileStringW(sec, L"CustomRes", L"", buf, ARRAYSIZE(buf), ini.c_str());
    s.customRes = buf;
    GetPrivateProfileStringW(sec, L"CustomDpi", L"", buf, ARRAYSIZE(buf), ini.c_str());
    s.customDpi = buf;
}

void SaveAppSettings(const AppSettings& s) {
    std::wstring ini = GetSettingsIniPath();
    const wchar_t* sec = L"Settings";
    auto writeInt = [&](const wchar_t* k, int v) {
        WritePrivateProfileStringW(sec, k, std::to_wstring(v).c_str(), ini.c_str());
    };
    writeInt(L"MirrorAudio", s.mirrorAudio ? 1 : 0);
    writeInt(L"AlwaysOnTop", s.alwaysOnTop ? 1 : 0);
    writeInt(L"TurnScreenOff", s.turnScreenOff ? 1 : 0);
    writeInt(L"StayAwake", s.stayAwake ? 1 : 0);
    writeInt(L"ShowTouches", s.showTouches ? 1 : 0);
    writeInt(L"ResIndex", s.resIndex);
    writeInt(L"BitrateIndex", s.bitrateIndex);
    writeInt(L"VideoSource", s.videoSourceIndex);
    writeInt(L"AudioSource", s.audioSourceIndex);
    writeInt(L"MaxFps", s.maxFpsIndex);
    writeInt(L"LockOrient", s.lockOrientIndex);
    WritePrivateProfileStringW(sec, L"CustomRes", s.customRes.empty() ? nullptr : s.customRes.c_str(), ini.c_str());
    WritePrivateProfileStringW(sec, L"CustomDpi", s.customDpi.empty() ? nullptr : s.customDpi.c_str(), ini.c_str());
}
