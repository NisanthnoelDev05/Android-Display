#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "MediaPipeline.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <objbase.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <emmintrin.h>
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>

namespace {

uint32_t ReadBE32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void WriteBE16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

void WriteBE32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

bool RecvExact(SOCKET s, uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        int r = recv(s, reinterpret_cast<char*>(buf + got), static_cast<int>(len - got), 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

} // namespace

// ============================================================================
// H264Decoder (Windows Media Foundation CLSID_CMSH264DecoderMFT)
// ============================================================================
struct H264Decoder::Impl {
    IMFTransform* decoder = nullptr;
    int width = 0;
    int height = 0;
    int codedHeight = 0;
    LONG stride = 0;
    bool isNv12 = true;
    bool mfStarted = false;
    bool initialized = false;

    bool ConfigureOutputType() {
        for (DWORD idx = 0; ; ++idx) {
            IMFMediaType* outType = nullptr;
            HRESULT hr = decoder->GetOutputAvailableType(0, idx, &outType);
            if (FAILED(hr)) break;

            GUID subtype = GUID_NULL;
            outType->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_RGB32 || subtype == MFVideoFormat_ARGB32) {
                isNv12 = (subtype == MFVideoFormat_NV12);
                hr = decoder->SetOutputType(0, outType, 0);
                if (SUCCEEDED(hr)) {
                    UINT32 w = 0, h = 0;
                    if (SUCCEEDED(MFGetAttributeSize(outType, MF_MT_FRAME_SIZE, &w, &h))) {
                        width = static_cast<int>(w);
                        height = static_cast<int>(h);
                        codedHeight = height;
                    }
                    MFVideoArea area = {};
                    UINT32 blobSz = 0;
                    if (SUCCEEDED(outType->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), &blobSz)) &&
                        blobSz == sizeof(area) && area.Area.cx > 0 && area.Area.cy > 0) {
                        width = static_cast<int>(area.Area.cx);
                        height = static_cast<int>(area.Area.cy);
                    }
                    UINT32 st = 0;
                    if (SUCCEEDED(outType->GetUINT32(MF_MT_DEFAULT_STRIDE, &st))) {
                        stride = static_cast<LONG>(st);
                    } else {
                        stride = isNv12 ? width : (width * 4);
                    }
                    outType->Release();
                    return true;
                }
            }
            outType->Release();
        }
        return false;
    }
};

H264Decoder::~H264Decoder() {
    Shutdown();
}

bool H264Decoder::Initialize() {
    Shutdown();
    m_impl = new Impl();

    if (SUCCEEDED(MFStartup(MF_VERSION))) {
        m_impl->mfStarted = true;
    }

    HRESULT hr = CoCreateInstance(
        CLSID_CMSH264DecoderMFT,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&m_impl->decoder)
    );
    if (FAILED(hr) || !m_impl->decoder) {
        Shutdown();
        return false;
    }

    IMFAttributes* attrs = nullptr;
    if (SUCCEEDED(m_impl->decoder->GetAttributes(&attrs)) && attrs) {
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        attrs->Release();
    }

    ICodecAPI* codecApi = nullptr;
    if (SUCCEEDED(m_impl->decoder->QueryInterface(IID_PPV_ARGS(&codecApi))) && codecApi) {
        VARIANT varLowLat = {};
        varLowLat.vt = VT_UI4;
        varLowLat.ulVal = 1;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &varLowLat);
        codecApi->Release();
    }

    IMFMediaType* inType = nullptr;
    hr = MFCreateMediaType(&inType);
    if (FAILED(hr)) {
        Shutdown();
        return false;
    }

    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, 1280, 720);

    hr = m_impl->decoder->SetInputType(0, inType, 0);
    inType->Release();
    if (FAILED(hr)) {
        Shutdown();
        return false;
    }

    m_impl->ConfigureOutputType();

    m_impl->decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_impl->decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    m_impl->initialized = true;
    return true;
}

void H264Decoder::Shutdown() {
    if (!m_impl) return;
    if (m_impl->decoder) {
        m_impl->decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_impl->decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        SafeRelease(m_impl->decoder);
    }
    if (m_impl->mfStarted) {
        MFShutdown();
    }
    delete m_impl;
    m_impl = nullptr;
}

