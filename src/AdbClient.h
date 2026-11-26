#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

struct AdbDevice {
    std::wstring serial;
    std::wstring state;       // "device", "unauthorized", "offline", etc.
    std::wstring model;       // e.g. "Pixel 7"
    std::wstring product;
    std::wstring androidVer;
    int screenWidth = 0;
    int screenHeight = 0;
    bool isWireless = false;

    std::wstring DisplayName() const {
        if (!model.empty()) return model;
        return serial;
    }

    bool IsOnline() const {
        return state == L"device";
    }
};

struct AdbCommandResult {
    int exitCode = -1;
    std::string stdoutBytes;
    std::string stderrBytes;
    bool timedOut = false;
};

struct AdbMdnsService {
    std::wstring instanceName;
    std::wstring serviceType;
    std::wstring ipPort;
};

class AdbClient {
public:
    AdbClient();

    bool IsReady() const;
    const std::wstring& GetServerJarPath() const { return m_serverJarPath; }

    AdbCommandResult RunCommand(const std::wstring& args, DWORD timeoutMs = 8000) const;

    std::vector<AdbDevice> ListDevices() const;
    void PopulateDeviceDetails(AdbDevice& dev) const;

    bool ConnectWireless(const std::wstring& ipPort, std::wstring& outMessage) const;
    bool PairWireless(const std::wstring& ipPort, const std::wstring& code, std::wstring& outMessage) const;
    std::vector<AdbMdnsService> ListMdnsServices() const;

    bool ForwardTcp(const std::wstring& serial, int localPort, const std::wstring& remoteSocket) const;
    bool RemoveForward(const std::wstring& serial, int localPort) const;
    bool PushFile(const std::wstring& serial, const std::wstring& localPath, const std::wstring& remotePath) const;
    bool InstallApk(const std::wstring& serial, const std::wstring& apkPath, std::wstring& outMessage) const;

    bool TakeScreenshotPng(const std::wstring& serial, std::vector<uint8_t>& outPng) const;

    bool StartStreamingProcess(
        const std::wstring& args,
        HANDLE& outProcess,
        HANDLE& outStdoutRead
    ) const;

private:
    std::wstring m_adbPath;
    std::wstring m_serverJarPath;

    void ExtractBundledAdb();
    static bool ExtractResourceToFile(int resourceId, const std::wstring& outPath);
    static bool FileExists(const std::wstring& path);
};
