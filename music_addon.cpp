// Simple in-game music player as a ReShade add-on.
// Needs: ReShade headers (reshade.hpp), imgui.h (from ReShade repo deps), miniaudio.h
// Build as 64-bit DLL, rename output to music.addon64 (32-bit games: .addon32)

#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#define NOMINMAX
#include <imgui.h>
#include <reshade.hpp>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static ma_engine g_engine;
static bool g_engine_ok = false;
static ma_sound g_sound;
static bool g_loaded = false;
static bool g_inited = false;

static std::vector<fs::path> g_songs;
static int g_cur = -1;
static float g_vol = 0.7f;
static bool g_paused = false;
static bool g_shuffle = false;
static std::mt19937 g_rng{ std::random_device{}() };

// ---------- helpers ----------
static fs::path exe_dir()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

static fs::path music_dir()
{
    // Put the full path of your music folder in music_path.txt (next to the game exe)
    // Otherwise it uses a folder called "music" next to the game exe.
    fs::path cfg = exe_dir() / "music_path.txt";
    std::ifstream f(cfg);
    std::string line;
    if (f && std::getline(f, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
            line.pop_back();
        if (!line.empty())
            return fs::u8path(line);
    }
    return exe_dir() / "music";
}

static void scan_songs()
{
    g_songs.clear();
    std::error_code ec;
    fs::path dir = music_dir();
    if (!fs::exists(dir, ec)) return;
    for (auto &e : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec))
    {
        if (!e.is_regular_file()) continue;
        std::wstring ext = e.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (ext == L".mp3" || ext == L".flac" || ext == L".wav")
            g_songs.push_back(e.path());
    }
    std::sort(g_songs.begin(), g_songs.end());
}

static void stop_current()
{
    if (g_loaded)
    {
        ma_sound_uninit(&g_sound);
        g_loaded = false;
    }
}

static void play_index(int idx)
{
    if (!g_engine_ok || g_songs.empty()) return;
    idx = (idx % (int)g_songs.size() + (int)g_songs.size()) % (int)g_songs.size();
    stop_current();
    if (ma_sound_init_from_file_w(&g_engine, g_songs[idx].c_str(), MA_SOUND_FLAG_STREAM,
                                  nullptr, nullptr, &g_sound) != MA_SUCCESS)
    {
        // Broken file: skip to the next one (but avoid infinite loop if all fail)
        g_cur = idx;
        return;
    }
    g_loaded = true;
    g_cur = idx;
    g_paused = false;
    ma_sound_set_volume(&g_sound, 1.0f);
    ma_sound_start(&g_sound);
}

static void next_song()
{
    if (g_songs.empty()) return;
    if (g_shuffle && g_songs.size() > 1)
    {
        int n;
        do { n = (int)(g_rng() % g_songs.size()); } while (n == g_cur);
        play_index(n);
    }
    else
        play_index(g_cur + 1);
}

static void prev_song() { play_index(g_cur - 1); }

static void toggle_pause()
{
    if (!g_loaded) { play_index(g_cur < 0 ? 0 : g_cur); return; }
    if (g_paused) { ma_sound_start(&g_sound); g_paused = false; }
    else          { ma_sound_stop(&g_sound);  g_paused = true;  }
}

static void set_volume(float v)
{
    g_vol = std::clamp(v, 0.0f, 1.0f);
    if (g_engine_ok) ma_engine_set_volume(&g_engine, g_vol);
}

static float cur_seconds()
{
    if (!g_loaded) return 0.f;
    ma_uint64 c = 0;
    ma_sound_get_cursor_in_pcm_frames(&g_sound, &c);
    return (float)c / (float)ma_engine_get_sample_rate(&g_engine);
}

static float len_seconds()
{
    if (!g_loaded) return 0.f;
    ma_uint64 l = 0;
    ma_sound_get_length_in_pcm_frames(&g_sound, &l);
    return (float)l / (float)ma_engine_get_sample_rate(&g_engine);
}

static void seek_seconds(float s)
{
    if (!g_loaded) return;
    ma_sound_seek_to_pcm_frame(&g_sound, (ma_uint64)(std::max(0.f, s) * ma_engine_get_sample_rate(&g_engine)));
}

static std::string fmt_time(float s)
{
    int t = (int)s;
    char b[16];
    snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}

static void ensure_init()
{
    if (g_inited) return;
    g_inited = true;
    if (ma_engine_init(nullptr, &g_engine) == MA_SUCCESS)
    {
        g_engine_ok = true;
        ma_engine_set_volume(&g_engine, g_vol);
        scan_songs();
        if (!g_songs.empty()) play_index(0);
    }
}