bool H264Decoder::DecodeFrame(const uint8_t* data, size_t size, uint64_t ptsUs, DecodedFrame& outFrame) {
    if (!m_impl || !m_impl->initialized || !data || size == 0) return false;

    IMFMediaBuffer* inBuf = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), &inBuf);
    if (FAILED(hr)) return false;

    BYTE* dst = nullptr;
    if (SUCCEEDED(inBuf->Lock(&dst, nullptr, nullptr))) {
        std::memcpy(dst, data, size);
        inBuf->Unlock();
        inBuf->SetCurrentLength(static_cast<DWORD>(size));
    }

    IMFSample* inSample = nullptr;
    hr = MFCreateSample(&inSample);
    if (FAILED(hr)) {
        inBuf->Release();
        return false;
    }
    inSample->AddBuffer(inBuf);
    inSample->SetSampleTime(static_cast<LONGLONG>(ptsUs * 10));
    inBuf->Release();

    hr = m_impl->decoder->ProcessInput(0, inSample, 0);
    if (hr == MF_E_NOTACCEPTING) {
        // Drain pending output first, then retry ProcessInput so no frame is dropped
        MFT_OUTPUT_STREAM_INFO osi = {};
        if (SUCCEEDED(m_impl->decoder->GetOutputStreamInfo(0, &osi))) {
            IMFSample* drainSample = nullptr;
            IMFMediaBuffer* drainBuf = nullptr;
            bool own = ((osi.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0);
            if (!own || (SUCCEEDED(MFCreateSample(&drainSample)) &&
                SUCCEEDED(MFCreateMemoryBuffer(osi.cbSize > 0 ? osi.cbSize : 1920 * 1080 * 4, &drainBuf)))) {
                if (own) { drainSample->AddBuffer(drainBuf); drainBuf->Release(); }
                MFT_OUTPUT_DATA_BUFFER odb = {};
                odb.pSample = drainSample;
                DWORD st = 0;
                m_impl->decoder->ProcessOutput(0, 1, &odb, &st);
                if (odb.pEvents) odb.pEvents->Release();
                if (odb.pSample) odb.pSample->Release();
            } else if (drainSample) {
                drainSample->Release();
            }
        }
        hr = m_impl->decoder->ProcessInput(0, inSample, 0);
    }
    inSample->Release();

    if (FAILED(hr) && hr != MF_E_NOTACCEPTING) {
        return false;
    }

    bool gotFrame = false;
    for (;;) {
        MFT_OUTPUT_STREAM_INFO osi = {};
        if (FAILED(m_impl->decoder->GetOutputStreamInfo(0, &osi))) break;

        IMFSample* outSample = nullptr;
        IMFMediaBuffer* outBuf = nullptr;
        bool weOwnSample = ((osi.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0);
        if (weOwnSample) {
            if (FAILED(MFCreateSample(&outSample))) break;
            DWORD bufSize = 1920 * 1080 * 4;
            if (osi.cbSize > 0) bufSize = osi.cbSize;
            else if (m_impl->width > 0 && m_impl->height > 0) bufSize = static_cast<DWORD>(m_impl->width * m_impl->height * 4);
            if (FAILED(MFCreateMemoryBuffer(bufSize, &outBuf))) {
                outSample->Release();
                break;
            }
            outSample->AddBuffer(outBuf);
            outBuf->Release();
        }

        MFT_OUTPUT_DATA_BUFFER odb = {};
        odb.pSample = outSample;
        DWORD status = 0;
        hr = m_impl->decoder->ProcessOutput(0, 1, &odb, &status);
        if (odb.pEvents) odb.pEvents->Release();

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (weOwnSample && outSample) outSample->Release();
            m_impl->ConfigureOutputType();
            continue;
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT || FAILED(hr)) {
            if (weOwnSample && outSample) outSample->Release();
            break;
        }

        IMFMediaBuffer* buf = nullptr;
        if (odb.pSample && SUCCEEDED(odb.pSample->ConvertToContiguousBuffer(&buf))) {
            BYTE* src = nullptr;
            DWORD curLen = 0;
            if (SUCCEEDED(buf->Lock(&src, nullptr, &curLen)) && m_impl->width > 0 && m_impl->height > 0) {
                int w = m_impl->width;
                int h = m_impl->height;
                LONG st = m_impl->stride;
                if (st == 0) st = m_impl->isNv12 ? w : (w * 4);
                LONG absSt = std::abs(st);

                if (m_impl->isNv12) {
                    int uvOffsetH = (m_impl->codedHeight > 0) ? m_impl->codedHeight : h;
                    DWORD neededNv12 = static_cast<DWORD>(absSt) * static_cast<DWORD>(uvOffsetH + h / 2);
                    if (curLen >= neededNv12) {
                        outFrame.width = w;
                        outFrame.height = h;
                        outFrame.bgra.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);

                        const BYTE* yPlane = src;
                        const BYTE* uvPlane = src + static_cast<size_t>(absSt) * uvOffsetH;

                        auto clampU8 = [](int v) -> uint8_t {
                            return static_cast<uint8_t>((v & ~255) ? ((~v >> 31) & 255) : v);
                        };

                        const __m128i kZero = _mm_setzero_si128();
                        const __m128i k16 = _mm_set1_epi16(16);
                        const __m128i k128 = _mm_set1_epi16(128);
                        const __m128i kYMul = _mm_set1_epi16(37);
                        const __m128i kVR = _mm_set1_epi16(51);
                        const __m128i kUG = _mm_set1_epi16(-13);
                        const __m128i kVG = _mm_set1_epi16(-26);
                        const __m128i kUB = _mm_set1_epi16(64);
                        const __m128i kRound = _mm_set1_epi16(16);
                        const __m128i kAlpha = _mm_set1_epi8(static_cast<char>(0xFF));
                        const __m128i kMaskLo = _mm_set1_epi16(0x00FF);

                        for (int row = 0; row < h - 1; row += 2) {
                            const BYTE* yRow0 = yPlane + static_cast<size_t>(row) * absSt;
                            const BYTE* yRow1 = yRow0 + absSt;
                            const BYTE* uvRow = uvPlane + static_cast<size_t>(row >> 1) * absSt;
                            uint8_t* dst0 = outFrame.bgra.data() + static_cast<size_t>(row) * w * 4;
                            uint8_t* dst1 = dst0 + static_cast<size_t>(w) * 4;

                            int col = 0;
                            for (; col + 7 < w; col += 8) {
                                // Load 8 UV bytes (4 U,V pairs)
                                __m128i uvRaw = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(uvRow + col));
                                __m128i u4 = _mm_sub_epi16(_mm_and_si128(uvRaw, kMaskLo), k128);
                                __m128i v4 = _mm_sub_epi16(_mm_srli_epi16(uvRaw, 8), k128);
                                // Duplicate each U,V for 2x2 pixel blocks -> 8 lanes
                                __m128i u8 = _mm_unpacklo_epi16(u4, u4);
                                __m128i v8 = _mm_unpacklo_epi16(v4, v4);

                                __m128i cR = _mm_add_epi16(_mm_mullo_epi16(v8, kVR), kRound);
                                __m128i cG = _mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(u8, kUG), _mm_mullo_epi16(v8, kVG)), kRound);
                                __m128i cB = _mm_add_epi16(_mm_mullo_epi16(u8, kUB), kRound);

                                auto storeRow8 = [&](const BYTE* ySrc, uint8_t* dst) {
                                    __m128i yRaw = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(ySrc));
                                    __m128i y16 = _mm_sub_epi16(_mm_unpacklo_epi8(yRaw, kZero), k16);
                                    __m128i yScaled = _mm_mullo_epi16(y16, kYMul);

                                    __m128i r8 = _mm_packus_epi16(_mm_srai_epi16(_mm_add_epi16(yScaled, cR), 5), kZero);
                                    __m128i g8 = _mm_packus_epi16(_mm_srai_epi16(_mm_add_epi16(yScaled, cG), 5), kZero);
                                    __m128i b8 = _mm_packus_epi16(_mm_srai_epi16(_mm_add_epi16(yScaled, cB), 5), kZero);

                                    __m128i bg = _mm_unpacklo_epi8(b8, g8);
                                    __m128i ra = _mm_unpacklo_epi8(r8, kAlpha);
                                    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), _mm_unpacklo_epi16(bg, ra));
                                    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 16), _mm_unpackhi_epi16(bg, ra));
                                };

                                storeRow8(yRow0 + col, dst0 + col * 4);
                                storeRow8(yRow1 + col, dst1 + col * 4);
                            }

                            for (; col < w - 1; col += 2) {
                                int uVal = static_cast<int>(uvRow[col]) - 128;
                                int vVal = static_cast<int>(uvRow[col + 1]) - 128;
                                int cR = 409 * vVal + 128;
                                int cG = -100 * uVal - 208 * vVal + 128;
                                int cB = 516 * uVal + 128;

                                int y00 = 298 * (static_cast<int>(yRow0[col]) - 16);
                                int y01 = 298 * (static_cast<int>(yRow0[col + 1]) - 16);
                                int y10 = 298 * (static_cast<int>(yRow1[col]) - 16);
                                int y11 = 298 * (static_cast<int>(yRow1[col + 1]) - 16);

                                int o0 = col * 4;
                                dst0[o0 + 0] = clampU8((y00 + cB) >> 8);
                                dst0[o0 + 1] = clampU8((y00 + cG) >> 8);
                                dst0[o0 + 2] = clampU8((y00 + cR) >> 8);
                                dst0[o0 + 3] = 255;

                                dst0[o0 + 4] = clampU8((y01 + cB) >> 8);
                                dst0[o0 + 5] = clampU8((y01 + cG) >> 8);
                                dst0[o0 + 6] = clampU8((y01 + cR) >> 8);
                                dst0[o0 + 7] = 255;

                                dst1[o0 + 0] = clampU8((y10 + cB) >> 8);
                                dst1[o0 + 1] = clampU8((y10 + cG) >> 8);
                                dst1[o0 + 2] = clampU8((y10 + cR) >> 8);
                                dst1[o0 + 3] = 255;

                                dst1[o0 + 4] = clampU8((y11 + cB) >> 8);
                                dst1[o0 + 5] = clampU8((y11 + cG) >> 8);
                                dst1[o0 + 6] = clampU8((y11 + cR) >> 8);
                                dst1[o0 + 7] = 255;
                            }
                        }
                        gotFrame = true;
                    }
                } else {
                    size_t needed = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
                    if (curLen >= static_cast<DWORD>(absSt * h)) {
                        outFrame.width = w;
                        outFrame.height = h;
                        outFrame.bgra.resize(needed);

                        if (st > 0) {
                            for (int row = 0; row < h; ++row) {
                                std::memcpy(
                                    outFrame.bgra.data() + static_cast<size_t>(row) * w * 4,
                                    src + static_cast<size_t>(row) * st,
                                    static_cast<size_t>(w) * 4
                                );
                            }
                        } else {
                            for (int row = 0; row < h; ++row) {
                                std::memcpy(
                                    outFrame.bgra.data() + static_cast<size_t>(row) * w * 4,
                                    src + static_cast<size_t>(h - 1 - row) * absSt,
                                    static_cast<size_t>(w) * 4
                                );
                            }
                        }
                        gotFrame = true;
                    }
                }
                buf->Unlock();
            }
            buf->Release();
        }
        if (odb.pSample) odb.pSample->Release();
    }

    return gotFrame;
}

