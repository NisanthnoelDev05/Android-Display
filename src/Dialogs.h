#pragma once

#include "AdbClient.h"
#include "Theme.h"
#include <windows.h>

struct AppSettings {
    bool mirrorAudio = true;
    bool alwaysOnTop = false;
    bool turnScreenOff = false;
    bool stayAwake = false;
    bool showTouches = false;
    int resIndex = 0;         // 0=Native, 1=1080p, 2=720p
    int bitrateIndex = 2;     // 0=4 Mbps, 1=8 Mbps, 2=16 Mbps
    int videoSourceIndex = 0; // 0=Display, 1=New Display (Desktop), 2=Simulate Secondary Displays, 3=Back Camera, 4=Front Camera
    int audioSourceIndex = 0; // 0=System Output, 1=Microphone
    int maxFpsIndex = 0;      // 0=Auto, 1=60 fps, 2=30 fps
    int lockOrientIndex = 0;  // 0=Auto, 1=Portrait, 2=Landscape
    std::wstring customRes;   // e.g. L"1080x2400"
    std::wstring customDpi;   // e.g. L"420"
};

inline bool IsCustomDisplayVideoSource(int index) { return index == 1 || index == 2; }
inline bool IsCameraVideoSource(int index) { return index == 3 || index == 4; }

void LoadAppSettings(AppSettings& settings);
void SaveAppSettings(const AppSettings& settings);

// Returns true if a device was connected/paired successfully.
bool ShowWirelessAdbDialog(HWND hwndParent, bool isDarkMode, const AdbClient& adb);

// Opens the Windows 11 Settings dialog.
bool ShowSettingsDialog(HWND hwndParent, bool isDarkMode, AppSettings& settings);
