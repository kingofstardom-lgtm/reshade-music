// In-game music player + notes as a ReShade add-on.
// Needs: ReShade headers (reshade.hpp), imgui.h (from ReShade repo deps), miniaudio.h
// Build as 64-bit DLL, rename output to music.addon64

#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#define NOMINMAX
#include <imgui.h>
#include <reshade.hpp>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static ma_engine g_engine;
static bool g_engine_ok = false;
static ma_sound g_sound;
static bool g_loaded = false;
static bool g_inited = false;

static std::vector<fs::path> g_songs;
static std::vector<std::string> g_names;   // display names (file name without extension)
static std::vector<std::string> g_lower;   // lowercase names, used by search
static std::vector<std::string> g_paths;   // full paths as UTF-8, used by the playlist
static std::set<std::string> g_sel;        // songs chosen for "My playlist"
static bool g_use_playlist = false;        // false = all songs, true = my playlist
static char g_search[128] = {};

static int g_cur = -1;
static float g_vol = 0.7f;
static bool g_paused = false;
static bool g_shuffle = false;
static int g_repeat = 0; // 0 = loop list, 1 = loop this song, 2 = play list once then stop, 3 = stop after this song
static int g_fail_skips = 0;
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

static std::string lower_str(std::string s)
{
    for (auto &c : s) c = (char)::tolower((unsigned char)c);
    return s;
}