// ============================================================================
// PcmAudioPlayer (WASAPI Shared-Mode 48kHz 16-bit Stereo PCM)
// ============================================================================
struct PcmAudioPlayer::Impl {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    UINT32 bufferFrames = 0;
    int blockAlign = 4;
    bool started = false;
    bool fadeNext = false;
};

PcmAudioPlayer::~PcmAudioPlayer() {
    Shutdown();
}

bool PcmAudioPlayer::Initialize(int sampleRate, int channels) {
    Shutdown();
    std::lock_guard<std::mutex> lock(m_mutex);

    m_impl = new Impl();
    m_impl->blockAlign = channels * 2;

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&m_impl->enumerator))) || !m_impl->enumerator) {
        delete m_impl;
        m_impl = nullptr;
        return false;
    }

    if (FAILED(m_impl->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_impl->device)) || !m_impl->device) {
        m_impl->enumerator->Release();
        delete m_impl;
        m_impl = nullptr;
        return false;
    }

    if (FAILED(m_impl->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(&m_impl->client))) || !m_impl->client) {
        m_impl->device->Release();
        m_impl->enumerator->Release();
        delete m_impl;
        m_impl = nullptr;
        return false;
    }

    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = static_cast<WORD>(channels);
    wfx.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = static_cast<WORD>(m_impl->blockAlign);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    // 200ms ring buffer with automatic sample rate & channel conversion for any Windows audio device
    DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    HRESULT hr = m_impl->client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, &wfx, nullptr);
    if (FAILED(hr)) {
        m_impl->client->Release();
        m_impl->device->Release();
        m_impl->enumerator->Release();
        delete m_impl;
        m_impl = nullptr;
        return false;
    }

    m_impl->client->GetBufferSize(&m_impl->bufferFrames);
    if (FAILED(m_impl->client->GetService(IID_PPV_ARGS(&m_impl->render))) || !m_impl->render) {
        m_impl->client->Release();
        m_impl->device->Release();
        m_impl->enumerator->Release();
        delete m_impl;
        m_impl = nullptr;
        return false;
    }

    if (SUCCEEDED(m_impl->client->Start())) {
        m_impl->started = true;
    }
    return m_impl->started;
}

void PcmAudioPlayer::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_impl) return;
    if (m_impl->client && m_impl->started) {
        m_impl->client->Stop();
    }
    SafeRelease(m_impl->render);
    SafeRelease(m_impl->client);
    SafeRelease(m_impl->device);
    SafeRelease(m_impl->enumerator);
    delete m_impl;
    m_impl = nullptr;
}

void PcmAudioPlayer::SubmitPcm(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_impl || !m_impl->started || !m_impl->render || !data || size < static_cast<size_t>(m_impl->blockAlign)) return;

    UINT32 frames = static_cast<UINT32>(size / static_cast<size_t>(m_impl->blockAlign));
    UINT32 padding = 0;
    if (FAILED(m_impl->client->GetCurrentPadding(&padding))) return;

    UINT32 avail = (m_impl->bufferFrames > padding) ? (m_impl->bufferFrames - padding) : 0;
    if (avail < frames) {
        m_impl->fadeNext = true;
        return;
    }

    BYTE* dst = nullptr;
    if (SUCCEEDED(m_impl->render->GetBuffer(frames, &dst)) && dst) {
        std::memcpy(dst, data, static_cast<size_t>(frames) * static_cast<size_t>(m_impl->blockAlign));
        if (m_impl->fadeNext) {
            m_impl->fadeNext = false;
            auto* samples = reinterpret_cast<int16_t*>(dst);
            UINT32 ramp = std::min<UINT32>(frames, 144);
            for (UINT32 f = 0; f < ramp; ++f) {
                float gain = static_cast<float>(f) / static_cast<float>(ramp);
                samples[f * 2 + 0] = static_cast<int16_t>(samples[f * 2 + 0] * gain);
                samples[f * 2 + 1] = static_cast<int16_t>(samples[f * 2 + 1] * gain);
            }
        }
        m_impl->render->ReleaseBuffer(frames, 0);
    }
}

