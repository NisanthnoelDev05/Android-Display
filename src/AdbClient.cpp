#include "AdbClient.h"
#include "ResourceIds.h"
#include <shlobj.h>
#include <algorithm>
#include <sstream>

namespace {

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::wstring TrimWide(const std::wstring& s) {
    size_t start = s.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return {};
    size_t end = s.find_last_not_of(L" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string ReadAllFromPipe(HANDLE hPipe, HANDLE hProcess, DWORD timeoutMs, bool& timedOut) {
    std::string result;
    char buf[8192];
    DWORD startTick = GetTickCount();
    timedOut = false;

    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(hPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD bytesRead = 0;
            if (ReadFile(hPipe, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
                result.append(buf, bytesRead);
                continue;
            }
        }

        DWORD waitRes = WaitForSingleObject(hProcess, 15);
        if (waitRes == WAIT_OBJECT_0) {
            for (;;) {
                DWORD bytesRead = 0;
                if (!ReadFile(hPipe, buf, sizeof(buf), &bytesRead, nullptr) || bytesRead == 0) {
                    break;
                }
                result.append(buf, bytesRead);
            }
            break;
        }

        if (GetTickCount() - startTick > timeoutMs) {
            timedOut = true;
            TerminateProcess(hProcess, 1);
            break;
        }
    }
    return result;
}

} // namespace

AdbClient::AdbClient() {
    ExtractBundledAdb();
}

bool AdbClient::FileExists(const std::wstring& path) {
    if (path.empty()) return false;
    DWORD attrs = GetFileAttributesW(path.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES) && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool AdbClient::ExtractResourceToFile(int resourceId, const std::wstring& outPath) {
    HMODULE hMod = GetModuleHandleW(nullptr);
    HRSRC hRes = FindResourceW(hMod, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!hRes) return false;

    DWORD resSize = SizeofResource(hMod, hRes);
    HGLOBAL hData = LoadResource(hMod, hRes);
    if (!hData || resSize == 0) return false;

    const void* pBytes = LockResource(hData);
    if (!pBytes) return false;

    // Skip write if file already exists with exact matching size
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (GetFileAttributesExW(outPath.c_str(), GetFileExInfoStandard, &fad)) {
        if (fad.nFileSizeHigh == 0 && fad.nFileSizeLow == resSize) {
            return true;
        }
    }

    HANDLE hFile = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return FileExists(outPath);
    }

    DWORD written = 0;
    BOOL ok = WriteFile(hFile, pBytes, resSize, &written, nullptr);
    CloseHandle(hFile);
    return ok && (written == resSize);
}

void AdbClient::ExtractBundledAdb() {
    wchar_t baseDir[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, baseDir))) {
        GetTempPathW(MAX_PATH, baseDir);
    }

    std::wstring appDir = std::wstring(baseDir) + L"\\AndroidDisplay";
    std::wstring binDir = appDir + L"\\bin";
    CreateDirectoryW(appDir.c_str(), nullptr);
    CreateDirectoryW(binDir.c_str(), nullptr);

    ExtractResourceToFile(IDR_ADB_WIN_API, binDir + L"\\AdbWinApi.dll");
    ExtractResourceToFile(IDR_ADB_WIN_USB_API, binDir + L"\\AdbWinUsbApi.dll");

    std::wstring adbExe = binDir + L"\\adb.exe";
    if (ExtractResourceToFile(IDR_ADB_EXE, adbExe)) {
        m_adbPath = adbExe;
    }

    std::wstring serverJar = binDir + L"\\android-display-server.jar";
    if (ExtractResourceToFile(IDR_DISPLAY_SERVER_JAR, serverJar)) {
        m_serverJarPath = serverJar;
    }
}

bool AdbClient::IsReady() const {
    return FileExists(m_adbPath);
}

