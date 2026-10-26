// The launcher's background video (menu_skin.cpp): assets/menu_skin/background.mp4, played without its sound and looped behind
// the launcher. Windows' own decoder (Media Foundation's source reader, which turns the H.264 into 32-bit pictures) decodes it on a
// thread of its own, a frame at a time at the video's own pace, and the launcher takes the newest frame each time it draws
// (menu_skin::update, which hands it to the UI as an RGBA picture). Not polled for a second (the game is running, the launcher
// is gone), the thread waits without decoding. On other systems, or if the video can't be opened, there is no video and the
// launcher keeps its background picture.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include "conker.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#endif

namespace {
    std::mutex frame_mutex;
    std::vector<char> latest;       // RGBA, width x height
    uint32_t latest_width = 0, latest_height = 0;
    bool fresh = false;
    std::atomic<bool> running{ false };
    std::atomic<bool> opened{ false };
    std::atomic<int64_t> last_poll_ms{ 0 };
    std::atomic<int> open_result{ 0 };   // the thread's answer: 1 playing, -1 couldn't
    std::thread worker;

    int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // How bright the video is drawn (1 = as it is): a little darker, so the launcher's lettering reads over it.
    constexpr uint32_t brightness_256 = 190;

#if defined(_WIN32)
    template <typename T> void release(T*& p) {
        if (p != nullptr) {
            p->Release();
            p = nullptr;
        }
    }