struct MirrorSession::RecImpl {
    IMFSinkWriter* writer = nullptr;
    DWORD streamIdx = static_cast<DWORD>(-1);
    DWORD audioStreamIdx = static_cast<DWORD>(-1);
    uint64_t startTick = 0;
    LONGLONG audioSampleTime = -1;
    int width = 0;
    int height = 0;
    bool mfStarted = false;
};

// ============================================================================
// MirrorSession (AndroidDisplayServer over ADB Sockets)
// ============================================================================
MirrorSession::~MirrorSession() {
    Stop();
}

bool MirrorSession::Start(HWND notifyHwnd, const AdbClient& adb, const MirrorConfig& cfg) {
    Stop();
    m_notifyHwnd = notifyHwnd;
    m_adb = adb;
    m_cfg = cfg;
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_latestFrame.reset();
    }
    m_fpsWindowStart = GetTickCount64();
    m_fpsFrameCount = 0;

    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats = {};
        m_stats.statusText = L"Connecting...";
    }

    m_running.store(true);
    m_workerThread = std::thread(&MirrorSession::RunStream, this);
    return true;
}

void MirrorSession::Stop() {
    StopRecording();
    m_running.store(false);

    if (m_ctrlSocket != ~uintptr_t(0)) {
        closesocket(static_cast<SOCKET>(m_ctrlSocket));
        m_ctrlSocket = ~uintptr_t(0);
    }
    if (m_streamProcess) {
        if (WaitForSingleObject(m_streamProcess, 800) == WAIT_TIMEOUT) {
            TerminateProcess(m_streamProcess, 0);
        }
        CloseHandle(m_streamProcess);
        m_streamProcess = nullptr;
    }
    if (m_streamPipe) {
        CloseHandle(m_streamPipe);
        m_streamPipe = nullptr;
    }
    if (m_ctrlRecvThread.joinable()) {
        m_ctrlRecvThread.join();
    }
    if (m_audioThread.joinable()) {
        m_audioThread.join();
    }
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

std::shared_ptr<const DecodedFrame> MirrorSession::GetLatestFrame() const {
    std::lock_guard<std::mutex> lock(m_frameMutex);
    return m_latestFrame;
}

StreamStats MirrorSession::GetStats() const {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats;
}

void MirrorSession::UpdateFpsCounter() {
    m_fpsFrameCount++;
    uint64_t now = GetTickCount64();
    uint64_t elapsed = now - m_fpsWindowStart;
    if (elapsed >= 1000) {
        int fps = static_cast<int>((m_fpsFrameCount * 1000ULL) / elapsed);
        m_fpsFrameCount = 0;
        m_fpsWindowStart = now;
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.fps = fps;
    }
}

void MirrorSession::RunStream() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    timeBeginPeriod(1);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    WSADATA wsa = {};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    const int videoPort = 27190;
    const int audioPort = 27191;
    const int ctrlPort  = 27192;

    auto cleanupSession = [&]() {
        m_adb.RemoveForward(m_cfg.serial, videoPort);
        m_adb.RemoveForward(m_cfg.serial, audioPort);
        m_adb.RemoveForward(m_cfg.serial, ctrlPort);
        m_running.store(false);
        PostMessageW(m_notifyHwnd, WM_APP_MIRROR_STOPPED, 0, 0);
        WSACleanup();
        CoUninitialize();
        timeEndPeriod(1);
    };

    if (!m_adb.PushFile(m_cfg.serial, m_adb.GetServerJarPath(), L"/data/local/tmp/android-display-server.jar")) {
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.statusText = L"Failed to push server to device";
        }
        m_running.store(false);
        PostMessageW(m_notifyHwnd, WM_APP_MIRROR_STOPPED, 0, 0);
        WSACleanup();
        CoUninitialize();
        return;
    }

    m_adb.ForwardTcp(m_cfg.serial, videoPort, L"localabstract:adisp_video");
    if (m_cfg.mirrorAudio) {
        m_adb.ForwardTcp(m_cfg.serial, audioPort, L"localabstract:adisp_audio");
    }
    m_adb.ForwardTcp(m_cfg.serial, ctrlPort, L"localabstract:adisp_ctrl");

    const wchar_t* vSrcStr = L"display";
    if (m_cfg.videoSource == 1) vSrcStr = L"new_display";
    else if (m_cfg.videoSource == 2) vSrcStr = L"sim_display";
    else if (m_cfg.videoSource == 3) vSrcStr = L"back_cam";
    else if (m_cfg.videoSource == 4) vSrcStr = L"front_cam";
    const bool isCameraMode = (m_cfg.videoSource == 3 || m_cfg.videoSource == 4);
    const bool isCustomDisplay = (m_cfg.videoSource == 1 || m_cfg.videoSource == 2);
    const wchar_t* aSrcStr = (isCameraMode || m_cfg.audioSource == 1) ? L"mic" : L"output";

    int passW = isCustomDisplay ? m_cfg.customW : 0;
    int passH = isCustomDisplay ? m_cfg.customH : 0;
    int passDpi = isCustomDisplay ? m_cfg.customDpi : 0;

    const bool isWireless = (m_cfg.serial.find(L':') != std::wstring::npos) ||
                            (m_cfg.serial.find(L"_adb-tls-connect._tcp") != std::wstring::npos) ||
                            (m_cfg.serial.find(L"_adb._tcp") != std::wstring::npos);
    const int effectiveMbps = isWireless ? std::min(m_cfg.bitrateMbps, 12) : m_cfg.bitrateMbps;

    std::wstring serverArgs = L"-s " + m_cfg.serial +
        L" shell CLASSPATH=/data/local/tmp/android-display-server.jar app_process / com.androiddisplay.Server" +
        L" maxSize=" + std::to_wstring(m_cfg.maxSize) +
        L" bitrate=" + std::to_wstring(effectiveMbps * 1000000) +
        L" maxFps=" + std::to_wstring(m_cfg.maxFps) +
        L" lockOrient=" + std::to_wstring(m_cfg.lockOrient) +
        L" customW=" + std::to_wstring(passW) +
        L" customH=" + std::to_wstring(passH) +
        L" customDpi=" + std::to_wstring(passDpi) +
        L" videoSource=" + vSrcStr +
        L" audioSource=" + aSrcStr +
        L" audio=" + (m_cfg.mirrorAudio ? L"true" : L"false") +
        L" screenOff=" + (m_cfg.turnScreenOff ? L"true" : L"false") +
        L" stayAwake=" + (m_cfg.stayAwake ? L"true" : L"false") +
        L" showTouches=" + (m_cfg.showTouches ? L"true" : L"false");

    if (!m_adb.StartStreamingProcess(serverArgs, m_streamProcess, m_streamPipe)) {
        cleanupSession();
        return;
    }

    // Wait for server to print "READY" before connecting forwarded TCP sockets
    {
        std::string readyBuf;
        char chBuf[256];
        uint64_t waitStart = GetTickCount64();
        bool isReady = false;
        while (m_running.load() && (GetTickCount64() - waitStart < 5000)) {
            DWORD avail = 0;
            if (PeekNamedPipe(m_streamPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                DWORD got = 0;
                if (ReadFile(m_streamPipe, chBuf, sizeof(chBuf), &got, nullptr) && got > 0) {
                    readyBuf.append(chBuf, got);
                    if (readyBuf.find("READY") != std::string::npos) {
                        isReady = true;
                        break;
                    }
                }
            } else if (WaitForSingleObject(m_streamProcess, 15) == WAIT_OBJECT_0) {
                break;
            }
        }
        if (!isReady) {
            std::wstring errLine = L"Server failed to start";
            if (!readyBuf.empty()) {
                int wlen = MultiByteToWideChar(CP_UTF8, 0, readyBuf.data(), static_cast<int>(readyBuf.size()), nullptr, 0);
                if (wlen > 0) {
                    std::wstring werr(wlen, L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, readyBuf.data(), static_cast<int>(readyBuf.size()), werr.data(), wlen);
                    size_t nl = werr.find_first_of(L"\r\n");
                    if (nl != std::wstring::npos) werr.resize(nl);
                    if (!werr.empty()) errLine = werr;
                }
            }
            {
                std::lock_guard<std::mutex> lock(m_statsMutex);
                m_stats.statusText = errLine;
            }
            cleanupSession();
            return;
        }
    }

    auto connectSocket = [&](int port, bool waitForData, int rcvBufBytes) -> SOCKET {
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        for (int attempt = 0; attempt < 40 && m_running.load(); ++attempt) {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s != INVALID_SOCKET) {
                BOOL noDelay = TRUE;
                setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
                if (rcvBufBytes > 0) {
                    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBufBytes), sizeof(rcvBufBytes));
                }
                if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                    if (!waitForData) return s;
                    uint8_t dummy = 0;
                    if (recv(s, reinterpret_cast<char*>(&dummy), 1, MSG_PEEK) > 0) {
                        return s;
                    }
                }
                closesocket(s);
            }
            Sleep(80);
        }
        return INVALID_SOCKET;
    };

    SOCKET videoSock = connectSocket(videoPort, false, 256 * 1024);
    SOCKET audioSock = (m_cfg.mirrorAudio && videoSock != INVALID_SOCKET) ? connectSocket(audioPort, false, 32 * 1024) : INVALID_SOCKET;
    SOCKET ctrlSock  = (videoSock != INVALID_SOCKET) ? connectSocket(ctrlPort, false, 0) : INVALID_SOCKET;

    if (videoSock == INVALID_SOCKET) {
        cleanupSession();
        return;
    }

    m_ctrlSocket = static_cast<uintptr_t>(ctrlSock);

    // Read initial video dimensions (8 bytes)
    uint8_t dimHdr[8];
    if (!RecvExact(videoSock, dimHdr, 8)) {
        // Read any error printed by Java server on m_streamPipe
        std::wstring errLine = L"Video stream closed by device";
        Sleep(50);
        DWORD avail = 0;
        if (m_streamPipe && PeekNamedPipe(m_streamPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char buf[512] = {};
            DWORD got = 0;
            if (ReadFile(m_streamPipe, buf, sizeof(buf) - 1, &got, nullptr) && got > 0) {
                int wlen = MultiByteToWideChar(CP_UTF8, 0, buf, static_cast<int>(got), nullptr, 0);
                if (wlen > 0) {
                    std::wstring werr(wlen, L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, buf, static_cast<int>(got), werr.data(), wlen);
                    size_t nl = werr.find_first_of(L"\r\n");
                    if (nl != std::wstring::npos) werr.resize(nl);
                    if (!werr.empty()) errLine = werr;
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.statusText = errLine;
        }
        closesocket(videoSock);
        if (audioSock != INVALID_SOCKET) closesocket(audioSock);
        cleanupSession();
        return;
    }

    int initW = static_cast<int>(ReadBE32(dimHdr));
    int initH = static_cast<int>(ReadBE32(dimHdr + 4));

    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.width = initW;
        m_stats.height = initH;
        m_stats.audioActive = (audioSock != INVALID_SOCKET);
        m_stats.statusText = L"Mirroring";
    }
    PostMessageW(m_notifyHwnd, WM_APP_MIRROR_STATUS, 0, 0);

    // Control receiver thread (Phone -> PC clipboard sync)
    if (ctrlSock != INVALID_SOCKET) {
        m_ctrlRecvThread = std::thread([this, ctrlSock]() {
            uint8_t type = 0;
            uint8_t lenBuf[2];
            std::string utf8;
            while (m_running.load() && RecvExact(ctrlSock, &type, 1)) {
                if (type == 5) {
                    if (!RecvExact(ctrlSock, lenBuf, 2)) break;
                    uint16_t sz = (uint16_t(lenBuf[0]) << 8) | uint16_t(lenBuf[1]);
                    if (sz > 0 && sz <= 60000) {
                        utf8.resize(sz);
                        if (!RecvExact(ctrlSock, reinterpret_cast<uint8_t*>(utf8.data()), sz)) break;
                        int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), sz, nullptr, 0);
                        if (wlen > 0) {
                            auto* wtext = new std::wstring(wlen, L'\0');
                            MultiByteToWideChar(CP_UTF8, 0, utf8.data(), sz, wtext->data(), wlen);
                            if (!PostMessageW(m_notifyHwnd, WM_APP_CLIPBOARD_TEXT, 0, reinterpret_cast<LPARAM>(wtext))) {
                                delete wtext;
                            }
                        }
                    }
                }
            }
        });
    }

    // Audio thread (48kHz 16-bit stereo PCM)
    if (audioSock != INVALID_SOCKET) {
        m_audioThread = std::thread([this, audioSock, isCameraMode]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            PcmAudioPlayer player;
            if (!isCameraMode) {
                player.Initialize(48000, 2);
            }
            std::vector<uint8_t> pcmBuf;
            uint8_t lenBuf[4];
            bool anyAudio = false;
            while (m_running.load() && RecvExact(audioSock, lenBuf, 4)) {
                uint32_t sz = ReadBE32(lenBuf);
                if (sz == 0 || sz > 262144) break;
                pcmBuf.resize(sz);
                if (!RecvExact(audioSock, pcmBuf.data(), sz)) break;
                if (!isCameraMode) {
                    player.SubmitPcm(pcmBuf.data(), sz);
                }
                if (m_recording.load() && m_rec && sz >= 4) {
                    std::lock_guard<std::mutex> rlock(m_recMutex);
                    if (m_rec && m_rec->writer && m_rec->audioStreamIdx != static_cast<DWORD>(-1)) {
                        IMFMediaBuffer* aBuf = nullptr;
                        if (SUCCEEDED(MFCreateMemoryBuffer(sz, &aBuf))) {
                            BYTE* dst = nullptr;
                            if (SUCCEEDED(aBuf->Lock(&dst, nullptr, nullptr))) {
                                std::memcpy(dst, pcmBuf.data(), sz);
                                aBuf->Unlock();
                                aBuf->SetCurrentLength(sz);
                                IMFSample* aSample = nullptr;
                                if (SUCCEEDED(MFCreateSample(&aSample))) {
                                    aSample->AddBuffer(aBuf);
                                    if (m_rec->audioSampleTime < 0) {
                                        m_rec->audioSampleTime = static_cast<LONGLONG>(GetTickCount64() - m_rec->startTick) * 10000LL;
                                    }
                                    LONGLONG dur = (static_cast<LONGLONG>(sz / 4) * 10000000LL) / 48000LL;
                                    aSample->SetSampleTime(m_rec->audioSampleTime);
                                    aSample->SetSampleDuration(dur);
                                    m_rec->writer->WriteSample(m_rec->audioStreamIdx, aSample);
                                    m_rec->audioSampleTime += dur;
                                    aSample->Release();
                                }
                            }
                            aBuf->Release();
                        }
                    }
                }
                if (!anyAudio) {
                    anyAudio = true;
                    std::lock_guard<std::mutex> lock(m_statsMutex);
                    m_stats.audioActive = true;
                }
            }
            if (!anyAudio) {
                std::lock_guard<std::mutex> lock(m_statsMutex);
                m_stats.audioActive = false;
            }
            closesocket(audioSock);
            player.Shutdown();
            if (SUCCEEDED(hrCom)) CoUninitialize();
        });
    }

    H264Decoder decoder;
    decoder.Initialize();

    std::shared_ptr<DecodedFrame> framePool[3] = {
        std::make_shared<DecodedFrame>(),
        std::make_shared<DecodedFrame>(),
        std::make_shared<DecodedFrame>()
    };
    int poolIdx = 0;

    auto acquireFrame = [&]() -> std::shared_ptr<DecodedFrame> {
        for (int i = 0; i < 3; ++i) {
            int idx = (poolIdx + i) % 3;
            if (framePool[idx].use_count() == 1) {
                poolIdx = (idx + 1) % 3;
                return framePool[idx];
            }
        }
        return std::make_shared<DecodedFrame>();
    };

    auto publishFrame = [&](const std::shared_ptr<DecodedFrame>& sp) {
        int prevW = 0, prevH = 0;
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            m_latestFrame = sp;
        }
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            prevW = m_stats.width;
            prevH = m_stats.height;
            m_stats.width = sp->width;
            m_stats.height = sp->height;
        }
        m_frameSeq.fetch_add(1, std::memory_order_release);
        if (m_frameEvent) {
            SetEvent(m_frameEvent);
        }
        UpdateFpsCounter();
        static uint64_t lastUiNotify = 0;
        uint64_t now = GetTickCount64();
        if (sp->width != prevW || sp->height != prevH || (now - lastUiNotify) >= 250) {
            lastUiNotify = now;
            PostMessageW(m_notifyHwnd, WM_APP_FRAME_READY, 0, 0);
        }
    };

    std::mutex pktQueueMutex;
    std::condition_variable pktQueueCv;
    std::deque<std::vector<uint8_t>> pktQueue;
    bool recvDone = false;

    std::thread videoRecvThread([&]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        uint8_t lenBuf[4];
        while (m_running.load() && RecvExact(videoSock, lenBuf, 4)) {
            uint32_t sz = ReadBE32(lenBuf);
            if (sz == 0 || sz > 16 * 1024 * 1024) break;
            std::vector<uint8_t> buf(sz);
            if (!RecvExact(videoSock, buf.data(), sz)) break;
            {
                std::lock_guard<std::mutex> qlock(pktQueueMutex);
                pktQueue.push_back(std::move(buf));
            }
            pktQueueCv.notify_one();
        }
        {
            std::lock_guard<std::mutex> qlock(pktQueueMutex);
            recvDone = true;
        }
        pktQueueCv.notify_all();
    });

    std::vector<uint8_t> pktBuf;
    std::vector<uint8_t> spsPpsBuf;
    std::vector<uint8_t> combinedBuf;
    uint64_t ptsUs = 0;
    const uint64_t frameStepUs = (m_cfg.maxFps > 0) ? (1000000ULL / static_cast<uint64_t>(m_cfg.maxFps)) : 8333ULL;

    for (;;) {
        {
            std::unique_lock<std::mutex> qlock(pktQueueMutex);
            pktQueueCv.wait(qlock, [&]() { return !pktQueue.empty() || recvDone || !m_running.load(); });
            if (pktQueue.empty()) break;
            pktBuf = std::move(pktQueue.front());
            pktQueue.pop_front();
        }
        size_t sz = pktBuf.size();

        // Check if this packet is codec config (SPS/PPS without IDR slice)
        if (sz < 128) {
            size_t off = (sz >= 4 && pktBuf[2] == 0x00) ? 4 : 3;
            if (off < sz) {
                int nalType = pktBuf[off] & 0x1F;
                if (nalType == 7 || nalType == 8) {
                    spsPpsBuf = std::move(pktBuf);
                    continue;
                }
            }
        }

        const uint8_t* ptr = pktBuf.data();
        size_t decodeSz = pktBuf.size();
        if (!spsPpsBuf.empty()) {
            combinedBuf.clear();
            combinedBuf.reserve(spsPpsBuf.size() + pktBuf.size());
            combinedBuf.insert(combinedBuf.end(), spsPpsBuf.begin(), spsPpsBuf.end());
            combinedBuf.insert(combinedBuf.end(), pktBuf.begin(), pktBuf.end());
            ptr = combinedBuf.data();
            decodeSz = combinedBuf.size();
            spsPpsBuf.clear();
        }

        ptsUs += frameStepUs;
        auto targetFrame = acquireFrame();
        if (decoder.DecodeFrame(ptr, decodeSz, ptsUs, *targetFrame)) {
            if (m_recording.load() && m_rec) {
                std::lock_guard<std::mutex> rlock(m_recMutex);
                if (m_rec && m_rec->writer && m_rec->streamIdx != static_cast<DWORD>(-1) &&
                    targetFrame->width == m_rec->width && targetFrame->height == m_rec->height &&
                    !targetFrame->bgra.empty()) {
                    DWORD frameBytes = static_cast<DWORD>(m_rec->width * m_rec->height * 4);
                    IMFSample* recSample = nullptr;
                    IMFMediaBuffer* recBuf = nullptr;
                    if (SUCCEEDED(MFCreateMemoryBuffer(frameBytes, &recBuf))) {
                        BYTE* dst = nullptr;
                        if (SUCCEEDED(recBuf->Lock(&dst, nullptr, nullptr))) {
                            // RGB32 in Media Foundation is bottom-up
                            int rowBytes = m_rec->width * 4;
                            for (int row = 0; row < m_rec->height; ++row) {
                                std::memcpy(
                                    dst + static_cast<size_t>(row) * rowBytes,
                                    targetFrame->bgra.data() + static_cast<size_t>(m_rec->height - 1 - row) * rowBytes,
                                    rowBytes
                                );
                            }
                            recBuf->Unlock();
                            recBuf->SetCurrentLength(frameBytes);
                            if (SUCCEEDED(MFCreateSample(&recSample))) {
                                recSample->AddBuffer(recBuf);
                                LONGLONG ts = static_cast<LONGLONG>(GetTickCount64() - m_rec->startTick) * 10000LL;
                                recSample->SetSampleTime(ts);
                                recSample->SetSampleDuration(333333LL);
                                m_rec->writer->WriteSample(m_rec->streamIdx, recSample);
                                recSample->Release();
                            }
                        }
                        recBuf->Release();
                    }
                }
            }
            publishFrame(targetFrame);
        }
    }

    shutdown(videoSock, SD_BOTH);
    closesocket(videoSock);
    {
        std::lock_guard<std::mutex> qlock(pktQueueMutex);
        recvDone = true;
    }
    pktQueueCv.notify_all();
    if (videoRecvThread.joinable()) videoRecvThread.join();
    cleanupSession();
}

