#pragma once

#include <windows.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <gdiplus.h>
#include <string>
#include <vector>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

struct ThemePalette {
    bool isDark = false;

    COLORREF windowBg = 0;      // #202020 / #F3F3F3
    COLORREF surfaceBg = 0;     // #2D2D2D / #FBFBFB
    COLORREF stageBg = 0;       // #181818 / #EAEAEA
    COLORREF border = 0;        // #3E3E3E / #DADADA
    COLORREF divider = 0;       // #333333 / #E0E0E0

    COLORREF textPrimary = 0;   // #FFFFFF / #1B1B1B
    COLORREF textSecondary = 0; // #A0A0A0 / #646464
    COLORREF textDisabled = 0;  // #6E6E6E / #9E9E9E

    COLORREF accent = 0;        // #4CC2FF / #0067C0
    COLORREF accentHover = 0;
    COLORREF accentPressed = 0;
    COLORREF accentText = 0;    // #000000 / #FFFFFF

    COLORREF statusErr = 0;
};

ThemePalette GetThemePalette(bool isDarkMode);
bool IsSystemInDarkMode();
UINT GetWindowDpi(HWND hwnd);
int ScaleDpi(int value, UINT dpi);
HFONT CreateModernFont(HWND hwnd, int pointSize, int weight, const wchar_t* faceName = L"Segoe UI");
void ApplyDwmWindowTheme(HWND hwnd, bool isDarkMode);

void AddRoundedRectPath(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float radius);
Gdiplus::Color ToGdipColor(COLORREF c, BYTE alpha = 255);

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
);

void RenderFluentInputFrame(
    HDC hdc,
    const RECT& rc,
    const ThemePalette& pal,
    bool isFocused,
    COLORREF parentBg,
    int cornerRadius
);

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
);

// Opens a Windows 11 rounded popup menu anchored below anchorScreenRect.
// Returns selected index [0..items.size()-1] or -1 if dismissed.
int ShowFluentDropdownMenu(
    HWND hwndOwner,
    const RECT& anchorScreenRect,
    const std::vector<std::wstring>& items,
    int selectedIdx,
    bool isDarkMode,
    HFONT hFont
);
