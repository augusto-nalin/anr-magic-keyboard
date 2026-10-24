// Measures key press latency with a microphone, for any keyboard setup.
//
// Both ends are timestamped on the same hardware clock (QueryPerformanceCounter) with nothing
// buffered in between:
//  - the click: WASAPI hands out the QPC time at which the first frame of each capture packet was
//    recorded, so a click's time comes from the audio engine, not from when we got to read it
//  - the key: a low-level keyboard hook, stamped the moment Windows calls it, on a time-critical
//    thread in a high priority process
//
// What's left between the two is constant (mic and audio engine path, click vs actuation point),
// so one run means little on its own: run it per setup (cable, bluetooth, bridge) and compare.
//
// Build: tools/latency/mic.ps1 (MSVC, C++latest)

// No WIN32_LEAN_AND_MEAN: the property key headers need what it strips
#define NOMINMAX
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
// initguid first, so PKEY_Device_FriendlyName is defined here instead of needing another lib
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <numeric>
#include <print>
#include <set>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

namespace mic_latency {
    namespace {
        // The click is searched for around each key event instead of with one global threshold,
        // Magic Keyboard scissor switches are too quiet for that. The noise floor comes from just
        // before the search window, and the onset is the first frame that rises ONSET_FRACTION of
        // the way from that floor to the click's peak.
        constexpr double SEARCH_BEFORE_MS{200.0};
        constexpr double SEARCH_AFTER_MS{30.0};
        constexpr double NOISE_BEFORE_MS{400.0};
        constexpr double ONSET_FRACTION{0.2};
        constexpr double MIN_SNR{4.0};

        LARGE_INTEGER qpc_frequency{};

        // Loudest channel of every captured frame, and where each packet starts on the QPC clock
        struct packet {
            size_t first;
            double first_ms;
        };

        std::vector<float> levels{};
        std::vector<packet> packets{};
        double frame_ms{};

        std::mutex gate{};
        std::vector<double> key_ms{};

        std::set<DWORD> keys_down{};
        std::atomic<bool> recording{true};
        DWORD hook_thread{};
        size_t target{30};

        // Same base as WASAPI's QPC positions, which come in 100ns units
        double qpc_now_ms() {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            return static_cast<double>(now.QuadPart) * 1000.0 / static_cast<double>(qpc_frequency.QuadPart);
        }

        std::string narrow(const wchar_t* text) {
            const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            std::string result(static_cast<size_t>(std::max(size - 1, 0)), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
            return result;
        }

        std::string device_name(IMMDevice* device) {
            IPropertyStore* store{nullptr};
            std::string name{"?"};
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) {
                    name = narrow(value.pwszVal);
                }
                PropVariantClear(&value);
                store->Release();
            }
            return name;
        }