void MirrorSession::SendTouch(int action, int x, int y, int screenW, int screenH) {
    if (!m_running.load() || screenW <= 0 || screenH <= 0) return;

    if (m_ctrlSocket != ~uintptr_t(0)) {
        uint8_t msg[10];
        msg[0] = 1; // TOUCH
        msg[1] = static_cast<uint8_t>(action);
        WriteBE16(msg + 2, static_cast<uint16_t>(x));
        WriteBE16(msg + 4, static_cast<uint16_t>(y));
        WriteBE16(msg + 6, static_cast<uint16_t>(screenW));
        WriteBE16(msg + 8, static_cast<uint16_t>(screenH));
        std::lock_guard<std::mutex> lock(m_ctrlMutex);
        send(static_cast<SOCKET>(m_ctrlSocket), reinterpret_cast<const char*>(msg), sizeof(msg), 0);
    }
}

void MirrorSession::SendKey(int androidKeyCode) {
    if (!m_running.load()) return;

    if (m_ctrlSocket != ~uintptr_t(0)) {
        uint8_t msg[5];
        msg[0] = 2; // KEY
        WriteBE32(msg + 1, static_cast<uint32_t>(androidKeyCode));
        std::lock_guard<std::mutex> lock(m_ctrlMutex);
        send(static_cast<SOCKET>(m_ctrlSocket), reinterpret_cast<const char*>(msg), sizeof(msg), 0);
    }
}

