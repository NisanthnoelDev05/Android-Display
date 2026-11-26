#pragma once

#include "AdbClient.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

constexpr UINT WM_APP_FRAME_READY    = WM_APP + 101;
constexpr UINT WM_APP_MIRROR_STATUS  = WM_APP + 102;
constexpr UINT WM_APP_MIRROR_STOPPED = WM_APP + 103;
constexpr UINT WM_APP_CLIPBOARD_TEXT = WM_APP + 106;

struct MirrorConfig {
    std::wstring serial;
    int maxSize = 0;        // 0 = native, 1080, 720
    int bitrateMbps = 8;    // 4, 8, 16
    int maxFps = 0;         // 0 = auto, 60, 30
    int lockOrient = 0;     // 0 = auto, 1 = portrait, 2 = landscape
    int videoSource = 0;    // 0=Display, 1=New Display (Desktop), 2=Simulate Secondary Displays, 3=Back Camera, 4=Front Camera
    int audioSource = 0;    // 0 = system output, 1 = microphone
    int customW = 0;
    int customH = 0;
    int customDpi = 0;
    bool mirrorAudio = true;
    bool turnScreenOff = false;
    bool stayAwake = false;
    bool showTouches = false;
    int deviceWidth = 0;
    int deviceHeight = 0;
};

struct DecodedFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra; // Top-down 32-bit BGRA
};

struct StreamStats {
    int width = 0;
    int height = 0;
    int fps = 0;
    bool audioActive = false;
    std::wstring statusText;
};

class H264Decoder {
public:
    H264Decoder() = default;
    ~H264Decoder();

    bool Initialize();
    void Shutdown();
    bool DecodeFrame(const uint8_t* data, size_t size, uint64_t ptsUs, DecodedFrame& outFrame);

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

class PcmAudioPlayer {
public:
    PcmAudioPlayer() = default;
    ~PcmAudioPlayer();

    bool Initialize(int sampleRate = 48000, int channels = 2);
    void Shutdown();
    void SubmitPcm(const uint8_t* data, size_t size);

private:
    struct Impl;
    Impl* m_impl = nullptr;
    std::mutex m_mutex;
};

class MirrorSession {
public:
    MirrorSession() = default;
    ~MirrorSession();

    bool Start(HWND notifyHwnd, const AdbClient& adb, const MirrorConfig& cfg);
    void Stop();
    bool IsRunning() const { return m_running.load(); }

    std::shared_ptr<const DecodedFrame> GetLatestFrame() const;
    uint64_t GetFrameSeq() const { return m_frameSeq.load(std::memory_order_acquire); }
    void SetFrameEvent(HANDLE hEvent) { m_frameEvent = hEvent; }
    StreamStats GetStats() const;

    void SendTouch(int action, int x, int y, int screenW, int screenH); // action: 0=down, 1=up, 2=move
    void SendKey(int androidKeyCode);
    void SendScroll(int x, int y, int screenW, int screenH, int vScroll);
    void SendText(const std::wstring& text);
    void SendClipboard(const std::wstring& text, bool paste);
    void SetScreenPower(bool on);
    void ExpandNotifications(bool expand);
    void RotateDevice();
    void SetCameraZoom(float zoomRatio);
    void TriggerCameraFocus();

    bool StartRecording(const std::wstring& mp4Path);
    void StopRecording();
    bool IsRecording() const { return m_recording.load(); }

private:
    void RunStream();
    void UpdateFpsCounter();

    HWND m_notifyHwnd = nullptr;
    AdbClient m_adb;
    MirrorConfig m_cfg;

    std::atomic<bool> m_running{ false };
    std::thread m_workerThread;
    std::thread m_audioThread;
    std::thread m_ctrlRecvThread;

    HANDLE m_streamProcess = nullptr;
    HANDLE m_streamPipe = nullptr;
    uintptr_t m_ctrlSocket = ~uintptr_t(0);
    std::mutex m_ctrlMutex;

    mutable std::mutex m_frameMutex;
    std::shared_ptr<const DecodedFrame> m_latestFrame;
    std::atomic<uint64_t> m_frameSeq{ 0 };
    HANDLE m_frameEvent = nullptr;

    mutable std::mutex m_statsMutex;
    StreamStats m_stats;
    uint64_t m_fpsWindowStart = 0;
    int m_fpsFrameCount = 0;

    std::atomic<bool> m_recording{ false };
    std::mutex m_recMutex;
    struct RecImpl;
    RecImpl* m_rec = nullptr;
};