        // Default capture device, or the first whose name contains `match` (case sensitive)
        IMMDevice* find_microphone(const std::string& match) {
            IMMDeviceEnumerator* enumerator{nullptr};
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)))) {
                return nullptr;
            }

            IMMDevice* found{nullptr};
            if (match.empty()) {
                enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &found);
            } else {
                IMMDeviceCollection* devices{nullptr};
                UINT count{};
                enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices);
                devices->GetCount(&count);
                for (UINT i = 0; i < count && found == nullptr; ++i) {
                    IMMDevice* device{nullptr};
                    devices->Item(i, &device);
                    if (device_name(device).find(match) != std::string::npos) {
                        found = device;
                    } else {
                        device->Release();
                    }
                }
                devices->Release();
            }

            enumerator->Release();
            return found;
        }

        void list_microphones() {
            IMMDeviceEnumerator* enumerator{nullptr};
            CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                             __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
            IMMDeviceCollection* devices{nullptr};
            UINT count{};
            enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices);
            devices->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* device{nullptr};
                devices->Item(i, &device);
                std::println("  {}", device_name(device));
                device->Release();
            }
            devices->Release();
            enumerator->Release();
        }

        bool is_float(const WAVEFORMATEX* format) {
            if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                return true;
            }
            if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
                return IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
            }
            return false;
        }

        // Loudest channel of one frame, 0..1
        double frame_level(const BYTE* data, UINT32 frame, const WAVEFORMATEX* format, bool floating) {
            double level{0};
            for (WORD channel = 0; channel < format->nChannels; ++channel) {
                const size_t index = static_cast<size_t>(frame) * format->nChannels + channel;
                const double sample = floating
                                          ? static_cast<double>(reinterpret_cast<const float*>(data)[index])
                                          : reinterpret_cast<const int16_t*>(data)[index] / 32768.0;
                level = std::max(level, std::abs(sample));
            }
            return level;
        }

        void record(IMMDevice* device) {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

            IAudioClient* client{nullptr};
            WAVEFORMATEX* format{nullptr};
            IAudioCaptureClient* capture{nullptr};
            HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);

            device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
            client->GetMixFormat(&format);
            const bool floating = is_float(format);
            if (!floating && format->wBitsPerSample != 16) {
                std::println(stderr, "unsupported mic format: {} bits", format->wBitsPerSample);
                recording = false;
                return;
            }

            // Event driven with the smallest buffer the engine allows, so packets come in steadily
            client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, format, nullptr);
            client->SetEventHandle(ready);
            client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture));
            client->Start();

            frame_ms = 1000.0 / format->nSamplesPerSec;
            levels.reserve(static_cast<size_t>(format->nSamplesPerSec) * 120);
            size_t timestamp_errors{0};

            while (recording) {
                WaitForSingleObject(ready, 100);

                UINT32 packet{};
                while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
                    BYTE* data{nullptr};
                    UINT32 frames{};
                    DWORD flags{};
                    UINT64 qpc_position{};  // 100ns units, when the packet's first frame was recorded
                    capture->GetBuffer(&data, &frames, &flags, nullptr, &qpc_position);

                    // Without a timestamp the frames still go in, their time is extrapolated from
                    // the previous packet
                    if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0) {
                        timestamp_errors++;
                    } else {
                        packets.push_back({levels.size(), static_cast<double>(qpc_position) / 10000.0});
                    }

                    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                    for (UINT32 frame = 0; frame < frames; ++frame) {
                        levels.push_back(silent ? 0.0f : static_cast<float>(frame_level(data, frame, format, floating)));
                    }

                    capture->ReleaseBuffer(frames);
                }
            }

            client->Stop();
            if (timestamp_errors > 0) {
                std::println(stderr, "{} packets had no valid timestamp and were skipped", timestamp_errors);
            }
            capture->Release();
            CoTaskMemFree(format);
            client->Release();
            CloseHandle(ready);
            CoUninitialize();
        }

        // Runs inside Windows' input processing: timestamp first, then get out
        LRESULT CALLBACK on_key(int code, WPARAM message, LPARAM lparam) {
            const double at = qpc_now_ms();

            if (code >= 0) {
                const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
                const bool injected = (info->flags & LLKHF_INJECTED) != 0;

                if (!injected && (message == WM_KEYUP || message == WM_SYSKEYUP)) {
                    keys_down.erase(info->vkCode);
                } else if (!injected && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && keys_down.insert(info->vkCode).second) {
                    // First key down only, auto-repeat has no click of its own
                    size_t count{};
                    {
                        std::lock_guard lock{gate};
                        key_ms.push_back(at);
                        count = key_ms.size();
                    }

                    std::print("\r{}/{} presses", count, target);
                    if (count >= target) {
                        PostThreadMessageW(hook_thread, WM_QUIT, 0, 0);
                    }
                }
            }

            return CallNextHookEx(nullptr, code, message, lparam);
        }

        double time_of(size_t index) {
            auto next = std::ranges::upper_bound(packets, index, {}, &packet::first);
            const packet& at = *std::prev(next);
            return at.first_ms + static_cast<double>(index - at.first) * frame_ms;
        }

        size_t index_of(double ms) {
            auto next = std::ranges::upper_bound(packets, ms, {}, &packet::first_ms);
            if (next == packets.begin()) {
                return 0;
            }
            const packet& at = *std::prev(next);
            const auto index = at.first + static_cast<size_t>(std::max(0.0, (ms - at.first_ms) / frame_ms));
            return std::min(index, levels.size());
        }

        // Only after recording stopped, the recorder owns levels and packets until then
        std::string report(const std::string& label) {
            std::vector<double> samples{};
            std::vector<double> snrs{};
            size_t unmatched{0};

            for (const double key: key_ms) {
                const size_t noise_from = index_of(key - NOISE_BEFORE_MS);
                const size_t from       = index_of(key - SEARCH_BEFORE_MS);
                const size_t to         = index_of(key + SEARCH_AFTER_MS);
                if (packets.empty() || from <= noise_from || to <= from) {
                    unmatched++;
                    continue;
                }

                const double noise = std::accumulate(levels.begin() + noise_from, levels.begin() + from, 0.0)
                                     / static_cast<double>(from - noise_from);
                const auto peak    = std::max_element(levels.begin() + from, levels.begin() + to);
                const double snr   = noise > 0 ? *peak / noise : 0;
                if (snr < MIN_SNR) {
                    unmatched++;
                    continue;
                }

                const double threshold = noise + ONSET_FRACTION * (*peak - noise);
                const auto onset = std::find_if(levels.begin() + from, peak + 1, [threshold](float level) {
                    return level > threshold;
                });

                samples.push_back(key - time_of(static_cast<size_t>(onset - levels.begin())));
                snrs.push_back(snr);
            }

            std::string text = std::format("[{}] key presses {}, matched {}, unmatched (click too quiet) {}\n",
                                           label, key_ms.size(), samples.size(), unmatched);
            if (samples.empty()) {
                return text + "no matched samples: is the mic picking up the clicks? Get it closer or tap harder.\n";
            }

            std::ranges::sort(snrs);
            text += std::format("click peak vs noise: median {:.0f}x (higher is cleaner)\n", snrs[snrs.size() / 2]);

            std::vector<double> sorted = samples;
            std::ranges::sort(sorted);
            const double mean = std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());
            double variance{0};
            for (const double sample: samples) {
                variance += (sample - mean) * (sample - mean);
            }
            const double sd = std::sqrt(variance / static_cast<double>(samples.size()));
            const auto percentile = [](const std::vector<double>& values, double p) {
                const auto index = static_cast<size_t>(std::ceil(p * static_cast<double>(values.size()))) - 1;
                return values[std::min(values.size() - 1, index)];
            };

            text += "click -> Windows key event (ms, includes constant offsets, compare between runs):\n";
            text += std::format("  min {:.1f}  median {:.1f}  mean {:.1f}  sd {:.1f}  p90 {:.1f}  max {:.1f}\n",
                                sorted.front(), percentile(sorted, 0.5), mean, sd, percentile(sorted, 0.9), sorted.back());

            // The first press after a pause waits for the link to wake, and the odd click gets
            // misjudged, so also without the fastest and slowest few
            constexpr size_t TRIM{2};
            if (sorted.size() > 2 * TRIM + 2) {
                const std::vector<double> trimmed(sorted.begin() + TRIM, sorted.end() - TRIM);
                const double trimmed_mean = std::accumulate(trimmed.begin(), trimmed.end(), 0.0) / static_cast<double>(trimmed.size());
                text += std::format("  trimmed (without the {} fastest and slowest): median {:.1f}  mean {:.1f}  p90 {:.1f}  max {:.1f}\n",
                                    TRIM, percentile(trimmed, 0.5), trimmed_mean, percentile(trimmed, 0.9), trimmed.back());
            }
            text += "  samples:";
            for (const double sample: samples) {
                text += std::format(" {:.1f}", sample);
            }
            return text + "\n";
        }
    }
}