void MirrorSession::SendScroll(int x, int y, int screenW, int screenH, int vScroll) {
    if (!m_running.load() || screenW <= 0 || screenH <= 0 || vScroll == 0) return;

    if (m_ctrlSocket != ~uintptr_t(0)) {
        uint8_t msg[10];
        msg[0] = 3; // SCROLL
        WriteBE16(msg + 1, static_cast<uint16_t>(x));
        WriteBE16(msg + 3, static_cast<uint16_t>(y));
        WriteBE16(msg + 5, static_cast<uint16_t>(screenW));
        WriteBE16(msg + 7, static_cast<uint16_t>(screenH));
        msg[9] = static_cast<uint8_t>(static_cast<int8_t>(std::clamp(vScroll, -127, 127)));
        std::lock_guard<std::mutex> lock(m_ctrlMutex);
        send(static_cast<SOCKET>(m_ctrlSocket), reinterpret_cast<const char*>(msg), sizeof(msg), 0);
    }
}

void MirrorSession::SendText(const std::wstring& text) {
    if (!m_running.load() || text.empty() || m_ctrlSocket == ~uintptr_t(0)) return;

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0 || utf8Len > 1024) return;

    std::vector<uint8_t> msg(3 + static_cast<size_t>(utf8Len));
    msg[0] = 4; // TEXT
    WriteBE16(msg.data() + 1, static_cast<uint16_t>(utf8Len));
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        reinterpret_cast<char*>(msg.data() + 3), utf8Len, nullptr, nullptr);

    std::lock_guard<std::mutex> lock(m_ctrlMutex);
    send(static_cast<SOCKET>(m_ctrlSocket), reinterpret_cast<const char*>(msg.data()), static_cast<int>(msg.size()), 0);
}

