#include "QrCode.h"
#include <algorithm>
#include <array>

namespace {

struct QrVersionSpec {
    int version;
    int size;
    int dataCodewords;
    int numBlocks;
    int ecPerBlock;
    int alignCenter; // 0 for V1, else single non-overlapping center coord (18,22,26,30,34)
};

// ECC Level M specs for Versions 1..6 (all blocks in each version have equal data count)
constexpr QrVersionSpec kSpecs[] = {
    { 1, 21,  16, 1, 10,  0 },
    { 2, 25,  28, 1, 16, 18 },
    { 3, 29,  44, 1, 26, 22 },
    { 4, 33,  64, 2, 18, 26 },
    { 5, 37,  86, 2, 24, 30 },
    { 6, 41, 108, 4, 16, 34 },
};

uint8_t GfMul(uint8_t x, uint8_t y) {
    uint8_t r = 0;
    for (int i = 0; i < 8; ++i) {
        if (y & 1) r ^= x;
        bool hi = (x & 0x80) != 0;
        x <<= 1;
        if (hi) x ^= 0x1D; // x^8 + x^4 + x^3 + x^2 + 1 (0x11D)
        y >>= 1;
    }
    return r;
}

std::vector<uint8_t> ComputeRsEc(const uint8_t* data, int dataLen, int ecLen) {
    // Build generator polynomial of degree ecLen
    std::vector<uint8_t> gen(ecLen, 0);
    gen[ecLen - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < ecLen; ++i) {
        for (int j = 0; j < ecLen; ++j) {
            gen[j] = GfMul(gen[j], root);
            if (j + 1 < ecLen) {
                gen[j] ^= gen[j + 1];
            }
        }
        root = GfMul(root, 2);
    }

    std::vector<uint8_t> rem(ecLen, 0);
    for (int i = 0; i < dataLen; ++i) {
        uint8_t factor = data[i] ^ rem[0];
        std::memmove(rem.data(), rem.data() + 1, static_cast<size_t>(ecLen - 1));
        rem[ecLen - 1] = 0;
        for (int j = 0; j < ecLen; ++j) {
            rem[j] ^= GfMul(gen[j], factor);
        }
    }
    return rem;
}

class QrBuilder {
public:
    explicit QrBuilder(const QrVersionSpec& spec)
        : m_spec(spec), m_sz(spec.size),
          m_modules(static_cast<size_t>(spec.size) * spec.size, 0),
          m_isFunc(static_cast<size_t>(spec.size) * spec.size, 0) {}

    void DrawFunctionPatterns() {
        // Timing patterns
        for (int i = 0; i < m_sz; ++i) {
            SetFunc(6, i, (i % 2) == 0);
            SetFunc(i, 6, (i % 2) == 0);
        }

        // Finder patterns + separators
        DrawFinder(3, 3);
        DrawFinder(m_sz - 4, 3);
        DrawFinder(3, m_sz - 4);

        // Alignment pattern (Versions 2..6 have one alignment pattern at (alignCenter, alignCenter))
        if (m_spec.alignCenter > 0) {
            DrawAlignment(m_spec.alignCenter, m_spec.alignCenter);
        }

        // Reserve format info areas (drawn after mask selection)
        for (int i = 0; i <= 8; ++i) {
            if (i != 6) {
                SetFunc(8, i, false);
                SetFunc(i, 8, false);
            }
            if (i < 8) {
                SetFunc(8, m_sz - 1 - i, false);
                SetFunc(m_sz - 1 - i, 8, false);
            }
        }
        // Dark module
        SetFunc(8, m_sz - 8, true);
    }

    void DrawCodewords(const std::vector<uint8_t>& cw) {
        size_t bitIdx = 0;
        size_t totalBits = cw.size() * 8;
        for (int right = m_sz - 1; right >= 1; right -= 2) {
            if (right == 6) right = 5;
            for (int vert = 0; vert < m_sz; ++vert) {
                for (int j = 0; j < 2; ++j) {
                    int x = right - j;
                    bool upward = (((right + 1) & 2) == 0);
                    int y = upward ? (m_sz - 1 - vert) : vert;
                    if (!IsFunc(x, y) && bitIdx < totalBits) {
                        bool bit = ((cw[bitIdx >> 3] >> (7 - (bitIdx & 7))) & 1) != 0;
                        SetMod(x, y, bit);
                        ++bitIdx;
                    }
                }
            }
        }
    }

