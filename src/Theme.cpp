#include "Theme.h"
#include <strsafe.h>

ThemePalette GetThemePalette(bool isDarkMode) {
    ThemePalette p;
    p.isDark = isDarkMode;
    if (isDarkMode) {
        p.windowBg      = RGB(32, 32, 32);    // #202020
        p.surfaceBg     = RGB(45, 45, 45);    // #2D2D2D
        p.stageBg       = RGB(24, 24, 24);    // #181818
        p.border        = RGB(62, 62, 62);    // #3E3E3E
        p.divider       = RGB(48, 48, 48);    // #303030

        p.textPrimary   = RGB(255, 255, 255);
        p.textSecondary = RGB(160, 160, 160);
        p.textDisabled  = RGB(110, 110, 110);

        p.accent        = RGB(76, 194, 255);
        p.accentHover   = RGB(100, 204, 255);
        p.accentPressed = RGB(60, 175, 235);
        p.accentText    = RGB(0, 0, 0);

        p.statusErr     = RGB(255, 153, 164);
    } else {
        p.windowBg      = RGB(243, 243, 243); // #F3F3F3
        p.surfaceBg     = RGB(251, 251, 251); // #FBFBFB
        p.stageBg       = RGB(234, 234, 234); // #EAEAEA
        p.border        = RGB(218, 218, 218); // #DADADA
        p.divider       = RGB(224, 224, 224); // #E0E0E0

        p.textPrimary   = RGB(27, 27, 27);
        p.textSecondary = RGB(100, 100, 100);
        p.textDisabled  = RGB(158, 158, 158);

        p.accent        = RGB(0, 103, 192);
        p.accentHover   = RGB(25, 117, 197);
        p.accentPressed = RGB(0, 90, 158);
        p.accentText    = RGB(255, 255, 255);

        p.statusErr     = RGB(196, 43, 28);
    }
    return p;
}

bool IsSystemInDarkMode() {
    DWORD data = 1;
    DWORD dataSize = sizeof(data);
    LONG result = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &data,
        &dataSize
    );
    return (result == ERROR_SUCCESS) && (data == 0);
}

UINT GetWindowDpi(HWND hwnd) {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32 && hwnd) {
        using GetDpiForWindowProc = UINT(WINAPI*)(HWND);
        auto pfn = reinterpret_cast<GetDpiForWindowProc>(GetProcAddress(hUser32, "GetDpiForWindow"));
        if (pfn) {
            UINT dpi = pfn(hwnd);
            if (dpi > 0) return dpi;
        }
    }
    HDC hdc = GetDC(hwnd);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(hwnd, hdc);
    return (dpi > 0) ? static_cast<UINT>(dpi) : 96;
}

int ScaleDpi(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

HFONT CreateModernFont(HWND hwnd, int pointSize, int weight, const wchar_t* faceName) {
    UINT dpi = GetWindowDpi(hwnd);
    int height = -MulDiv(pointSize, static_cast<int>(dpi), 72);

    LOGFONTW lf = {};
    lf.lfHeight = height;
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    lf.lfCharSet = DEFAULT_CHARSET;
    StringCchCopyW(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName), faceName);

    return CreateFontIndirectW(&lf);
}

void ApplyDwmWindowTheme(HWND hwnd, bool isDarkMode) {
    BOOL useDark = isDarkMode ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));

    DWM_WINDOW_CORNER_PREFERENCE cornerPreference = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPreference, sizeof(cornerPreference));
}