void MirrorSession::SendClipboard(const std::wstring& text, bool paste) {
    if (!m_running.load() || m_ctrlSocket == ~uintptr_t(0)) return;
    int utf8Len = text.empty() ? 0 : WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (utf8Len < 0 || utf8Len > 60000) return;

    std::vector<uint8_t> msg(4 + static_cast<size_t>(utf8Len));
    msg[0] = 5; // SET_CLIPBOARD
    msg[1] = paste ? 1 : 0;
    WriteBE16(msg.data() + 2, static_cast<uint16_t>(utf8Len));
    if (utf8Len > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
            reinterpret_cast<char*>(msg.data() + 4), utf8Len, nullptr, nullptr);
    }

    std::lock_guard<std::mutex> lock(m_ctrlMutex);
    send(static_cast<SOCKET>(m_ctrlSocket), reinterpret_cast<const char*>(msg.data()), static_cast<int>(msg.size()), 0);
}

void MirrorSession::SetScreenPower(bool on) {
    if (!m_running.load() || m_ctrlSocket == ~uintptr_t(0)) return;
    uint8_t msg[2] = { 6, static_cast<uint8_t>(on ? 1 : 0) };
    std::lock_guard<std::mutex> lock(m_ctrlMutex);
    send(static_cast<SOCKET>(m_ctrlSocket), reinterpret_cast<const char*>(msg), sizeof(msg), 0);
}