AdbCommandResult AdbClient::RunCommand(const std::wstring& args, DWORD timeoutMs) const {
    AdbCommandResult res;
    if (!IsReady()) {
        res.stderrBytes = "Bundled ADB unavailable";
        return res;
    }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE hOutRead = nullptr, hOutWrite = nullptr;
    if (!CreatePipe(&hOutRead, &hOutWrite, &sa, 0)) {
        return res;
    }
    SetHandleInformation(hOutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hOutWrite;
    si.hStdError = hOutWrite;
    si.hStdError = hOutWrite;

    PROCESS_INFORMATION pi = {};
    std::wstring cmdLine = L"\"" + m_adbPath + L"\" " + args;

    BOOL ok = CreateProcessW(
        nullptr,
        cmdLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    );

    CloseHandle(hOutWrite);

    if (!ok) {
        CloseHandle(hOutRead);
        return res;
    }

    res.stdoutBytes = ReadAllFromPipe(hOutRead, pi.hProcess, timeoutMs, res.timedOut);

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    res.exitCode = static_cast<int>(exitCode);

    CloseHandle(hOutRead);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return res;
}

std::vector<AdbDevice> AdbClient::ListDevices() const {
    std::vector<AdbDevice> devices;
    if (!IsReady()) return devices;

    AdbCommandResult r = RunCommand(L"devices -l", 6000);
    if (r.exitCode != 0 && r.stdoutBytes.empty()) {
        return devices;
    }

    std::wstring text = Utf8ToWide(r.stdoutBytes);
    std::wistringstream iss(text);
    std::wstring line;

    while (std::getline(iss, line)) {
        line = TrimWide(line);
        if (line.empty()) continue;
        if (line.rfind(L"List of devices", 0) == 0) continue;
        if (line.rfind(L"* ", 0) == 0) continue;

        std::wistringstream lss(line);
        AdbDevice dev;
        if (!(lss >> dev.serial >> dev.state)) {
            continue;
        }

        dev.isWireless = (dev.serial.find(L':') != std::wstring::npos) ||
                         (dev.serial.find(L"_adb-tls-connect._tcp") != std::wstring::npos) ||
                         (dev.serial.find(L"_adb._tcp") != std::wstring::npos);

        std::wstring token;
        while (lss >> token) {
            if (token.rfind(L"model:", 0) == 0) {
                dev.model = token.substr(6);
                std::replace(dev.model.begin(), dev.model.end(), L'_', L' ');
            } else if (token.rfind(L"product:", 0) == 0) {
                dev.product = token.substr(8);
            }
        }

        devices.push_back(std::move(dev));
    }

    // Deduplicate multiple wireless entries for the same device (e.g. mDNS service + ip:port)
    std::vector<AdbDevice> deduped;
    deduped.reserve(devices.size());
    for (auto& d : devices) {
        if (d.isWireless && !d.model.empty()) {
            auto it = std::find_if(deduped.begin(), deduped.end(), [&](const AdbDevice& existing) {
                return existing.isWireless && existing.model == d.model && existing.product == d.product;
            });
            if (it != deduped.end()) {
                bool dIsIp = (d.serial.find(L':') != std::wstring::npos);
                bool exIsIp = (it->serial.find(L':') != std::wstring::npos);
                if ((!it->IsOnline() && d.IsOnline()) || (d.IsOnline() == it->IsOnline() && dIsIp && !exIsIp)) {
                    *it = std::move(d);
                }
                continue;
            }
        }
        deduped.push_back(std::move(d));
    }

    return deduped;
}

void AdbClient::PopulateDeviceDetails(AdbDevice& dev) const {
    if (!dev.IsOnline()) return;

    AdbCommandResult rModel = RunCommand(L"-s " + dev.serial + L" shell getprop ro.product.model", 3000);
    if (rModel.exitCode == 0) {
        std::wstring m = TrimWide(Utf8ToWide(rModel.stdoutBytes));
        if (!m.empty()) dev.model = m;
    }

    AdbCommandResult rVer = RunCommand(L"-s " + dev.serial + L" shell getprop ro.build.version.release", 3000);
    if (rVer.exitCode == 0) {
        dev.androidVer = TrimWide(Utf8ToWide(rVer.stdoutBytes));
    }

    AdbCommandResult rSize = RunCommand(L"-s " + dev.serial + L" shell wm size", 3000);
    if (rSize.exitCode == 0) {
        std::wstring s = Utf8ToWide(rSize.stdoutBytes);
        size_t colon = s.rfind(L':');
        if (colon != std::wstring::npos) {
            std::wstring dims = TrimWide(s.substr(colon + 1));
            size_t xPos = dims.find(L'x');
            if (xPos != std::wstring::npos) {
                dev.screenWidth = _wtoi(dims.substr(0, xPos).c_str());
                dev.screenHeight = _wtoi(dims.substr(xPos + 1).c_str());
            }
        }
    }
}

bool AdbClient::ConnectWireless(const std::wstring& ipPort, std::wstring& outMessage) const {
    AdbCommandResult r = RunCommand(L"connect " + ipPort, 8000);
    outMessage = TrimWide(Utf8ToWide(r.stdoutBytes));
    if (outMessage.empty()) outMessage = L"Failed to execute adb connect.";
    return (r.exitCode == 0) &&
           (outMessage.find(L"connected to") != std::wstring::npos ||
            outMessage.find(L"already connected") != std::wstring::npos);
}

bool AdbClient::PairWireless(const std::wstring& ipPort, const std::wstring& code, std::wstring& outMessage) const {
    AdbCommandResult r = RunCommand(L"pair " + ipPort + L" " + code, 8000);
    outMessage = TrimWide(Utf8ToWide(r.stdoutBytes));
    if (outMessage.empty()) outMessage = L"Failed to execute adb pair.";
    return (r.exitCode == 0) && (outMessage.find(L"Successfully paired") != std::wstring::npos);
}

std::vector<AdbMdnsService> AdbClient::ListMdnsServices() const {
    std::vector<AdbMdnsService> services;
    AdbCommandResult r = RunCommand(L"mdns services", 3000);
    if (r.exitCode != 0) return services;

    std::wistringstream iss(Utf8ToWide(r.stdoutBytes));
    std::wstring line;
    while (std::getline(iss, line)) {
        line = TrimWide(line);
        if (line.empty()) continue;
        std::wistringstream lss(line);
        AdbMdnsService svc;
        if (lss >> svc.instanceName >> svc.serviceType >> svc.ipPort) {
            if (!svc.ipPort.empty() && svc.ipPort.find(L':') != std::wstring::npos) {
                services.push_back(std::move(svc));
            }
        }
    }
    return services;
}

bool AdbClient::ForwardTcp(const std::wstring& serial, int localPort, const std::wstring& remoteSocket) const {
    std::wstring args = L"-s " + serial + L" forward tcp:" + std::to_wstring(localPort) + L" " + remoteSocket;
    return RunCommand(args, 4000).exitCode == 0;
}

bool AdbClient::RemoveForward(const std::wstring& serial, int localPort) const {
    std::wstring args = L"-s " + serial + L" forward --remove tcp:" + std::to_wstring(localPort);
    return RunCommand(args, 3000).exitCode == 0;
}

bool AdbClient::PushFile(const std::wstring& serial, const std::wstring& localPath, const std::wstring& remotePath) const {
    std::wstring args = L"-s " + serial + L" push \"" + localPath + L"\" \"" + remotePath + L"\"";
    return RunCommand(args, 15000).exitCode == 0;
}

bool AdbClient::InstallApk(const std::wstring& serial, const std::wstring& apkPath, std::wstring& outMessage) const {
    std::wstring args = L"-s " + serial + L" install -r \"" + apkPath + L"\"";
    AdbCommandResult r = RunCommand(args, 60000);
    outMessage = TrimWide(Utf8ToWide(r.stdoutBytes));
    return (r.exitCode == 0) && (outMessage.find(L"Success") != std::wstring::npos);
}

bool AdbClient::TakeScreenshotPng(const std::wstring& serial, std::vector<uint8_t>& outPng) const {
    AdbCommandResult r = RunCommand(L"-s " + serial + L" exec-out screencap -p", 10000);
    if (r.exitCode == 0 && r.stdoutBytes.size() > 8) {
        outPng.assign(r.stdoutBytes.begin(), r.stdoutBytes.end());
        return true;
    }
    return false;
}

bool AdbClient::StartStreamingProcess(
    const std::wstring& args,
    HANDLE& outProcess,
    HANDLE& outStdoutRead
) const {
    outProcess = nullptr;
    outStdoutRead = nullptr;
    if (!IsReady()) return false;

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE hOutRead = nullptr, hOutWrite = nullptr;
    if (!CreatePipe(&hOutRead, &hOutWrite, &sa, 1024 * 256)) {
        return false;
    }
    SetHandleInformation(hOutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hOutWrite;
    si.hStdError = hOutWrite;

    PROCESS_INFORMATION pi = {};
    std::wstring cmdLine = L"\"" + m_adbPath + L"\" " + args;

    BOOL ok = CreateProcessW(
        nullptr,
        cmdLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    );

    CloseHandle(hOutWrite);

    if (!ok) {
        CloseHandle(hOutRead);
        return false;
    }

    CloseHandle(pi.hThread);
    outProcess = pi.hProcess;
    outStdoutRead = hOutRead;
    return true;
}