static void scan_songs()
{
    g_songs.clear();
    std::error_code ec;
    fs::path dir = music_dir();
    if (fs::exists(dir, ec))
    {
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
    g_names.clear(); g_lower.clear(); g_paths.clear();
    for (auto &p : g_songs)
    {
        std::string n = p.stem().u8string();
        g_names.push_back(n);
        g_lower.push_back(lower_str(n));
        g_paths.push_back(p.u8string());
    }
}

static void rescan()
{
    std::wstring cur_path = (g_cur >= 0 && g_cur < (int)g_songs.size()) ? g_songs[g_cur].wstring() : L"";
    scan_songs();
    g_cur = -1;
    for (int i = 0; i < (int)g_songs.size(); ++i)
        if (g_songs[i].wstring() == cur_path) g_cur = i;
}

// ---------- playlist (saved per game in playlist.txt) ----------
static fs::path playlist_path() { return exe_dir() / "playlist.txt"; }

static void load_playlist()
{
    g_sel.clear();
    std::ifstream f(playlist_path());
    std::string line;
    while (std::getline(f, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        if (line == "#MODE=PLAYLIST") g_use_playlist = true;
        else if (line == "#MODE=ALL") g_use_playlist = false;
        else g_sel.insert(line);
    }
}

static void save_playlist()
{
    std::ofstream f(playlist_path(), std::ios::trunc);
    if (!f) return;
    f << (g_use_playlist ? "#MODE=PLAYLIST" : "#MODE=ALL") << "\n";
    for (auto &s : g_sel) f << s << "\n";
}

static int playlist_count()
{
    int n = 0;
    for (auto &p : g_paths) if (g_sel.count(p)) ++n;
    return n;
}

// The songs that Next/Prev/auto-advance move through
static std::vector<int> active_queue()
{
    std::vector<int> q;
    if (g_use_playlist)
        for (int i = 0; i < (int)g_songs.size(); ++i)
            if (g_sel.count(g_paths[i])) q.push_back(i);
    if (q.empty())
        for (int i = 0; i < (int)g_songs.size(); ++i) q.push_back(i);
    return q;
}

// ---------- playback ----------
static void stop_current()
{
    if (g_loaded)
    {
        ma_sound_uninit(&g_sound);
        g_loaded = false;
    }
}

static void next_song();

static void play_index(int idx)
{
    if (!g_engine_ok || g_songs.empty()) return;
    idx = (idx % (int)g_songs.size() + (int)g_songs.size()) % (int)g_songs.size();
    stop_current();
    if (ma_sound_init_from_file_w(&g_engine, g_songs[idx].c_str(), MA_SOUND_FLAG_STREAM,
                                  nullptr, nullptr, &g_sound) != MA_SUCCESS)
    {
        // Unreadable file: skip to the next one (bounded, so it can't loop forever)
        g_cur = idx;
        if (++g_fail_skips < (int)g_songs.size()) next_song();
        else g_fail_skips = 0;
        return;
    }
    g_fail_skips = 0;
    g_loaded = true;
    g_cur = idx;
    g_paused = false;
    ma_sound_set_volume(&g_sound, 1.0f);
    ma_sound_start(&g_sound);
}

static void next_song()
{
    auto q = active_queue();
    if (q.empty()) return;
    if (g_shuffle && q.size() > 1)
    {
        int n;
        do { n = q[g_rng() % q.size()]; } while (n == g_cur);
        play_index(n);
        return;
    }
    auto it = std::find(q.begin(), q.end(), g_cur);
    if (it == q.end() || ++it == q.end()) play_index(q.front());
    else play_index(*it);
}

static void prev_song()
{
    auto q = active_queue();
    if (q.empty()) return;
    auto it = std::find(q.begin(), q.end(), g_cur);
    if (it == q.end() || it == q.begin()) play_index(q.back());
    else play_index(*(it - 1));
}

static void toggle_pause()
{
    if (!g_loaded)
    {
        auto q = active_queue();
        if (!q.empty()) play_index(g_cur < 0 ? q.front() : g_cur);
        return;
    }
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
        load_playlist();
        scan_songs();
        auto q = active_queue();
        if (!q.empty()) play_index(q.front());
    }
}

// ---------- notes (saved per game in game_notes.txt) ----------
static const size_t NOTES_SIZE = 65536;
static char g_notes[NOTES_SIZE] = {};
static bool g_notes_loaded = false;
static bool g_notes_dirty = false;
static ULONGLONG g_notes_last_change = 0;

static fs::path notes_path() { return exe_dir() / "game_notes.txt"; }

static void ensure_notes_loaded()
{
    if (g_notes_loaded) return;
    g_notes_loaded = true;
    std::ifstream f(notes_path(), std::ios::binary);
    if (!f) return;
    f.read(g_notes, NOTES_SIZE - 1);
    g_notes[f.gcount()] = '\0';
}

static void save_notes()
{
    std::ofstream f(notes_path(), std::ios::binary | std::ios::trunc);
    if (f) f.write(g_notes, (std::streamsize)strlen(g_notes));
    g_notes_dirty = false;
}

static void notes_autosave_tick()
{
    // Save about 1 second after the last edit
    if (g_notes_dirty && GetTickCount64() - g_notes_last_change > 1000)
        save_notes();
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
    ensure_notes_loaded();
    notes_autosave_tick();
    if (!g_engine_ok) return;

    // When a song ends: repeat it, or move on, or stop (depending on the Repeat button)
    if (g_loaded && !g_paused && ma_sound_at_end(&g_sound))
    {
        if (g_repeat == 1)
        {
            ma_sound_seek_to_pcm_frame(&g_sound, 0);
            ma_sound_start(&g_sound);
        }
        else
        {
            bool stop_now = false;
            if (g_repeat == 3)
                stop_now = true; // stop after this song
            else if (g_repeat == 2 && !g_shuffle)
            {
                auto q = active_queue();
                stop_now = !q.empty() && g_cur == q.back(); // end of the list
            }
            if (stop_now)
            {
                ma_sound_stop(&g_sound);
                ma_sound_seek_to_pcm_frame(&g_sound, 0); // so Play starts it again from the beginning
                g_paused = true;
            }
            else
                next_song();
        }
    }

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

// ---------- menu: Player tab ----------
static void draw_player()
{
    if (!g_engine_ok) { ImGui::TextUnformatted("Audio engine failed to start."); return; }

    if (g_songs.empty())
    {
        ImGui::TextUnformatted("No songs found. Put MP3/FLAC/WAV files in:");
        ImGui::TextUnformatted(music_dir().u8string().c_str());
        ImGui::TextUnformatted("(or write your folder path in music_path.txt next to the game exe)");
        if (ImGui::Button("Rescan"))
        {
            rescan();
            auto q = active_queue();
            if (!g_songs.empty() && !q.empty()) play_index(q.front());
        }
        return;
    }

    // Now playing
    std::string name = (g_cur >= 0 && g_cur < (int)g_names.size()) ? g_names[g_cur] : "-";
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

    const char *repeat_labels[4] = { "Loop playlist", "Loop this song", "Play list once, then stop", "Stop after this song" };
    if (ImGui::Button(repeat_labels[g_repeat])) g_repeat = (g_repeat + 1) % 4;
    ImGui::SameLine();
    ImGui::TextDisabled("(click to change what happens when a song ends)");

    // Volume
    float v = g_vol * 100.f;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##vol", &v, 0.f, 100.f, "Volume %.0f%%")) set_volume(v / 100.f);

    ImGui::Separator();

    // Which songs to play: everything, or only the playlist you picked
    bool mode_changed = false;
    if (ImGui::RadioButton("All songs", !g_use_playlist)) { g_use_playlist = false; mode_changed = true; }
    ImGui::SameLine();
    if (ImGui::RadioButton("My playlist", g_use_playlist)) { g_use_playlist = true; mode_changed = true; }
    ImGui::SameLine();
    ImGui::Text("(%d chosen)", playlist_count());
    ImGui::SameLine();
    if (ImGui::SmallButton("Rescan folder")) rescan();
    if (mode_changed) save_playlist();

    // Search
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##search", "Search songs...", g_search, sizeof g_search);
    std::string q = lower_str(g_search);

    if (ImGui::SmallButton("Add all shown to playlist"))
    {
        for (int i = 0; i < (int)g_songs.size(); ++i)
            if (q.empty() || g_lower[i].find(q) != std::string::npos)
                g_sel.insert(g_paths[i]);
        save_playlist();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear playlist"))
    {
        g_sel.clear();
        save_playlist();
    }

    if (g_use_playlist && playlist_count() == 0)
        ImGui::TextDisabled("Playlist is empty. Pick 'All songs' and tick the boxes next to the songs you want.");
    else
        ImGui::TextDisabled("Tick the box next to a song to add it to your playlist. Click a name to play it.");

    ImGui::BeginChild("##songlist", ImVec2(0, 300), true);
    for (int i = 0; i < (int)g_songs.size(); ++i)
    {
        bool in_pl = g_sel.count(g_paths[i]) > 0;
        if (g_use_playlist && !in_pl) continue;
        if (!q.empty() && g_lower[i].find(q) == std::string::npos) continue;

        ImGui::PushID(i);
        bool chk = in_pl;
        if (ImGui::Checkbox("##pl", &chk))
        {
            if (chk) g_sel.insert(g_paths[i]);
            else g_sel.erase(g_paths[i]);
            save_playlist();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add to / remove from my playlist");
        ImGui::SameLine();
        if (ImGui::Selectable(g_names[i].c_str(), i == g_cur))
            play_index(i);
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::TextDisabled("Hotkeys (menu closed): F5/F6 volume, F7 prev, F8 play/pause, F9 next");
}

// ---------- menu: Notes tab ----------
static void draw_notes_tab()
{
    ensure_notes_loaded();
    ImGui::TextUnformatted("Notes for this game (saved automatically in game_notes.txt)");
    if (ImGui::InputTextMultiline("##notes", g_notes, NOTES_SIZE, ImVec2(-1.0f, 420.0f),
                                  ImGuiInputTextFlags_AllowTabInput))
    {
        g_notes_dirty = true;
        g_notes_last_change = GetTickCount64();
    }
    ImGui::TextDisabled(g_notes_dirty ? "Saving..." : "Saved");
}

// ---------- one window with two inner tabs ----------
static void draw_ui(reshade::api::effect_runtime *)
{
    ensure_init();
    if (ImGui::BeginTabBar("##music_tabs"))
    {
        if (ImGui::BeginTabItem("Player"))
        {
            draw_player();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Notes"))
        {
            draw_notes_tab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// ---------- add-on entry ----------
extern "C" __declspec(dllexport) const char *NAME = "Music Player";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Offline music player (search, playlist, repeat) and per-game notes.";

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
        if (g_notes_dirty) save_notes();
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