void MirrorSession::ExpandNotifications(bool expand) {
    if (!m_running.load() || m_ctrlSocket == ~uintptr_t(0)) return;
    uint8_t msg[2] = { 7, static_cast<uint8_t>(expand ? 1 : 0) };
    std::lock_guard<std::mutex> lock(m_ctrlMutex);
    send(static_cast<SOCKET>(m_ctrlSocket), reinterpret_cast<const char*>(msg), sizeof(msg), 0);
}

void MirrorSession::RotateDevice() {
    if (!m_running.load() || m_ctrlSocket == ~uintptr_t(0)) return;
    uint8_t msg[1] = { 8 };
    std::lock_guard<std::mutex> lock(m_ctrlMutex);
    send(static_cast<SOCKET>(m_ctrlSocket), reinterpret_cast<const char*>(msg), sizeof(msg), 0);
}

void MirrorSession::SetCameraZoom(float zoomRatio) {
    if (!m_running.load() || m_ctrlSocket == ~uintptr_t(0)) return;
    int pct = std::clamp<int>(static_cast<int>(zoomRatio * 100.0f + 0.5f), 100, 800);
    uint8_t msg[3];
    msg[0] = 9;
    WriteBE16(msg + 1, static_cast<uint16_t>(pct));
    std::lock_guard<std::mutex> lock(m_ctrlMutex);
    send(static_cast<SOCKET>(m_ctrlSocket), reinterpret_cast<const char*>(msg), sizeof(msg), 0);
}

void MirrorSession::TriggerCameraFocus() {
    if (!m_running.load() || m_ctrlSocket == ~uintptr_t(0)) return;
    uint8_t msg[1] = { 10 };
    std::lock_guard<std::mutex> lock(m_ctrlMutex);
    send(static_cast<SOCKET>(m_ctrlSocket), reinterpret_cast<const char*>(msg), sizeof(msg), 0);
}

bool MirrorSession::StartRecording(const std::wstring& mp4Path) {
    StopRecording();
    if (!m_running.load()) return false;

    auto cur = GetLatestFrame();
    StreamStats st = GetStats();
    int w = (cur && cur->width > 0) ? cur->width : ((st.width > 0) ? st.width : 1280);
    int h = (cur && cur->height > 0) ? cur->height : ((st.height > 0) ? st.height : 720);

    std::lock_guard<std::mutex> lock(m_recMutex);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_rec = new RecImpl();
    m_rec->startTick = GetTickCount64();
    m_rec->width = w;
    m_rec->height = h;
    m_rec->mfStarted = SUCCEEDED(MFStartup(MF_VERSION));

    if (FAILED(MFCreateSinkWriterFromURL(mp4Path.c_str(), nullptr, nullptr, &m_rec->writer)) || !m_rec->writer) {
        if (m_rec->mfStarted) MFShutdown();
        delete m_rec;
        m_rec = nullptr;
        return false;
    }

    IMFMediaType* outType = nullptr;
    if (SUCCEEDED(MFCreateMediaType(&outType))) {
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        outType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(m_cfg.bitrateMbps * 1000000));
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(outType, MF_MT_FRAME_RATE, 30, 1);
        MFSetAttributeRatio(outType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        m_rec->writer->AddStream(outType, &m_rec->streamIdx);
        outType->Release();
    }

    if (m_rec->streamIdx == static_cast<DWORD>(-1)) {
        m_rec->writer->Release();
        if (m_rec->mfStarted) MFShutdown();
        delete m_rec;
        m_rec = nullptr;
        return false;
    }

    IMFMediaType* inType = nullptr;
    if (SUCCEEDED(MFCreateMediaType(&inType))) {
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(inType, MF_MT_FRAME_RATE, 30, 1);
        MFSetAttributeRatio(inType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        HRESULT hr = m_rec->writer->SetInputMediaType(m_rec->streamIdx, inType, nullptr);
        inType->Release();
        if (FAILED(hr)) {
            m_rec->writer->Release();
            if (m_rec->mfStarted) MFShutdown();
            delete m_rec;
            m_rec = nullptr;
            return false;
        }
    }

    if (m_cfg.mirrorAudio) {
        IMFMediaType* aOutType = nullptr;
        if (SUCCEEDED(MFCreateMediaType(&aOutType))) {
            aOutType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            aOutType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            aOutType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            aOutType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
            aOutType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            aOutType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000);
            aOutType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
            aOutType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
            if (SUCCEEDED(m_rec->writer->AddStream(aOutType, &m_rec->audioStreamIdx))) {
                IMFMediaType* aInType = nullptr;
                if (SUCCEEDED(MFCreateMediaType(&aInType))) {
                    aInType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                    aInType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                    aInType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                    aInType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
                    aInType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                    aInType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
                    aInType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
                    if (FAILED(m_rec->writer->SetInputMediaType(m_rec->audioStreamIdx, aInType, nullptr))) {
                        m_rec->audioStreamIdx = static_cast<DWORD>(-1);
                    }
                    aInType->Release();
                }
            }
            aOutType->Release();
        }
    }

    if (FAILED(m_rec->writer->BeginWriting())) {
        m_rec->writer->Release();
        if (m_rec->mfStarted) MFShutdown();
        delete m_rec;
        m_rec = nullptr;
        return false;
    }

    m_recording.store(true);
    return true;
}

void MirrorSession::StopRecording() {
    m_recording.store(false);
    std::lock_guard<std::mutex> lock(m_recMutex);
    if (m_rec) {
        if (m_rec->writer) {
            m_rec->writer->Finalize();
            m_rec->writer->Release();
        }
        if (m_rec->mfStarted) {
            MFShutdown();
        }
        delete m_rec;
        m_rec = nullptr;
    }
}