    int SelectAndApplyBestMask() {
        int bestMask = 0;
        int bestPenalty = INT_MAX;
        std::vector<uint8_t> bestModules;

        for (int mask = 0; mask < 8; ++mask) {
            ApplyMask(mask);
            DrawFormatBits(mask);
            int p = ComputePenalty();
            if (p < bestPenalty) {
                bestPenalty = p;
                bestMask = mask;
                bestModules = m_modules;
            }
            ApplyMask(mask); // Unmask
        }
        m_modules = std::move(bestModules);
        return bestMask;
    }

    std::vector<uint8_t> TakeModules() { return std::move(m_modules); }

private:
    const QrVersionSpec& m_spec;
    int m_sz;
    std::vector<uint8_t> m_modules;
    std::vector<uint8_t> m_isFunc;

    bool IsFunc(int x, int y) const { return m_isFunc[static_cast<size_t>(y) * m_sz + x] != 0; }
    bool GetMod(int x, int y) const { return m_modules[static_cast<size_t>(y) * m_sz + x] != 0; }
    void SetMod(int x, int y, bool dark) { m_modules[static_cast<size_t>(y) * m_sz + x] = dark ? 1 : 0; }
    void SetFunc(int x, int y, bool dark) {
        size_t idx = static_cast<size_t>(y) * m_sz + x;
        m_modules[idx] = dark ? 1 : 0;
        m_isFunc[idx] = 1;
    }

    void DrawFinder(int cx, int cy) {
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dx = -4; dx <= 4; ++dx) {
                int xx = cx + dx, yy = cy + dy;
                if (xx >= 0 && xx < m_sz && yy >= 0 && yy < m_sz) {
                    int dist = std::max(std::abs(dx), std::abs(dy));
                    SetFunc(xx, yy, dist != 2 && dist != 4);
                }
            }
        }
    }

    void DrawAlignment(int cx, int cy) {
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                int dist = std::max(std::abs(dx), std::abs(dy));
                SetFunc(cx + dx, cy + dy, dist != 1);
            }
        }
    }

    void ApplyMask(int mask) {
        for (int y = 0; y < m_sz; ++y) {
            for (int x = 0; x < m_sz; ++x) {
                if (IsFunc(x, y)) continue;
                bool inv = false;
                switch (mask) {
                case 0: inv = ((x + y) % 2) == 0; break;
                case 1: inv = (y % 2) == 0; break;
                case 2: inv = (x % 3) == 0; break;
                case 3: inv = ((x + y) % 3) == 0; break;
                case 4: inv = ((x / 3 + y / 2) % 2) == 0; break;
                case 5: inv = ((x * y) % 2 + (x * y) % 3) == 0; break;
                case 6: inv = (((x * y) % 2 + (x * y) % 3) % 2) == 0; break;
                case 7: inv = (((x + y) % 2 + (x * y) % 3) % 2) == 0; break;
                }
                if (inv) SetMod(x, y, !GetMod(x, y));
            }
        }
    }

    void DrawFormatBits(int mask) {
        // ECC Level M = 0 (2-bit code 00)
        int data = (0 << 3) | mask;
        int rem = data;
        for (int i = 0; i < 10; ++i) {
            rem = (rem << 1) ^ (((rem >> 9) & 1) * 0x537);
        }
        int bits = ((data << 10) | rem) ^ 0x5412;

        for (int i = 0; i <= 5; ++i) SetFunc(8, i, ((bits >> i) & 1) != 0);
        SetFunc(8, 7, ((bits >> 6) & 1) != 0);
        SetFunc(8, 8, ((bits >> 7) & 1) != 0);
        SetFunc(7, 8, ((bits >> 8) & 1) != 0);
        for (int i = 9; i < 15; ++i) SetFunc(14 - i, 8, ((bits >> i) & 1) != 0);

        for (int i = 0; i < 8; ++i) SetFunc(m_sz - 1 - i, 8, ((bits >> i) & 1) != 0);
        for (int i = 8; i < 15; ++i) SetFunc(8, m_sz - 15 + i, ((bits >> i) & 1) != 0);
        SetFunc(8, m_sz - 8, true);
    }

    int ComputePenalty() const {
        int result = 0;
        // N1: 5+ consecutive same-color modules in row/col
        for (int y = 0; y < m_sz; ++y) {
            bool runColor = false;
            int runLen = 0;
            for (int x = 0; x < m_sz; ++x) {
                bool c = GetMod(x, y);
                if (x == 0 || c != runColor) {
                    if (runLen >= 5) result += 3 + (runLen - 5);
                    runColor = c;
                    runLen = 1;
                } else {
                    ++runLen;
                }
            }
            if (runLen >= 5) result += 3 + (runLen - 5);
        }
        for (int x = 0; x < m_sz; ++x) {
            bool runColor = false;
            int runLen = 0;
            for (int y = 0; y < m_sz; ++y) {
                bool c = GetMod(x, y);
                if (y == 0 || c != runColor) {
                    if (runLen >= 5) result += 3 + (runLen - 5);
                    runColor = c;
                    runLen = 1;
                } else {
                    ++runLen;
                }
            }
            if (runLen >= 5) result += 3 + (runLen - 5);
        }
        // N2: 2x2 blocks of same color
        for (int y = 0; y < m_sz - 1; ++y) {
            for (int x = 0; x < m_sz - 1; ++x) {
                bool c = GetMod(x, y);
                if (c == GetMod(x + 1, y) && c == GetMod(x, y + 1) && c == GetMod(x + 1, y + 1)) {
                    result += 3;
                }
            }
        }
        // N4: dark/light ratio
        int dark = 0;
        for (uint8_t m : m_modules) dark += (m ? 1 : 0);
        int total = m_sz * m_sz;
        int k = (std::abs(dark * 20 - total * 10) + total - 1) / total - 1;
        if (k > 0) result += k * 10;
        return result;
    }
};

} // namespace