void AddRoundedRectPath(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float radius) {
    float r = radius;
    if (r * 2.0f > w) r = w * 0.5f;
    if (r * 2.0f > h) r = h * 0.5f;
    if (r <= 0.0f) {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    float d = 2.0f * r;
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

Gdiplus::Color ToGdipColor(COLORREF c, BYTE alpha) {
    return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

static void ResolveNeutralControlColors(
    const ThemePalette& pal,
    bool isHovered,
    bool isPressed,
    bool isEnabled,
    Gdiplus::Color& fillColor,
    Gdiplus::Color& borderColor,
    COLORREF& textColor
) {
    if (!isEnabled) {
        fillColor   = ToGdipColor(pal.isDark ? RGB(40, 40, 40) : RGB(238, 238, 238));
        borderColor = ToGdipColor(pal.isDark ? RGB(52, 52, 52) : RGB(222, 222, 222));
        textColor   = pal.textDisabled;
        return;
    }
    if (pal.isDark) {
        if (isPressed) {
            fillColor   = Gdiplus::Color(255, 38, 38, 38);
            borderColor = Gdiplus::Color(255, 55, 55, 55);
            textColor   = RGB(195, 195, 195);
        } else if (isHovered) {
            fillColor   = Gdiplus::Color(255, 56, 56, 56);
            borderColor = Gdiplus::Color(255, 80, 80, 80);
            textColor   = RGB(255, 255, 255);
        } else {
            fillColor   = Gdiplus::Color(255, 45, 45, 45);
            borderColor = Gdiplus::Color(255, 62, 62, 62);
            textColor   = RGB(255, 255, 255);
        }
    } else {
        if (isPressed) {
            fillColor   = Gdiplus::Color(255, 235, 235, 235);
            borderColor = Gdiplus::Color(255, 175, 175, 175);
            textColor   = RGB(80, 80, 80);
        } else if (isHovered) {
            fillColor   = Gdiplus::Color(255, 246, 246, 246);
            borderColor = Gdiplus::Color(255, 190, 190, 190);
            textColor   = RGB(27, 27, 27);
        } else {
            fillColor   = Gdiplus::Color(255, 251, 251, 251);
            borderColor = Gdiplus::Color(255, 218, 218, 218);
            textColor   = RGB(27, 27, 27);
        }
    }
}

void RenderFluentButton(
    HDC hdc,
    const RECT& rc,
    const wchar_t* text,
    HFONT hFont,
    const ThemePalette& pal,
    bool isHovered,
    bool isPressed,
    bool isFocused,
    bool isEnabled,
    bool isAccent,
    COLORREF parentBg,
    int cornerRadius
) {
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hbmMem = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP hbmOld = static_cast<HBITMAP>(SelectObject(hdcMem, hbmMem));

    HBRUSH hbrParent = CreateSolidBrush(parentBg);
    RECT rcLocal = { 0, 0, w, h };
    FillRect(hdcMem, &rcLocal, hbrParent);
    DeleteObject(hbrParent);

    {
        Gdiplus::Graphics g(hdcMem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeDefault);

        Gdiplus::Color fillColor;
        Gdiplus::Color borderColor;
        COLORREF textColor;

        if (isEnabled && isAccent) {
            if (isPressed) {
                fillColor   = ToGdipColor(pal.accentPressed);
                borderColor = ToGdipColor(pal.accentPressed);
            } else if (isHovered) {
                fillColor   = ToGdipColor(pal.accentHover);
                borderColor = ToGdipColor(pal.accentHover);
            } else {
                fillColor   = ToGdipColor(pal.accent);
                borderColor = ToGdipColor(pal.accent);
            }
            textColor = pal.accentText;
        } else {
            ResolveNeutralControlColors(pal, isHovered, isPressed, isEnabled, fillColor, borderColor, textColor);
        }

        if (isFocused && isEnabled && !isPressed && !isAccent) {
            borderColor = ToGdipColor(pal.accent);
        }

        Gdiplus::GraphicsPath path;
        AddRoundedRectPath(path, 0.5f, 0.5f, static_cast<float>(w) - 1.5f, static_cast<float>(h) - 1.5f, static_cast<float>(cornerRadius));

        Gdiplus::SolidBrush brush(fillColor);
        g.FillPath(&brush, &path);

        Gdiplus::Pen pen(borderColor, 1.0f);
        g.DrawPath(&pen, &path);

        SelectObject(hdcMem, hFont);
        SetTextColor(hdcMem, textColor);
        SetBkMode(hdcMem, TRANSPARENT);
        RECT rcText = rcLocal;
        if (isPressed && isEnabled) {
            OffsetRect(&rcText, 0, 1);
        }
        DrawTextW(hdcMem, text, -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    BitBlt(hdc, rc.left, rc.top, w, h, hdcMem, 0, 0, SRCCOPY);
    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbmMem);
    DeleteDC(hdcMem);
}

void RenderFluentInputFrame(
    HDC hdc,
    const RECT& rc,
    const ThemePalette& pal,
    bool isFocused,
    COLORREF parentBg,
    int cornerRadius
) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeDefault);

    Gdiplus::SolidBrush parentBr(ToGdipColor(parentBg));
    g.FillRectangle(
        &parentBr,
        static_cast<INT>(rc.left),
        static_cast<INT>(rc.top),
        static_cast<INT>(rc.right - rc.left),
        static_cast<INT>(rc.bottom - rc.top)
    );

    Gdiplus::GraphicsPath path;
    AddRoundedRectPath(
        path,
        static_cast<float>(rc.left) + 0.5f,
        static_cast<float>(rc.top) + 0.5f,
        static_cast<float>(rc.right - rc.left) - 1.5f,
        static_cast<float>(rc.bottom - rc.top) - 1.5f,
        static_cast<float>(cornerRadius)
    );

    Gdiplus::SolidBrush fillBr(ToGdipColor(pal.surfaceBg));
    g.FillPath(&fillBr, &path);

    Gdiplus::Pen borderPen(ToGdipColor(isFocused ? pal.accent : pal.border), isFocused ? 1.5f : 1.0f);
    g.DrawPath(&borderPen, &path);
}

void RenderFluentDropdown(
    HDC hdc,
    const RECT& rc,
    const wchar_t* text,
    HFONT hFont,
    const ThemePalette& pal,
    bool isHovered,
    bool isPressed,
    bool isFocused,
    bool isEnabled,
    COLORREF parentBg,
    int cornerRadius,
    UINT dpi
) {
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hbmMem = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP hbmOld = static_cast<HBITMAP>(SelectObject(hdcMem, hbmMem));

    HBRUSH hbrParent = CreateSolidBrush(parentBg);
    RECT rcLocal = { 0, 0, w, h };
    FillRect(hdcMem, &rcLocal, hbrParent);
    DeleteObject(hbrParent);

    {
        Gdiplus::Graphics g(hdcMem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeDefault);

        Gdiplus::Color fillColor;
        Gdiplus::Color borderColor;
        COLORREF textColor;
        ResolveNeutralControlColors(pal, isHovered, isPressed, isEnabled, fillColor, borderColor, textColor);

        if (isFocused && isEnabled && !isPressed) {
            borderColor = ToGdipColor(pal.accent);
        }

        Gdiplus::GraphicsPath path;
        AddRoundedRectPath(path, 0.5f, 0.5f, static_cast<float>(w) - 1.5f, static_cast<float>(h) - 1.5f, static_cast<float>(cornerRadius));

        Gdiplus::SolidBrush brush(fillColor);
        g.FillPath(&brush, &path);

        Gdiplus::Pen pen(borderColor, (isFocused && isEnabled && !isPressed) ? 1.5f : 1.0f);
        g.DrawPath(&pen, &path);

        // Draw crisp Windows 11 chevron on the right
        int chevPadR = ScaleDpi(12, dpi);
        float chevW  = static_cast<float>(ScaleDpi(8, dpi));
        float chevH  = static_cast<float>(ScaleDpi(4, dpi));
        float cx = static_cast<float>(w - chevPadR) - chevW * 0.5f;
        float cy = static_cast<float>(h) * 0.5f;

        Gdiplus::PointF pts[3] = {
            { cx - chevW * 0.5f, cy - chevH * 0.4f },
            { cx,                cy + chevH * 0.6f },
            { cx + chevW * 0.5f, cy - chevH * 0.4f }
        };
        Gdiplus::Pen chevPen(ToGdipColor(isEnabled ? pal.textSecondary : pal.textDisabled), 1.3f);
        chevPen.SetStartCap(Gdiplus::LineCapRound);
        chevPen.SetEndCap(Gdiplus::LineCapRound);
        chevPen.SetLineJoin(Gdiplus::LineJoinRound);
        g.DrawLines(&chevPen, pts, 3);

        // Draw label text
        SelectObject(hdcMem, hFont);
        SetTextColor(hdcMem, textColor);
        SetBkMode(hdcMem, TRANSPARENT);
        int padL = ScaleDpi(12, dpi);
        int padR = chevPadR + ScaleDpi(14, dpi);
        RECT rcText = { padL, 0, w - padR, h };
        DrawTextW(hdcMem, text ? text : L"", -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    BitBlt(hdc, rc.left, rc.top, w, h, hdcMem, 0, 0, SRCCOPY);
    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbmMem);
    DeleteDC(hdcMem);
}

namespace {

struct FluentMenuData {
    std::vector<std::wstring> items;
    int selectedIdx = -1;
    int hoveredIdx = -1;
    int chosenIdx = -1;
    bool isDarkMode = false;
    HFONT hFont = nullptr;
    int itemH = 32;
    int padY = 4;
};

LRESULT CALLBACK FluentMenuProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    auto* data = reinterpret_cast<FluentMenuData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (uMsg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        data = reinterpret_cast<FluentMenuData*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
        ApplyDwmWindowTheme(hwnd, data->isDarkMode);
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_MOUSEMOVE: {
        if (!data) break;
        int y = static_cast<short>(HIWORD(lParam));
        int idx = (y - data->padY) / data->itemH;
        if (y < data->padY || idx < 0 || idx >= static_cast<int>(data->items.size())) {
            idx = -1;
        }
        if (idx != data->hoveredIdx) {
            data->hoveredIdx = idx;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (!data) break;
        int y = static_cast<short>(HIWORD(lParam));
        int idx = (y - data->padY) / data->itemH;
        if (y >= data->padY && idx >= 0 && idx < static_cast<int>(data->items.size())) {
            data->chosenIdx = idx;
        }
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_KEYDOWN: {
        if (!data) break;
        int count = static_cast<int>(data->items.size());
        if (wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (wParam == VK_RETURN && data->hoveredIdx >= 0 && data->hoveredIdx < count) {
            data->chosenIdx = data->hoveredIdx;
            DestroyWindow(hwnd);
            return 0;
        }
        if (wParam == VK_DOWN && count > 0) {
            data->hoveredIdx = (data->hoveredIdx + 1) % count;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_UP && count > 0) {
            data->hoveredIdx = (data->hoveredIdx <= 0) ? (count - 1) : (data->hoveredIdx - 1);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
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
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hbm = CreateCompatibleBitmap(hdc, w, h);
        HBITMAP hbmOld = static_cast<HBITMAP>(SelectObject(hdcMem, hbm));

        ThemePalette pal = GetThemePalette(data->isDarkMode);
        COLORREF menuBg = data->isDarkMode ? RGB(44, 44, 44) : RGB(249, 249, 249);
        COLORREF itemHover = data->isDarkMode ? RGB(58, 58, 58) : RGB(238, 238, 238);
        UINT dpi = GetWindowDpi(hwnd);

        HBRUSH hbrBg = CreateSolidBrush(menuBg);
        FillRect(hdcMem, &rc, hbrBg);
        DeleteObject(hbrBg);

        {
            Gdiplus::Graphics g(hdcMem);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeDefault);

            // Subtle 1px outer border
            Gdiplus::Pen borderPen(ToGdipColor(pal.border), 1.0f);
            g.DrawRectangle(&borderPen, 0.5f, 0.5f, static_cast<float>(w) - 1.0f, static_cast<float>(h) - 1.0f);

            int padX = ScaleDpi(5, dpi);
            float r = static_cast<float>(ScaleDpi(4, dpi));
            SelectObject(hdcMem, data->hFont);
            SetBkMode(hdcMem, TRANSPARENT);

            for (size_t i = 0; i < data->items.size(); ++i) {
                int iy = data->padY + static_cast<int>(i) * data->itemH;
                RECT rcItem = { padX, iy + 1, w - padX, iy + data->itemH - 1 };
                bool isSel = (static_cast<int>(i) == data->selectedIdx);
                bool isHov = (static_cast<int>(i) == data->hoveredIdx);

                if (isHov || isSel) {
                    Gdiplus::GraphicsPath p;
                    AddRoundedRectPath(
                        p,
                        static_cast<float>(rcItem.left),
                        static_cast<float>(rcItem.top),
                        static_cast<float>(rcItem.right - rcItem.left),
                        static_cast<float>(rcItem.bottom - rcItem.top),
                        r
                    );
                    Gdiplus::SolidBrush br(ToGdipColor(itemHover));
                    g.FillPath(&br, &p);
                }

                if (isSel) {
                    int pillW = ScaleDpi(3, dpi);
                    int pillH = ScaleDpi(16, dpi);
                    int pillY = rcItem.top + ((rcItem.bottom - rcItem.top) - pillH) / 2;
                    Gdiplus::GraphicsPath pill;
                    AddRoundedRectPath(pill, static_cast<float>(rcItem.left + ScaleDpi(3, dpi)),
                        static_cast<float>(pillY), static_cast<float>(pillW), static_cast<float>(pillH), 1.5f);
                    Gdiplus::SolidBrush pillBr(ToGdipColor(pal.accent));
                    g.FillPath(&pillBr, &pill);
                }

                SetTextColor(hdcMem, pal.textPrimary);
                RECT rcTxt = { rcItem.left + ScaleDpi(12, dpi), rcItem.top, rcItem.right - ScaleDpi(8, dpi), rcItem.bottom };
                DrawTextW(hdcMem, data->items[i].c_str(), -1, &rcTxt, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }

        BitBlt(hdc, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, hbmOld);
        DeleteObject(hbm);
        DeleteDC(hdcMem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

} // namespace

int ShowFluentDropdownMenu(
    HWND hwndOwner,
    const RECT& anchorScreenRect,
    const std::vector<std::wstring>& items,
    int selectedIdx,
    bool isDarkMode,
    HFONT hFont
) {
    if (items.empty()) return -1;

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    const wchar_t MENU_CLASS[] = L"Win11FluentMenuClass";
    WNDCLASSEXW wc = { sizeof(wc) };
    if (!GetClassInfoExW(hInst, MENU_CLASS, &wc)) {
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = FluentMenuProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = MENU_CLASS;
        RegisterClassExW(&wc);
    }

    UINT dpi = GetWindowDpi(hwndOwner);
    FluentMenuData data;
    data.items = items;
    data.selectedIdx = selectedIdx;
    data.hoveredIdx = selectedIdx;
    data.isDarkMode = isDarkMode;
    data.hFont = hFont;
    data.itemH = ScaleDpi(32, dpi);
    data.padY  = ScaleDpi(4, dpi);

    int w = anchorScreenRect.right - anchorScreenRect.left;
    if (hFont) {
        HDC hdcMeasure = GetDC(hwndOwner);
        if (hdcMeasure) {
            HGDIOBJ hOldFont = SelectObject(hdcMeasure, hFont);
            int padLR = ScaleDpi(36, dpi); // 4px outer + 12px left + 16px right + 4px outer
            for (const auto& item : items) {
                SIZE sz = {};
                if (GetTextExtentPoint32W(hdcMeasure, item.c_str(), static_cast<int>(item.size()), &sz)) {
                    w = (std::max)(w, static_cast<int>(sz.cx) + padLR);
                }
            }
            SelectObject(hdcMeasure, hOldFont);
            ReleaseDC(hwndOwner, hdcMeasure);
        }
    }
    int h = data.padY * 2 + static_cast<int>(items.size()) * data.itemH;
    int x = anchorScreenRect.left;
    int y = anchorScreenRect.bottom + ScaleDpi(4, dpi);

    // Keep within monitor work area
    HMONITOR hMon = MonitorFromRect(&anchorScreenRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(hMon, &mi)) {
        if (y + h > mi.rcWork.bottom) {
            y = anchorScreenRect.top - h - ScaleDpi(4, dpi);
        }
        if (x + w > mi.rcWork.right) {
            x = mi.rcWork.right - w;
        }
    }

    HWND hwndMenu = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        MENU_CLASS, L"",
        WS_POPUP | WS_VISIBLE,
        x, y, w, h,
        hwndOwner, nullptr, hInst, &data
    );

    if (hwndMenu) {
        ShowWindow(hwndMenu, SW_SHOW);
        SetForegroundWindow(hwndMenu);
        SetFocus(hwndMenu);

        MSG msg;
        while (IsWindow(hwndMenu) && GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    return data.chosenIdx;
}