    void play(std::filesystem::path path) {
        std::atomic<int>* result = &open_result;
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool com = SUCCEEDED(hr);
        hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(hr)) {
            std::printf("[skin] background.mp4: Media Foundation didn't start (%08lX)\n", (unsigned long)hr);
            result->store(-1);
            if (com) CoUninitialize();
            return;
        }
        IMFAttributes* attributes = nullptr;
        IMFSourceReader* reader = nullptr;
        IMFMediaType* wanted = nullptr;
        IMFMediaType* actual = nullptr;
        UINT32 width = 0, height = 0;
        LONG stride = 0;
        bool ok = SUCCEEDED(MFCreateAttributes(&attributes, 1)) &&
            SUCCEEDED(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)) &&
            SUCCEEDED(MFCreateSourceReaderFromURL(path.wstring().c_str(), attributes, &reader)) &&
            SUCCEEDED(reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE)) &&
            SUCCEEDED(reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE)) &&
            SUCCEEDED(MFCreateMediaType(&wanted)) &&
            SUCCEEDED(wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) &&
            SUCCEEDED(wanted->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) &&
            SUCCEEDED(reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, wanted)) &&
            SUCCEEDED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual)) &&
            SUCCEEDED(MFGetAttributeSize(actual, MF_MT_FRAME_SIZE, &width, &height)) && width > 0 && height > 0;
        if (ok) {
            UINT32 s = 0;
            stride = SUCCEEDED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)) ? (LONG)(INT32)s : (LONG)(width * 4);
        }
        release(wanted);
        release(actual);
        release(attributes);
        if (!ok) {
            std::printf("[skin] background.mp4: couldn't be opened for playing (no video: the background picture is used)\n");
            release(reader);
            MFShutdown();
            if (com) CoUninitialize();
            result->store(-1);
            return;
        }
        std::printf("[skin] background.mp4: playing (%ux%u)\n", (unsigned)width, (unsigned)height);
        result->store(1);
        std::vector<char> rgba((size_t)width * height * 4);
        auto clock_start = std::chrono::steady_clock::now();
        int64_t paused_ms = 0;
        while (running.load()) {
            // nobody has looked for a second: wait (and don't let the time spent waiting count)
            if (now_ms() - last_poll_ms.load() > 1000) {
                const int64_t before = now_ms();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                paused_ms += now_ms() - before;
                continue;
            }
            DWORD index = 0, flags = 0;
            LONGLONG timestamp = 0;
            IMFSample* sample = nullptr;
            hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &index, &flags, &timestamp, &sample);
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR) != 0) {
                release(sample);
                std::printf("[skin] background.mp4: stopped (read failed, %08lX)\n", (unsigned long)hr);
                break;
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                release(sample);
                // back to the start, and its clock with it
                PROPVARIANT position;
                PropVariantInit(&position);
                position.vt = VT_I8;
                position.hVal.QuadPart = 0;
                reader->SetCurrentPosition(GUID_NULL, position);
                PropVariantClear(&position);
                clock_start = std::chrono::steady_clock::now();
                paused_ms = 0;
                continue;
            }
            if (sample == nullptr) {
                continue;
            }
            // the frame's time (100 ns units) from the start: wait for it
            const auto due = clock_start + std::chrono::milliseconds(timestamp / 10000 + paused_ms);
            while (running.load() && std::chrono::steady_clock::now() < due) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            IMFMediaBuffer* buffer = nullptr;
            IMF2DBuffer* buffer2d = nullptr;
            BYTE* data = nullptr;
            DWORD length = 0;
            LONG pitch = stride != 0 ? stride : (LONG)(width * 4);
            bool locked2d = false, locked = false;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer))) {
                // (0.10.34) the decoder's rows can be wider than the picture (854 wide came in rows of 864 or so): the buffer's own
                // pitch, from its 2D lock where it has one, or else the length over the rows
                if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2d))) && SUCCEEDED(buffer2d->Lock2D(&data, &pitch))) {
                    locked2d = true;
                    length = (DWORD)((pitch < 0 ? -pitch : pitch) * (LONG)height);
                } else if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
                    locked = true;
                    if (stride == 0 && height > 0 && length / height >= width * 4) {
                        pitch = (LONG)(length / height);
                    }
                }
            }
            if (locked2d || locked) {
                const size_t needed = (size_t)(pitch < 0 ? -pitch : pitch) * height;
                if (length >= needed) {
                    for (UINT32 y = 0; y < height; y++) {
                        // (a 2D lock's pointer is the top row's, whichever way the rows run; a plain lock's is the buffer's start)
                        const BYTE* row = locked2d ? data + (ptrdiff_t)y * (ptrdiff_t)pitch :
                            pitch >= 0 ? data + (size_t)y * (size_t)pitch : data + (size_t)(height - 1 - y) * (size_t)(-pitch);
                        char* out = rgba.data() + (size_t)y * width * 4;
                        for (UINT32 x = 0; x < width; x++) {
                            // B G R X in, R G B A out
                            out[x * 4 + 0] = (char)((row[x * 4 + 2] * brightness_256) >> 8);
                            out[x * 4 + 1] = (char)((row[x * 4 + 1] * brightness_256) >> 8);
                            out[x * 4 + 2] = (char)((row[x * 4 + 0] * brightness_256) >> 8);
                            out[x * 4 + 3] = (char)0xFF;
                        }
                    }
                    std::lock_guard lock{ frame_mutex };
                    latest = rgba;
                    latest_width = width;
                    latest_height = height;
                    fresh = true;
                }
                if (locked2d) {
                    buffer2d->Unlock2D();
                } else {
                    buffer->Unlock();
                }
            }
            release(buffer2d);
            release(buffer);
            release(sample);
        }
        release(reader);
        MFShutdown();
        if (com) CoUninitialize();
    }
#endif
}

bool conker::background_video::start(const std::filesystem::path& path) {
#if defined(_WIN32)
    if (running.load()) {
        return opened.load();
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return false;
    }
    running.store(true);
    last_poll_ms.store(now_ms());
    open_result.store(0);
    std::atomic<int>& result = open_result;
    worker = std::thread(play, path);
    // the first answer: opened or not (a moment at most)
    for (int i = 0; i < 300 && result.load() == 0; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (result.load() != 1) {
        running.store(false);
        // (the thread ends by itself once it's failed, or at the next read once running is off)
        if (result.load() == 0) {
            worker.detach();
        } else {
            worker.join();
        }
        return false;
    }
    worker.detach();
    opened.store(true);
    return true;
#else
    (void)path;
    return false;
#endif
}

bool conker::background_video::take_frame(std::vector<char>& rgba, uint32_t& width, uint32_t& height) {
    last_poll_ms.store(now_ms());
    std::lock_guard lock{ frame_mutex };
    if (!fresh) {
        return false;
    }
    fresh = false;
    rgba.swap(latest);
    width = latest_width;
    height = latest_height;
    return true;
}