bool EncodeQrCode(const std::string& utf8Text, QrCodeMatrix& outQr) {
    if (utf8Text.empty() || utf8Text.size() > 106) return false;

    // Pick smallest version (1..6) whose Byte-mode capacity fits utf8Text
    const QrVersionSpec* spec = nullptr;
    for (const auto& s : kSpecs) {
        int maxBytes = s.dataCodewords - 2; // 4-bit mode + 8-bit length + 4-bit terminator = 16 bits
        if (static_cast<int>(utf8Text.size()) <= maxBytes) {
            spec = &s;
            break;
        }
    }
    if (!spec) return false;

    // Build bit stream: Byte mode (0100), 8-bit length, payload bytes, terminator + pad bytes
    std::vector<uint8_t> dataCw;
    dataCw.reserve(static_cast<size_t>(spec->dataCodewords));

    int bitBuf = 0;
    int bitLen = 0;
    auto pushBits = [&](int val, int n) {
        bitBuf = (bitBuf << n) | (val & ((1 << n) - 1));
        bitLen += n;
        while (bitLen >= 8) {
            bitLen -= 8;
            dataCw.push_back(static_cast<uint8_t>((bitBuf >> bitLen) & 0xFF));
        }
    };

    pushBits(0b0100, 4);
    pushBits(static_cast<int>(utf8Text.size()), 8);
    for (unsigned char ch : utf8Text) {
        pushBits(ch, 8);
    }
    int remBits = spec->dataCodewords * 8 - (static_cast<int>(dataCw.size()) * 8 + bitLen);
    pushBits(0, std::min(4, remBits));
    if (bitLen > 0) {
        pushBits(0, 8 - bitLen);
    }
    for (uint8_t pad = 0xEC; static_cast<int>(dataCw.size()) < spec->dataCodewords; pad ^= 0xEC ^ 0x11) {
        dataCw.push_back(pad);
    }

    // Split into equal blocks, compute RS ECC, and interleave
    int blockDataLen = spec->dataCodewords / spec->numBlocks;
    std::vector<std::vector<uint8_t>> ecBlocks(static_cast<size_t>(spec->numBlocks));
    for (int b = 0; b < spec->numBlocks; ++b) {
        ecBlocks[b] = ComputeRsEc(dataCw.data() + b * blockDataLen, blockDataLen, spec->ecPerBlock);
    }

    std::vector<uint8_t> allCw;
    allCw.reserve(static_cast<size_t>(spec->dataCodewords + spec->numBlocks * spec->ecPerBlock));
    for (int i = 0; i < blockDataLen; ++i) {
        for (int b = 0; b < spec->numBlocks; ++b) {
            allCw.push_back(dataCw[static_cast<size_t>(b) * blockDataLen + i]);
        }
    }
    for (int i = 0; i < spec->ecPerBlock; ++i) {
        for (int b = 0; b < spec->numBlocks; ++b) {
            allCw.push_back(ecBlocks[b][i]);
        }
    }

    QrBuilder builder(*spec);
    builder.DrawFunctionPatterns();
    builder.DrawCodewords(allCw);
    builder.SelectAndApplyBestMask();

    outQr.size = spec->size;
    outQr.modules = builder.TakeModules();
    return true;
}
