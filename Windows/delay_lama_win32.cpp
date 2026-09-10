#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <mmsystem.h>
#include <objidl.h>
#include <shellapi.h>
#include <gdiplus.h>

#include "../Sources/standalone_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")

using Gdiplus::Color;
using Gdiplus::Graphics;
using Gdiplus::Image;
using Gdiplus::InterpolationModeHighQualityBicubic;
using Gdiplus::LineCapRound;
using Gdiplus::Pen;
using Gdiplus::PointF;
using Gdiplus::Rect;
using Gdiplus::RectF;
using Gdiplus::SolidBrush;
using Gdiplus::SmoothingModeAntiAlias;
using Gdiplus::UnitPixel;

namespace {

constexpr int kCanvasWidth = 360;
constexpr int kCanvasHeight = 510;
constexpr uint32_t kAudioFrames = 512;
constexpr uint32_t kAudioBuffers = 4;

constexpr UINT_PTR kAnimationTimer = 1;
constexpr UINT kAnimationIntervalMs = 60;
constexpr UINT kMenuAbout = 1001;
constexpr UINT kMenuExit = 1002;

const RECT kXYRect = {98, 365, 262, 426};
const RECT kGlideRect = {17, 443, 84, 505};
const RECT kVoiceRect = {279, 443, 345, 505};
const RECT kDelayRect = {82, 458, 278, 501};
const RECT kHelpRect = {280, 295, 328, 338};

float clamp01(float value) {
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

bool contains(const RECT &rect, int x, int y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

std::wstring module_directory() {
    wchar_t path[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (length == 0)
        return L".";
    std::wstring full_path(path, length);
    const size_t slash = full_path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : full_path.substr(0, slash);
}

bool file_exists(const std::wstring &path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::unique_ptr<Image> load_png(const std::wstring &path) {
    std::unique_ptr<Image> image(new Image(path.c_str(), FALSE));
    if (!image || image->GetLastStatus() != Gdiplus::Ok)
        return nullptr;
    return image;
}

class AudioEngine {
public:
    explicit AudioEngine(DelayLamaHandle *handle) : handle_(handle) {}

    ~AudioEngine() { stop(); }

    bool start() {
        if (!handle_ || wave_out_)
            return false;

        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_)
            return false;

        WAVEFORMATEX format = {};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = 48000;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        const MMRESULT open_result = waveOutOpen(
            &wave_out_, WAVE_MAPPER, &format,
            reinterpret_cast<DWORD_PTR>(&AudioEngine::wave_callback),
            reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
        if (open_result != MMSYSERR_NOERROR) {
            CloseHandle(event_);
            event_ = nullptr;
            wave_out_ = nullptr;
            return false;
        }

        prepared_headers_ = 0;
        for (uint32_t index = 0; index < kAudioBuffers; ++index) {
            WAVEHDR &header = headers_[index];
            ZeroMemory(&header, sizeof(header));
            header.lpData = reinterpret_cast<LPSTR>(pcm_[index]);
            header.dwBufferLength = sizeof(pcm_[index]);
            if (waveOutPrepareHeader(wave_out_, &header, sizeof(header)) != MMSYSERR_NOERROR) {
                stop();
                return false;
            }
            ++prepared_headers_;
        }

        InterlockedExchange(&running_, 1);
        for (uint32_t index = 0; index < kAudioBuffers; ++index) {
            fill_buffer(index);
            if (waveOutWrite(wave_out_, &headers_[index], sizeof(headers_[index])) !=
                MMSYSERR_NOERROR) {
                stop();
                return false;
            }
        }

        thread_ = CreateThread(nullptr, 0, &AudioEngine::audio_thread, this, 0, nullptr);
        if (!thread_) {
            stop();
            return false;
        }
        return true;
    }

    void stop() {
        InterlockedExchange(&running_, 0);
        if (event_)
            SetEvent(event_);
        if (thread_) {
            WaitForSingleObject(thread_, INFINITE);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        if (wave_out_) {
            waveOutReset(wave_out_);
            for (uint32_t index = 0; index < prepared_headers_; ++index)
                waveOutUnprepareHeader(wave_out_, &headers_[index], sizeof(headers_[index]));
            waveOutClose(wave_out_);
            wave_out_ = nullptr;
            prepared_headers_ = 0;
        }
        if (event_) {
            CloseHandle(event_);
            event_ = nullptr;
        }
    }

private:
    static void CALLBACK wave_callback(HWAVEOUT wave_out, UINT message, DWORD_PTR instance,
                                       DWORD_PTR param1, DWORD_PTR param2) {
        (void)wave_out;
        (void)param1;
        (void)param2;
        if (message == WOM_DONE) {
            AudioEngine *engine = reinterpret_cast<AudioEngine *>(instance);
            if (engine && engine->event_)
                SetEvent(engine->event_);
        }
    }

    static DWORD WINAPI audio_thread(LPVOID parameter) {
        AudioEngine *engine = reinterpret_cast<AudioEngine *>(parameter);
        engine->render_loop();
        return 0;
    }

    void fill_buffer(uint32_t index) {
        float left[kAudioFrames] = {};
        float right[kAudioFrames] = {};
        dl_process(handle_, left, right, kAudioFrames);

        int16_t *destination = pcm_[index];
        for (uint32_t frame = 0; frame < kAudioFrames; ++frame) {
            const float left_sample = std::max(-1.0f, std::min(1.0f, left[frame]));
            const float right_sample = std::max(-1.0f, std::min(1.0f, right[frame]));
            destination[frame * 2] = static_cast<int16_t>(left_sample * 32767.0f);
            destination[frame * 2 + 1] = static_cast<int16_t>(right_sample * 32767.0f);
        }
    }

    void render_loop() {
        while (InterlockedCompareExchange(&running_, 0, 0) != 0) {
            const DWORD wait_result = WaitForSingleObject(event_, 1000);
            if (InterlockedCompareExchange(&running_, 0, 0) == 0)
                break;
            if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_TIMEOUT)
                break;

            for (uint32_t index = 0; index < kAudioBuffers; ++index) {
                WAVEHDR &header = headers_[index];
                if ((header.dwFlags & WHDR_DONE) == 0)
                    continue;
                fill_buffer(index);
                if (waveOutWrite(wave_out_, &header, sizeof(header)) != MMSYSERR_NOERROR) {
                    InterlockedExchange(&running_, 0);
                    break;
                }
            }
        }
    }

    DelayLamaHandle *handle_ = nullptr;
    HWAVEOUT wave_out_ = nullptr;
    HANDLE event_ = nullptr;
    HANDLE thread_ = nullptr;
    volatile LONG running_ = 0;
    uint32_t prepared_headers_ = 0;
    WAVEHDR headers_[kAudioBuffers] = {};
    int16_t pcm_[kAudioBuffers][kAudioFrames * 2] = {};
};

class Win32App {
public:
    explicit Win32App(DelayLamaHandle *handle) : handle_(handle), audio_(handle) {}

    bool load_resources() {
        resource_dir_ = module_directory() + L"\\Resources";
        const std::wstring background_path = resource_dir_ + L"\\background.png";
        const std::wstring faces_path = resource_dir_ + L"\\faces.png";
        if (!file_exists(background_path) || !file_exists(faces_path))
            return false;
        background_ = load_png(background_path);
        faces_ = load_png(faces_path);
        return background_ && faces_;
    }

    bool create_window(HINSTANCE instance, int command_show) {
        instance_ = instance;
        const wchar_t class_name[] = L"DelayLamaStandaloneWin32";

        WNDCLASSW window_class = {};
        window_class.hInstance = instance;
        window_class.lpfnWndProc = &Win32App::window_proc;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = class_name;
        if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;

        HMENU menu = CreateMenu();
        if (!menu)
            return false;
        AppendMenuW(menu, MF_STRING, kMenuAbout, L"About Delay Lama Standalone");
        AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT window_rect = {0, 0, kCanvasWidth, kCanvasHeight};
        AdjustWindowRectEx(&window_rect, style, TRUE, 0);

        hwnd_ = CreateWindowExW(0, class_name, L"Delay Lama Standalone", style,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                window_rect.right - window_rect.left,
                                window_rect.bottom - window_rect.top, nullptr, menu, instance,
                                this);
        if (!hwnd_)
            return false;

        SetTimer(hwnd_, kAnimationTimer, kAnimationIntervalMs, nullptr);
        ShowWindow(hwnd_, command_show);
        UpdateWindow(hwnd_);
        return true;
    }

    bool start_audio() { return audio_.start(); }

    void destroy_window() {
        if (hwnd_)
            DestroyWindow(hwnd_);
    }

private:
    enum class DragMode { None, XY, Glide, Voice, Delay };

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM w_param,
                                        LPARAM l_param) {
        Win32App *app = reinterpret_cast<Win32App *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const CREATESTRUCTW *create = reinterpret_cast<const CREATESTRUCTW *>(l_param);
            app = reinterpret_cast<Win32App *>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->hwnd_ = hwnd;
        }
        return app ? app->handle_message(message, w_param, l_param)
                   : DefWindowProcW(hwnd, message, w_param, l_param);
    }

    static int point_x(LPARAM l_param) { return static_cast<int>(static_cast<short>(LOWORD(l_param))); }
    static int point_y(LPARAM l_param) { return static_cast<int>(static_cast<short>(HIWORD(l_param))); }

    LRESULT handle_message(UINT message, WPARAM w_param, LPARAM l_param) {
        switch (message) {
        case WM_PAINT:
            paint_window();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (w_param == kAnimationTimer)
                InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_COMMAND:
            if (LOWORD(w_param) == kMenuAbout) {
                show_help();
                return 0;
            }
            if (LOWORD(w_param) == kMenuExit) {
                DestroyWindow(hwnd_);
                return 0;
            }
            break;
        case WM_LBUTTONDOWN:
            mouse_down(point_x(l_param), point_y(l_param));
            return 0;
        case WM_MOUSEMOVE:
            if (drag_mode_ != DragMode::None && (w_param & MK_LBUTTON))
                update_drag(point_x(l_param), point_y(l_param));
            return 0;
        case WM_LBUTTONUP:
            mouse_up();
            return 0;
        case WM_CAPTURECHANGED:
            if (drag_mode_ != DragMode::None)
                finish_drag();
            return 0;
        case WM_KEYDOWN:
            key_down(static_cast<UINT>(w_param), l_param);
            return 0;
        case WM_KEYUP:
            key_up(static_cast<UINT>(w_param));
            return 0;
        case WM_KILLFOCUS:
            release_input();
            return 0;
        case WM_DESTROY:
            release_input();
            KillTimer(hwnd_, kAnimationTimer);
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, message, w_param, l_param);
    }

    void paint_window() {
        PAINTSTRUCT paint = {};
        HDC target = BeginPaint(hwnd_, &paint);
        RECT client = {};
        GetClientRect(hwnd_, &client);
        const int width = std::max(1L, client.right - client.left);
        const int height = std::max(1L, client.bottom - client.top);

        HDC buffer = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
        HGDIOBJ previous = SelectObject(buffer, bitmap);
        render(buffer, width, height);
        BitBlt(target, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, previous);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        EndPaint(hwnd_, &paint);
    }

    int active_frame() const {
        if (handle_ && dl_is_active(handle_)) {
            const int frame = 6 + static_cast<int>(dl_get_vowel(handle_) * 23.0f + 0.5f);
            return std::max(6, std::min(29, frame));
        }
        const int cycle = static_cast<int>((GetTickCount64() / kAnimationIntervalMs) % 150);
        if ((cycle >= 68 && cycle < 72) || (cycle >= 78 && cycle < 81))
            return 4;
        return 5;
    }

    void render(HDC device_context, int width, int height) const {
        Graphics graphics(device_context);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);

        if (background_) {
            graphics.DrawImage(background_.get(), Rect(0, 0, width, height), 0, 0,
                               static_cast<INT>(background_->GetWidth()),
                               static_cast<INT>(background_->GetHeight()), UnitPixel);
        } else {
            graphics.Clear(Color(255, 30, 30, 30));
        }

        if (faces_) {
            const int frame = active_frame();
            const int column = frame / 6;
            const int row = frame % 6;
            graphics.DrawImage(faces_.get(), Rect(23, 0, 314, 311), column * 314, row * 311,
                               314, 311, UnitPixel);
        }

        draw_knob(graphics, 50.0f, 473.0f, glide_);
        draw_knob(graphics, 311.0f, 473.0f, voice_);
        draw_delay_handle(graphics);

        if (drag_mode_ == DragMode::XY) {
            const float x = kXYRect.left + xy_x_ * (kXYRect.right - kXYRect.left);
            const float y = kXYRect.top + xy_y_ * (kXYRect.bottom - kXYRect.top);
            Pen cross(Color(185, 255, 255, 255), 1.0f);
            graphics.DrawLine(&cross, PointF(static_cast<REAL>(kXYRect.left), y),
                              PointF(static_cast<REAL>(kXYRect.right), y));
            graphics.DrawLine(&cross, PointF(x, static_cast<REAL>(kXYRect.top)),
                              PointF(x, static_cast<REAL>(kXYRect.bottom)));
            SolidBrush marker(Color(230, 255, 255, 255));
            graphics.FillEllipse(&marker, RectF(x - 4.0f, y - 4.0f, 8.0f, 8.0f));
        }
    }

    static void draw_knob(Graphics &graphics, float center_x, float center_y, float value) {
        const float angle = -2.35f + clamp01(value) * 4.70f;
        const float inner_radius = 5.0f;
        const float outer_radius = 16.0f;
        const PointF start(center_x + std::sin(angle) * inner_radius,
                           center_y - std::cos(angle) * inner_radius);
        const PointF end(center_x + std::sin(angle) * outer_radius,
                         center_y - std::cos(angle) * outer_radius);
        Pen indicator(Color(255, 255, 255, 255), 4.0f);
        indicator.SetLineCap(LineCapRound, LineCapRound, Gdiplus::DashCapRound);
        graphics.DrawLine(&indicator, start, end);
    }

    void draw_delay_handle(Graphics &graphics) const {
        const float x = kDelayRect.left + 12.0f + delay_ * (kDelayRect.right - kDelayRect.left - 24.0f);
        const RectF outer(x - 9.0f, 473.0f - 9.0f, 18.0f, 18.0f);
        SolidBrush shadow(Color(165, 0, 0, 0));
        graphics.FillEllipse(&shadow, outer);
        const RectF handle(x - 8.0f, 473.0f - 8.0f, 16.0f, 16.0f);
        SolidBrush fill(Color(255, 184, 122, 46));
        graphics.FillEllipse(&fill, handle);
        Pen ring(Color(255, 242, 199, 82), 1.0f);
        graphics.DrawEllipse(&ring, RectF(x - 6.0f, 473.0f - 6.0f, 12.0f, 12.0f));
    }

    void mouse_down(int x, int y) {
        SetFocus(hwnd_);
        drag_start_x_ = x;
        drag_start_y_ = y;
        if (contains(kHelpRect, x, y)) {
            show_help();
            return;
        }
        if (contains(kXYRect, x, y)) {
            drag_mode_ = DragMode::XY;
            update_xy(x, y, true);
        } else if (contains(kGlideRect, x, y)) {
            drag_mode_ = DragMode::Glide;
            drag_start_value_ = glide_;
        } else if (contains(kVoiceRect, x, y)) {
            drag_mode_ = DragMode::Voice;
            drag_start_value_ = voice_;
        } else if (contains(kDelayRect, x, y)) {
            drag_mode_ = DragMode::Delay;
            update_drag(x, y);
        }
        if (drag_mode_ != DragMode::None)
            SetCapture(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void update_xy(int x, int y, bool begin) {
        const float width = static_cast<float>(kXYRect.right - kXYRect.left);
        const float height = static_cast<float>(kXYRect.bottom - kXYRect.top);
        xy_x_ = clamp01((static_cast<float>(x) - kXYRect.left) / width);
        xy_y_ = clamp01((static_cast<float>(y) - kXYRect.top) / height);
        const float vowel = 1.0f - xy_y_;
        if (begin)
            dl_xy_begin(handle_, xy_x_, vowel);
        else
            dl_xy_move(handle_, xy_x_, vowel);
    }

    void update_drag(int x, int y) {
        switch (drag_mode_) {
        case DragMode::XY:
            update_xy(x, y, false);
            break;
        case DragMode::Glide:
            glide_ = clamp01(drag_start_value_ + (drag_start_y_ - y) / 100.0f);
            dl_set_glide(handle_, glide_);
            break;
        case DragMode::Voice:
            voice_ = clamp01(drag_start_value_ + (drag_start_y_ - y) / 100.0f);
            dl_set_voice(handle_, voice_);
            break;
        case DragMode::Delay:
            delay_ = clamp01((static_cast<float>(x) - kDelayRect.left - 12.0f) /
                             (kDelayRect.right - kDelayRect.left - 24.0f));
            dl_set_delay(handle_, delay_);
            break;
        case DragMode::None:
            break;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void mouse_up() { finish_drag(); }

    void finish_drag() {
        if (drag_mode_ == DragMode::XY)
            dl_xy_end(handle_);
        drag_mode_ = DragMode::None;
        if (GetCapture() == hwnd_)
            ReleaseCapture();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    static int note_for_key(UINT virtual_key) {
        switch (virtual_key) {
        case 'A': return 48;
        case 'W': return 49;
        case 'S': return 50;
        case 'E': return 51;
        case 'D': return 52;
        case 'F': return 53;
        case 'T': return 54;
        case 'G': return 55;
        case 'Y': return 56;
        case 'H': return 57;
        case 'U': return 58;
        case 'J': return 59;
        case 'K': return 60;
        case 'O': return 61;
        case 'L': return 62;
        default: return -1;
        }
    }

    void key_down(UINT virtual_key, LPARAM l_param) {
        if (virtual_key >= ARRAYSIZE(key_pressed_))
            return;
        const int note = note_for_key(virtual_key);
        if (note < 0)
            return;
        if ((l_param & (1LL << 30)) != 0 || key_pressed_[virtual_key])
            return;
        key_pressed_[virtual_key] = true;
        key_notes_[virtual_key] = static_cast<uint8_t>(note);
        dl_note_on(handle_, static_cast<uint8_t>(note), 1.0f);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void key_up(UINT virtual_key) {
        if (virtual_key >= ARRAYSIZE(key_pressed_) || !key_pressed_[virtual_key])
            return;
        key_pressed_[virtual_key] = false;
        dl_note_off(handle_, key_notes_[virtual_key]);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void release_input() {
        if (drag_mode_ != DragMode::None)
            finish_drag();
        for (UINT key = 0; key < ARRAYSIZE(key_pressed_); ++key) {
            if (key_pressed_[key])
                dl_note_off(handle_, key_notes_[key]);
            key_pressed_[key] = false;
        }
        dl_all_notes_off(handle_);
    }

    void show_help() const {
        std::wstring message =
            L"Drag across the Tibetan flag to sing: horizontal controls pitch and vertical "
            L"controls the vowel. Drag the left and right knobs vertically for Glide and Voice. "
            L"Drag the bottom handle for Delay.\n\n"
            L"Computer keyboard: A W S E D F T G Y H U J K O L\n\n"
            L"MIDI: notes, pitch wheel -> vowel, CC1 vibrato, CC5 glide, CC7 volume, "
            L"CC12 delay and CC13 voice. Connected MIDI inputs: ";
        message += std::to_wstring(dl_midi_source_count(handle_));
        message += L".\n\nNative Win32 build using the shared C vocal DSP.";

        const int result = MessageBoxW(hwnd_, message.c_str(), L"Delay Lama Standalone",
                                       MB_YESNO | MB_ICONINFORMATION);
        if (result == IDYES) {
            const std::wstring manual = resource_dir_ + L"\\DelayLama-Original-Manual.pdf";
            if (file_exists(manual))
                ShellExecuteW(hwnd_, L"open", manual.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    DelayLamaHandle *handle_ = nullptr;
    AudioEngine audio_;
    std::wstring resource_dir_;
    std::unique_ptr<Image> background_;
    std::unique_ptr<Image> faces_;

    DragMode drag_mode_ = DragMode::None;
    int drag_start_x_ = 0;
    int drag_start_y_ = 0;
    float drag_start_value_ = 0.0f;
    float glide_ = 0.5f;
    float voice_ = 0.5f;
    float delay_ = 0.42f;
    float xy_x_ = 0.5f;
    float xy_y_ = 0.5f;
    bool key_pressed_[256] = {};
    uint8_t key_notes_[256] = {};
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int command_show) {
    SetProcessDPIAware();

    Gdiplus::GdiplusStartupInput gdiplus_input;
    ULONG gdiplus_token = 0;
    if (Gdiplus::GdiplusStartup(&gdiplus_token, &gdiplus_input, nullptr) != Gdiplus::Ok) {
        MessageBoxW(nullptr, L"Could not initialise Windows graphics.", L"Delay Lama Standalone",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    DelayLamaHandle *handle = dl_create(48000.0f);
    if (!handle) {
        MessageBoxW(nullptr, L"Could not initialise the vocal synthesis engine.",
                    L"Delay Lama Standalone", MB_OK | MB_ICONERROR);
        Gdiplus::GdiplusShutdown(gdiplus_token);
        return 1;
    }

    int exit_code = 0;
    {
        Win32App app(handle);
        if (!app.load_resources()) {
            MessageBoxW(nullptr,
                        L"The Windows build could not find background.png and faces.png in its "
                        L"Resources folder.",
                        L"Delay Lama Standalone", MB_OK | MB_ICONERROR);
            exit_code = 1;
        } else if (!app.create_window(instance, command_show)) {
            MessageBoxW(nullptr, L"Could not create the Delay Lama window.",
                        L"Delay Lama Standalone", MB_OK | MB_ICONERROR);
            exit_code = 1;
        } else if (!app.start_audio()) {
            MessageBoxW(nullptr,
                        L"Could not open the default Windows audio device. Check that an output "
                        L"device is available and try again.",
                        L"Delay Lama Standalone", MB_OK | MB_ICONERROR);
            app.destroy_window();
            exit_code = 1;
        } else {
            MSG message = {};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            exit_code = static_cast<int>(message.wParam);
        }
    }

    dl_destroy(handle);
    Gdiplus::GdiplusShutdown(gdiplus_token);
    return exit_code;
}