// ---------- hotkeys (work with the menu closed) ----------
// F5 vol down | F6 vol up | F7 previous | F8 play/pause | F9 next
static bool game_is_focused()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static bool key_pressed_once(int vk)
{
    static bool was_down[256] = {};
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool fire = down && !was_down[vk];
    was_down[vk] = down;
    return fire;
}

static void on_present(reshade::api::effect_runtime *)
{
    ensure_init();
    if (!g_engine_ok) return;

    // Auto-advance when a song ends
    if (g_loaded && !g_paused && ma_sound_at_end(&g_sound))
        next_song();

    bool focused = game_is_focused();
    bool f5 = key_pressed_once(VK_F5), f6 = key_pressed_once(VK_F6), f7 = key_pressed_once(VK_F7),
         f8 = key_pressed_once(VK_F8), f9 = key_pressed_once(VK_F9);
    if (!focused) return;
    if (f5) set_volume(g_vol - 0.05f);
    if (f6) set_volume(g_vol + 0.05f);
    if (f7) prev_song();
    if (f8) toggle_pause();
    if (f9) next_song();
}

// ---------- menu ----------
static void draw_ui(reshade::api::effect_runtime *)
{
    ensure_init();
    if (!g_engine_ok) { ImGui::TextUnformatted("Audio engine failed to start."); return; }

    if (g_songs.empty())
    {
        ImGui::TextUnformatted("No songs found. Put MP3/FLAC/WAV files in:");
        ImGui::TextUnformatted(music_dir().u8string().c_str());
        ImGui::TextUnformatted("(or write your folder path in music_path.txt next to the game exe)");
        if (ImGui::Button("Rescan")) { scan_songs(); if (!g_songs.empty()) play_index(0); }
        return;
    }

    // Now playing
    std::string name = (g_cur >= 0) ? g_songs[g_cur].stem().u8string() : "-";
    ImGui::Text("Now playing: %s", name.c_str());

    // Seek bar (click anywhere or drag to jump)
    static bool seeking = false;
    static float seek_pos = 0.f;
    float len = std::max(len_seconds(), 1.0f);
    float pos = seeking ? seek_pos : cur_seconds();
    ImGui::SetNextItemWidth(-110.0f);
    if (ImGui::SliderFloat("##seek", &pos, 0.0f, len, ""))
    {
        seeking = true;
        seek_pos = pos;
    }
    if (seeking && ImGui::IsItemDeactivated())
    {
        seek_seconds(seek_pos);
        seeking = false;
    }
    ImGui::SameLine();
    ImGui::Text("%s / %s", fmt_time(pos).c_str(), fmt_time(len_seconds()).c_str());

    // Controls
    if (ImGui::Button("<< Prev")) prev_song();
    ImGui::SameLine();
    if (ImGui::Button(g_paused || !g_loaded ? "  Play  " : " Pause  ")) toggle_pause();
    ImGui::SameLine();
    if (ImGui::Button("Next >>")) next_song();
    ImGui::SameLine();
    if (ImGui::Button("-10s")) seek_seconds(cur_seconds() - 10.f);
    ImGui::SameLine();
    if (ImGui::Button("+10s")) seek_seconds(cur_seconds() + 10.f);
    ImGui::SameLine();
    ImGui::Checkbox("Shuffle", &g_shuffle);

    // Volume
    float v = g_vol * 100.f;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##vol", &v, 0.f, 100.f, "Volume %.0f%%")) set_volume(v / 100.f);

    ImGui::Separator();
    ImGui::Text("Songs (%d)", (int)g_songs.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Rescan folder"))
    {
        std::wstring cur_path = (g_cur >= 0 && g_cur < (int)g_songs.size()) ? g_songs[g_cur].wstring() : L"";
        scan_songs();
        g_cur = -1;
        for (int i = 0; i < (int)g_songs.size(); ++i)
            if (g_songs[i].wstring() == cur_path) g_cur = i;
    }

    ImGui::BeginChild("##songlist", ImVec2(0, 300), true);
    for (int i = 0; i < (int)g_songs.size(); ++i)
    {
        ImGui::PushID(i);
        std::string label = g_songs[i].stem().u8string();
        if (ImGui::Selectable(label.c_str(), i == g_cur))
            play_index(i);
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::TextDisabled("Hotkeys (menu closed): F5/F6 volume, F7 prev, F8 play/pause, F9 next");
}

// ---------- add-on entry ----------
extern "C" __declspec(dllexport) const char *NAME = "Music Player";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Offline music player with in-game menu and hotkeys.";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID lpReserved)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(hModule))
            return FALSE;
        reshade::register_overlay("Music", draw_ui);
        reshade::register_event<reshade::addon_event::reshade_present>(on_present);
        break;
    case DLL_PROCESS_DETACH:
        if (lpReserved == nullptr) // skip heavy cleanup if the whole process is dying
        {
            stop_current();
            if (g_engine_ok) ma_engine_uninit(&g_engine);
        }
        reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}