// mic_latency <label> [presses] [report file] [mic name contains]
// mic_latency --list
int main(int argc, char** argv) {
    using namespace mic_latency;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    QueryPerformanceFrequency(&qpc_frequency);

    const std::string label = argc > 1 ? argv[1] : "run";
    if (label == "--list") {
        std::println("capture devices:");
        list_microphones();
        return 0;
    }

    target = argc > 2 ? std::stoul(argv[2]) : 30;
    const std::string report_file = argc > 3 ? argv[3] : "";
    const std::string mic         = argc > 4 ? argv[4] : "";

    IMMDevice* device = find_microphone(mic);
    if (device == nullptr) {
        std::println(stderr, "no microphone found{}", mic.empty() ? "" : " matching '" + mic + "'");
        return 1;
    }

    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    std::thread recorder{record, device};

    hook_thread = GetCurrentThreadId();
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, on_key, GetModuleHandleW(nullptr), 0);
    if (hook == nullptr) {
        std::println(stderr, "SetWindowsHookEx failed: {}", GetLastError());
        recording = false;
        recorder.join();
        return 1;
    }

    std::println("[{}] listening on '{}'", label, device_name(device));
    std::println("Tap ONE key {} times, firmly and slowly (about once a second), mic close to the keyboard, room quiet.", target);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {}

    UnhookWindowsHookEx(hook);
    std::this_thread::sleep_for(std::chrono::milliseconds{300});   // the last click's packets
    recording = false;
    recorder.join();
    device->Release();

    const std::string text = report(label);
    std::println("\n{}", text);
    if (!report_file.empty()) {
        std::ofstream{report_file} << text;
    }

    CoUninitialize();
    return 0;
}
