#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Compact ISO/IEC 18004 QR Code encoder (Byte mode, ECC Level M, Versions 1-6).
struct QrCodeMatrix {
    int size = 0;                 // Module width/height (e.g. 33 for Version 4)
    std::vector<uint8_t> modules; // size * size, 1 = dark, 0 = light

    bool Get(int x, int y) const {
        if (x < 0 || y < 0 || x >= size || y >= size) return false;
        return modules[static_cast<size_t>(y) * size + x] != 0;
    }
};

bool EncodeQrCode(const std::string& utf8Text, QrCodeMatrix& outQr);
