// In-game music player + notes as a ReShade add-on.
// Needs: ReShade headers (reshade.hpp), imgui.h (from ReShade repo deps), miniaudio.h
// Build as 64-bit DLL, rename output to music.addon64
// Lyrics overlay settings are saved per game in lyrics_settings.txt (next to the game exe).

#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#define NOMINMAX
#include <imgui.h>
#include <reshade.hpp>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <locale>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <cwctype>

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
static void lyrics_load_for(const fs::path &song);

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
    lyrics_load_for(g_songs[idx]);
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

// ======================================================================
// ---------- LYRICS: reading (embedded tags + .lrc files) ----------
// Lyrics are read from the song's own tag (MP3: USLT frame, FLAC: LYRICS comment).
// If the song has none, a .lrc file with the same name next to it is used.
// If the text contains timestamps like [01:23.45] it is "synced", otherwise it is plain text.
// ==== LYRICS PARSER BEGIN ====
struct LyLine { float t; std::string text; };
static std::vector<LyLine> g_ly;
static bool g_ly_synced = false;
static std::string g_ly_source;
static float g_ly_scroll = 0.f;

static std::string ly_trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

static void ly_put_utf8(std::string &o, uint32_t cp)
{
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800)
    {
        o += (char)(0xC0 | (cp >> 6));
        o += (char)(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        o += (char)(0xE0 | (cp >> 12));
        o += (char)(0x80 | ((cp >> 6) & 0x3F));
        o += (char)(0x80 | (cp & 0x3F));
    }
    else
    {
        o += (char)(0xF0 | (cp >> 18));
        o += (char)(0x80 | ((cp >> 12) & 0x3F));
        o += (char)(0x80 | ((cp >> 6) & 0x3F));
        o += (char)(0x80 | (cp & 0x3F));
    }
}

// ID3 text encodings: 0 = Latin-1, 1 = UTF-16 with BOM, 2 = UTF-16BE, 3 = UTF-8
static std::string ly_decode(int enc, const unsigned char *p, size_t n)
{
    std::string o;
    if (enc == 1 || enc == 2)
    {
        bool be = (enc == 2);
        size_t i = 0;
        if (enc == 1 && n >= 2)
        {
            if (p[0] == 0xFF && p[1] == 0xFE) { be = false; i = 2; }
            else if (p[0] == 0xFE && p[1] == 0xFF) { be = true; i = 2; }
        }
        for (; i + 1 < n; i += 2)
        {
            uint32_t u = be ? (uint32_t)((p[i] << 8) | p[i + 1]) : (uint32_t)((p[i + 1] << 8) | p[i]);
            if (u >= 0xD800 && u < 0xDC00 && i + 3 < n)
            {
                uint32_t u2 = be ? (uint32_t)((p[i + 2] << 8) | p[i + 3]) : (uint32_t)((p[i + 3] << 8) | p[i + 2]);
                if (u2 >= 0xDC00 && u2 < 0xE000)
                {
                    u = 0x10000 + ((u - 0xD800) << 10) + (u2 - 0xDC00);
                    i += 2;
                }
            }
            ly_put_utf8(o, u);
        }
    }
    else if (enc == 3)
    {
        o.assign((const char *)p, n);
        if (o.size() >= 3 && (unsigned char)o[0] == 0xEF && (unsigned char)o[1] == 0xBB && (unsigned char)o[2] == 0xBF)
            o.erase(0, 3);
    }
    else
    {
        for (size_t i = 0; i < n; ++i) ly_put_utf8(o, p[i]);
    }
    while (!o.empty() && o.back() == '\0') o.pop_back();
    return o;
}

static uint32_t ly_syncsafe(const unsigned char *b)
{
    return ((uint32_t)(b[0] & 0x7F) << 21) | ((uint32_t)(b[1] & 0x7F) << 14) |
           ((uint32_t)(b[2] & 0x7F) << 7) | (uint32_t)(b[3] & 0x7F);
}
static uint32_t ly_be32(const unsigned char *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}
static uint32_t ly_le32(const unsigned char *b)
{
    return ((uint32_t)b[3] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[1] << 8) | (uint32_t)b[0];
}

static void ly_deunsync(std::vector<unsigned char> &d)
{
    size_t w = 0;
    for (size_t r = 0; r < d.size(); ++r)
    {
        d[w++] = d[r];
        if (d[r] == 0xFF && r + 1 < d.size() && d[r + 1] == 0x00) ++r;
    }
    d.resize(w);
}

static bool ly_looks_synced(const std::string &s)
{
    for (size_t i = 0; i + 3 < s.size(); ++i)
    {
        if (s[i] == '[' && isdigit((unsigned char)s[i + 1]))
        {
            size_t j = i + 1;
            while (j < s.size() && isdigit((unsigned char)s[j])) ++j;
            if (j < s.size() && s[j] == ':') return true;
        }
    }
    return false;
}

// USLT frame body: encoding(1) language(3) descriptor(terminated) text
static std::string ly_uslt_text(const unsigned char *b, size_t n)
{
    if (n < 5) return "";
    int enc = b[0];
    if (enc > 3) enc = 0;
    size_t i = 4;
    if (enc == 1 || enc == 2)
    {
        while (i + 1 < n && !(b[i] == 0 && b[i + 1] == 0)) i += 2;
        i += 2;
    }
    else
    {
        while (i < n && b[i] != 0) ++i;
        i += 1;
    }
    if (i >= n) return "";
    return ly_decode(enc, b + i, n - i);
}

static std::string ly_id3(const fs::path &p)
{
    std::ifstream f(p, std::ios::binary);
    unsigned char h[10];
    if (!f.read((char *)h, 10) || memcmp(h, "ID3", 3) != 0) return "";
    int ver = h[3];
    if (ver < 2 || ver > 4) return "";
    uint32_t tsize = ly_syncsafe(h + 6);
    if (tsize == 0 || tsize > (32u << 20)) return "";
    std::vector<unsigned char> d(tsize);
    f.read((char *)d.data(), (std::streamsize)tsize);
    d.resize((size_t)f.gcount());
    if (ver < 4 && (h[5] & 0x80)) ly_deunsync(d);

    size_t pos = 0;
    if (ver >= 3 && (h[5] & 0x40) && d.size() >= 4)
        pos = (ver == 4) ? (size_t)ly_syncsafe(d.data()) : (size_t)ly_be32(d.data()) + 4;
    const size_t hdr = (ver == 2) ? 6 : 10;
    std::string first;
    while (pos + hdr <= d.size())
    {
        const unsigned char *fh = d.data() + pos;
        if (fh[0] == 0) break;
        std::string id((const char *)fh, ver == 2 ? 3 : 4);
        size_t fsz = (ver == 2) ? (size_t)((fh[3] << 16) | (fh[4] << 8) | fh[5])
                   : (ver == 4) ? (size_t)ly_syncsafe(fh + 4)
                                : (size_t)ly_be32(fh + 4);
        unsigned fl = (ver == 2) ? 0u : (unsigned)((fh[8] << 8) | fh[9]);
        size_t b0 = pos + hdr;
        if (fsz > d.size() - b0) break;
        if (id == "USLT" || id == "ULT")
        {
            std::vector<unsigned char> body(d.begin() + b0, d.begin() + b0 + fsz);
            if (ver == 4)
            {
                if (fl & 0x0001) body.erase(body.begin(), body.begin() + std::min<size_t>(4, body.size()));
                if (fl & 0x0002) ly_deunsync(body);
            }
            std::string t = ly_uslt_text(body.data(), body.size());
            if (!ly_trim(t).empty())
            {
                if (ly_looks_synced(t)) return t;
                if (first.empty()) first = t;
            }
        }
        pos = b0 + fsz;
    }
    return first;
}

static std::string ly_flac(const fs::path &p)
{
    std::ifstream f(p, std::ios::binary);
    char m[4];
    if (!f.read(m, 4) || memcmp(m, "fLaC", 4) != 0) return "";
    std::string first;
    for (int guard = 0; guard < 4096; ++guard)
    {
        unsigned char bh[4];
        if (!f.read((char *)bh, 4)) break;
        bool last = (bh[0] & 0x80) != 0;
        int type = bh[0] & 0x7F;
        uint32_t len = ((uint32_t)bh[1] << 16) | ((uint32_t)bh[2] << 8) | (uint32_t)bh[3];
        if (type == 4) // VORBIS_COMMENT
        {
            if (len > (32u << 20)) break;
            std::vector<unsigned char> b(len);
            f.read((char *)b.data(), (std::streamsize)len);
            if ((uint32_t)f.gcount() != len) break;
            if (len < 8) break;
            uint32_t vl = ly_le32(b.data());
            if (vl > len - 8) break;
            size_t pos = 4 + (size_t)vl;
            uint32_t cnt = ly_le32(&b[pos]);
            pos += 4;
            for (uint32_t k = 0; k < cnt && pos + 4 <= len; ++k)
            {
                uint32_t l = ly_le32(&b[pos]);
                pos += 4;
                if (l > len - pos) break;
                std::string s((const char *)&b[pos], l);
                pos += l;
                size_t eq = s.find('=');
                if (eq == std::string::npos) continue;
                std::string key = s.substr(0, eq);
                for (auto &c : key) c = (char)toupper((unsigned char)c);
                if (key == "LYRICS" || key == "UNSYNCEDLYRICS" || key == "SYNCEDLYRICS" || key == "LYRIC")
                {
                    std::string v = s.substr(eq + 1);
                    if (!ly_trim(v).empty())
                    {
                        if (ly_looks_synced(v)) return v;
                        if (first.empty()) first = v;
                    }
                }
            }
        }
        else
            f.seekg(len, std::ios::cur);
        if (last) break;
    }
    return first;
}

static std::string ly_read_text_file(const fs::path &p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.size() > (4u << 20)) s.resize(4u << 20);
    if (s.size() >= 2 && (unsigned char)s[0] == 0xFF && (unsigned char)s[1] == 0xFE)
        return ly_decode(1, (const unsigned char *)s.data(), s.size());
    if (s.size() >= 2 && (unsigned char)s[0] == 0xFE && (unsigned char)s[1] == 0xFF)
        return ly_decode(1, (const unsigned char *)s.data(), s.size());
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        s.erase(0, 3);
    return s;
}

// "mm:ss", "mm:ss.xx" or "mm:ss:xx"
static bool ly_parse_ts(const std::string &tag, float &out)
{
    if (tag.empty() || !isdigit((unsigned char)tag[0])) return false;
    size_t c = tag.find(':');
    if (c == std::string::npos) return false;
    for (char ch : tag)
        if (!(isdigit((unsigned char)ch) || ch == ':' || ch == '.')) return false;
    float m = (float)atoi(tag.c_str());
    std::string rest = tag.substr(c + 1);
    size_t c2 = rest.find(':');
    if (c2 != std::string::npos) rest[c2] = '.';
    out = m * 60.f + (float)atof(rest.c_str());
    return true;
}

static bool ly_is_meta(const std::string &tag)
{
    static const char *keys[] = { "ar", "ti", "al", "au", "by", "re", "ve", "length", "offset", "la", "id", "lr", "tool" };
    size_t c = tag.find(':');
    if (c == std::string::npos || c == 0) return false;
    std::string k = tag.substr(0, c);
    for (auto &ch : k) ch = (char)tolower((unsigned char)ch);
    for (auto *key : keys)
        if (k == key) return true;
    return false;
}

// removes enhanced-LRC word stamps like <00:12.30>
static std::string ly_strip_word_tags(const std::string &s)
{
    std::string o;
    for (size_t i = 0; i < s.size();)
    {
        if (s[i] == '<')
        {
            size_t e = s.find('>', i);
            float t;
            if (e != std::string::npos && ly_parse_ts(s.substr(i + 1, e - i - 1), t)) { i = e + 1; continue; }
        }
        o += s[i++];
    }
    return o;
}

static void ly_parse(const std::string &text)
{
    g_ly.clear();
    g_ly_synced = false;
    std::vector<LyLine> timed, plain;
    float offset = 0.f; // seconds; positive = lyrics appear earlier (LRC standard)
    size_t start = 0;
    while (start <= text.size())
    {
        size_t e = text.find('\n', start);
        std::string line = text.substr(start, e == std::string::npos ? std::string::npos : e - start);
        start = (e == std::string::npos) ? text.size() + 1 : e + 1;
        while (!line.empty() && line.back() == '\r') line.pop_back();

        size_t i = 0;
        std::vector<float> times;
        bool meta = false;
        while (i < line.size() && line[i] == '[')
        {
            size_t c = line.find(']', i);
            if (c == std::string::npos) break;
            std::string tag = line.substr(i + 1, c - i - 1);
            float t;
            if (ly_parse_ts(tag, t)) times.push_back(t);
            else if (ly_is_meta(tag))
            {
                meta = true;
                std::string lt = tag;
                for (auto &ch : lt) ch = (char)tolower((unsigned char)ch);
                if (lt.compare(0, 7, "offset:") == 0) offset = (float)atof(tag.c_str() + 7) / 1000.f;
            }
            else break; // something like [Chorus]: keep it as normal text
            i = c + 1;
        }
        std::string rest = ly_trim(ly_strip_word_tags(line.substr(i)));
        if (!times.empty())
        {
            for (float t : times) timed.push_back({ t, rest });
        }
        else if (!meta)
            plain.push_back({ 0.f, rest });
    }

    if (!timed.empty())
    {
        std::stable_sort(timed.begin(), timed.end(), [](const LyLine &a, const LyLine &b) { return a.t < b.t; });
        for (auto &l : timed) l.t -= offset;
        g_ly = timed;
        g_ly_synced = true;
    }
    else
    {
        while (!plain.empty() && plain.front().text.empty()) plain.erase(plain.begin());
        while (!plain.empty() && plain.back().text.empty()) plain.pop_back();
        g_ly = plain;
    }
}

static void lyrics_load_for(const fs::path &song)
{
    g_ly.clear();
    g_ly_synced = false;
    g_ly_source.clear();
    g_ly_scroll = 0.f;
    try
    {
        std::wstring ext = song.extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        std::string text;
        if (ext == L".mp3") text = ly_id3(song);
        else if (ext == L".flac") text = ly_flac(song);
        if (!ly_trim(text).empty()) g_ly_source = "embedded tag";
        else
        {
            fs::path l = song;
            l.replace_extension(L".lrc");
            text = ly_read_text_file(l);
            if (!ly_trim(text).empty()) g_ly_source = ".lrc file";
            else text.clear();
        }
        if (!text.empty()) ly_parse(text);
    }
    catch (...)
    {
        g_ly.clear();
        g_ly_source.clear();
    }
    if (g_ly.empty()) g_ly_source.clear();
}
// ==== LYRICS PARSER END ====

// ---------- LYRICS: style + settings (saved per game in lyrics_settings.txt) ----------
struct LyStyle
{
    float cur[4]  = { 1.00f, 0.85f, 0.25f, 1.00f }; // current line
    float past[4] = { 1.00f, 1.00f, 1.00f, 0.35f }; // lines already sung
    float next[4] = { 1.00f, 1.00f, 1.00f, 0.70f }; // lines coming up
    float outc[4] = { 0.00f, 0.00f, 0.00f, 0.90f }; // outline colour
    float bgc[4]  = { 0.00f, 0.00f, 0.00f, 0.45f }; // background box colour
    float scale = 1.0f;     // text size when "fit text to box" is off
    int lines = 3;          // visible lines
    int align = 1;          // 0 left, 1 center, 2 right
    float outline = 1.5f;
    float shadow = 2.0f;
    bool use_bg = false;
    float rounding = 10.0f;
    float opacity = 1.0f;
    bool fit_box = true;    // text size follows the box height (resize box = resize text)
    bool fit_width = true;  // shrink a long line so it fits the box width
    float anim = 12.0f;     // scroll animation speed
    bool show_title = true; // song name for the first few seconds
    bool pop = true;        // current line is slightly bigger
};

static LyStyle g_ly_style;
static std::string g_ly_slots[3];
static bool g_ly_enabled = true;
static bool g_ly_edit = false;
static bool g_ly_plain_scroll = true;   // plain (unsynced) lyrics scroll with song progress
static int g_ly_offset_ms = 0;          // + = lyrics appear later
static float g_ly_rect[4] = { 0.25f, 0.76f, 0.50f, 0.17f }; // x, y, w, h as fractions of the screen
static bool g_ly_reset = false;
static bool g_ly_dirty = false;
static ULONGLONG g_ly_last_change = 0;
static unsigned long long g_frame = 0;
static unsigned long long g_ui_frame = 0;
static unsigned long long g_overlay_calls = 0;
static unsigned long long g_overlay_calls_closed = 0;

static void ly_mark_dirty() { g_ly_dirty = true; g_ly_last_change = GetTickCount64(); }

static std::string ly_style_to_str(const LyStyle &s)
{
    std::ostringstream o;
    o.imbue(std::locale::classic());
    auto a = [&](const float *c) { for (int i = 0; i < 4; ++i) o << c[i] << ' '; };
    a(s.cur); a(s.past); a(s.next); a(s.outc); a(s.bgc);
    o << s.scale << ' ' << s.lines << ' ' << s.align << ' ' << s.outline << ' ' << s.shadow << ' '
      << (int)s.use_bg << ' ' << s.rounding << ' ' << s.opacity << ' ' << (int)s.fit_box << ' '
      << (int)s.fit_width << ' ' << s.anim << ' ' << (int)s.show_title << ' ' << (int)s.pop;
    return o.str();
}

static bool ly_style_from_str(const std::string &str, LyStyle &out)
{
    std::istringstream i(str);
    i.imbue(std::locale::classic());
    LyStyle s;
    auto a = [&](float *c) { for (int k = 0; k < 4; ++k) i >> c[k]; };
    int b1, b2, b3, b4, b5;
    a(s.cur); a(s.past); a(s.next); a(s.outc); a(s.bgc);
    i >> s.scale >> s.lines >> s.align >> s.outline >> s.shadow >> b1 >> s.rounding >> s.opacity >> b2 >> b3 >> s.anim >> b4 >> b5;
    if (i.fail()) return false;
    s.use_bg = b1 != 0; s.fit_box = b2 != 0; s.fit_width = b3 != 0; s.show_title = b4 != 0; s.pop = b5 != 0;
    s.lines = std::clamp(s.lines, 1, 9);
    s.align = std::clamp(s.align, 0, 2);
    s.scale = std::clamp(s.scale, 0.3f, 6.f);
    out = s;
    return true;
}

static fs::path ly_settings_path() { return exe_dir() / "lyrics_settings.txt"; }

static void ly_save_settings()
{
    std::ofstream f(ly_settings_path(), std::ios::trunc);
    if (f)
    {
        f.imbue(std::locale::classic());
        f << "enabled=" << (g_ly_enabled ? 1 : 0) << "\n";
        f << "plain_scroll=" << (g_ly_plain_scroll ? 1 : 0) << "\n";
        f << "offset=" << g_ly_offset_ms << "\n";
        f << "rect=" << g_ly_rect[0] << ' ' << g_ly_rect[1] << ' ' << g_ly_rect[2] << ' ' << g_ly_rect[3] << "\n";
        f << "style=" << ly_style_to_str(g_ly_style) << "\n";
        for (int i = 0; i < 3; ++i)
            if (!g_ly_slots[i].empty()) f << "slot" << (i + 1) << "=" << g_ly_slots[i] << "\n";
    }
    g_ly_dirty = false;
}

static void ly_load_settings()
{
    std::ifstream f(ly_settings_path());
    std::string line;
    while (std::getline(f, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "enabled") g_ly_enabled = (v == "1");
        else if (k == "plain_scroll") g_ly_plain_scroll = (v == "1");
        else if (k == "offset") g_ly_offset_ms = std::clamp(atoi(v.c_str()), -5000, 5000);
        else if (k == "rect")
        {
            std::istringstream is(v);
            is.imbue(std::locale::classic());
            float r[4];
            if (is >> r[0] >> r[1] >> r[2] >> r[3])
            {
                g_ly_rect[0] = std::clamp(r[0], -0.5f, 1.5f);
                g_ly_rect[1] = std::clamp(r[1], -0.5f, 1.5f);
                g_ly_rect[2] = std::clamp(r[2], 0.05f, 2.0f);
                g_ly_rect[3] = std::clamp(r[3], 0.03f, 2.0f);
            }
        }
        else if (k == "style") ly_style_from_str(v, g_ly_style);
        else if (k == "slot1") g_ly_slots[0] = v;
        else if (k == "slot2") g_ly_slots[1] = v;
        else if (k == "slot3") g_ly_slots[2] = v;
    }
}

static void ly_ensure_loaded()
{
    static bool done = false;
    if (done) return;
    done = true;
    ly_load_settings();
}

static void ly_tick()
{
    if (g_ly_dirty && GetTickCount64() - g_ly_last_change > 1000)
        ly_save_settings();
}

static void ly_apply_preset(int n)
{
    LyStyle s;
    if (n == 0) // Minimal: one clean line
    {
        s.lines = 1; s.outline = 1.5f; s.shadow = 1.0f; s.use_bg = false; s.pop = false;
        s.cur[0] = 1; s.cur[1] = 1; s.cur[2] = 1; s.cur[3] = 1;
    }
    else if (n == 1) // Karaoke: gold current line, several lines
    {
        s.lines = 5; s.outline = 2.0f; s.shadow = 2.0f; s.use_bg = false; s.pop = true;
    }
    else if (n == 2) // Cinematic: dark rounded box
    {
        s.lines = 2; s.outline = 0.0f; s.shadow = 0.0f; s.use_bg = true; s.rounding = 16.f; s.pop = false;
        s.cur[0] = 1; s.cur[1] = 1; s.cur[2] = 1; s.cur[3] = 1;
        s.next[0] = 0.85f; s.next[1] = 0.85f; s.next[2] = 0.85f; s.next[3] = 0.65f;
        s.bgc[3] = 0.60f;
    }
    else // Neon
    {
        s.lines = 3; s.outline = 2.0f; s.shadow = 0.0f; s.use_bg = false; s.pop = true;
        s.cur[0] = 0.20f; s.cur[1] = 1.00f; s.cur[2] = 0.90f; s.cur[3] = 1.0f;
        s.next[0] = 0.95f; s.next[1] = 0.35f; s.next[2] = 1.00f; s.next[3] = 0.75f;
        s.past[0] = 0.95f; s.past[1] = 0.35f; s.past[2] = 1.00f; s.past[3] = 0.30f;
    }
    g_ly_style = s;
    ly_mark_dirty();
}

// ---------- LYRICS: drawing on the game screen ----------
static int ly_current_index(float t)
{
    if (g_ly.empty()) return -1;
    if (g_ly_synced)
    {
        float tt = t - g_ly_offset_ms / 1000.f;
        int lo = 0, hi = (int)g_ly.size() - 1, ans = -1;
        while (lo <= hi)
        {
            int mid = (lo + hi) / 2;
            if (g_ly[mid].t <= tt) { ans = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        return ans;
    }
    if (!g_ly_plain_scroll) return -1;
    float len = len_seconds();
    if (len < 1.f) return 0;
    int n = (int)g_ly.size();
    return std::clamp((int)(t / len * n), 0, n - 1);
}

static ImU32 ly_col(const float c[4], float amul)
{
    float a = std::clamp(c[3] * amul * g_ly_style.opacity, 0.f, 1.f);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], a));
}

static void ly_text(ImDrawList *dl, ImFont *font, float sz, ImVec2 p, const float col[4], float amul, const char *s)
{
    const LyStyle &S = g_ly_style;
    if (S.shadow > 0.05f)
    {
        const float sh[4] = { 0.f, 0.f, 0.f, 0.55f };
        dl->AddText(font, sz, ImVec2(p.x + S.shadow, p.y + S.shadow), ly_col(sh, amul), s);
    }
    if (S.outline > 0.05f)
    {
        static const float dx[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
        static const float dy[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
        ImU32 oc = ly_col(S.outc, amul);
        for (int k = 0; k < 8; ++k)
            dl->AddText(font, sz, ImVec2(p.x + dx[k] * S.outline, p.y + dy[k] * S.outline), oc, s);
    }
    dl->AddText(font, sz, p, ly_col(col, amul), s);
}

// Width of a text at a given pixel size (ReShade only exposes ImGui::CalcTextSize, not ImFont methods)
static float ly_text_width(const char *s, float sz)
{
    float base = ImGui::GetFontSize();
    if (base < 1.f) base = 13.f;
    return ImGui::CalcTextSize(s).x * (sz / base);
}

static void ly_dashed_rect(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 c)
{
    auto seg = [&](ImVec2 p, ImVec2 q)
    {
        float dx = q.x - p.x, dy = q.y - p.y;
        float len = sqrtf(dx * dx + dy * dy);
        int n = (int)(len / 8.f);
        for (int i = 0; i < n; i += 2)
        {
            float t0 = (float)i / n, t1 = (float)(i + 1) / n;
            dl->AddLine(ImVec2(p.x + dx * t0, p.y + dy * t0), ImVec2(p.x + dx * t1, p.y + dy * t1), c, 2.0f);
        }
    };
    seg(a, ImVec2(b.x, a.y));
    seg(ImVec2(b.x, a.y), b);
    seg(b, ImVec2(a.x, b.y));
    seg(ImVec2(a.x, b.y), a);
}

static const std::vector<LyLine> &ly_demo()
{
    static const std::vector<LyLine> d = {
        { 0.f, "Your lyrics will appear here" },
        { 0.f, "Drag the box to move it" },
        { 0.f, "Drag the corner to resize it" },
        { 0.f, "Colours change live" },
        { 0.f, "Style it however you like" },
    };
    return d;
}

static void ly_draw_contents(bool editing, float t, float title_a)
{
    LyStyle &S = g_ly_style;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    ImVec2 p1(p0.x + sz.x, p0.y + sz.y);
    ImVec2 ds = ImGui::GetIO().DisplaySize;

    if (editing) // remember where the box was dragged / resized to
    {
        float r[4] = { p0.x / ds.x, p0.y / ds.y, sz.x / ds.x, sz.y / ds.y };
        bool chg = false;
        for (int k = 0; k < 4; ++k)
            if (fabsf(r[k] - g_ly_rect[k]) > 1e-4f) chg = true;
        if (chg)
        {
            memcpy(g_ly_rect, r, sizeof r);
            ly_mark_dirty();
        }
    }

    float rounding = S.use_bg ? S.rounding : 0.f;
    if (S.use_bg) dl->AddRectFilled(p0, p1, ly_col(S.bgc, 1.f), rounding);
    if (editing)
    {
        dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 14), rounding);
        ly_dashed_rect(dl, p0, p1, IM_COL32(255, 255, 255, 220));
        dl->AddTriangleFilled(ImVec2(p1.x - 16, p1.y - 2), ImVec2(p1.x - 2, p1.y - 16), ImVec2(p1.x - 2, p1.y - 2),
                              IM_COL32(255, 255, 255, 200));
        dl->AddText(ImVec2(p0.x + 6, p0.y + 4), IM_COL32(255, 255, 255, 200),
                    "Lyrics box: drag to move, drag the corner to resize");
    }

    dl->PushClipRect(p0, p1, true);
    ImFont *font = ImGui::GetFont();

    const bool demo_mode = editing && g_ly.empty();
    const std::vector<LyLine> &L = demo_mode ? ly_demo() : g_ly;
    static float demo_scroll = 0.f;
    float &scroll = demo_mode ? demo_scroll : g_ly_scroll;
    int cur = demo_mode ? ((int)(ImGui::GetTime() / 1.8) % (int)L.size()) : ly_current_index(t);

    const float N = (float)S.lines;
    const float base = S.fit_box ? std::clamp(sz.y / (N * 1.45f), 10.f, 240.f) : 28.f * S.scale;

    if (!L.empty())
    {
        float target = (float)std::max(cur, 0);
        float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
        if (fabsf(target - scroll) > 6.f) scroll = target;
        else scroll += (target - scroll) * (1.f - expf(-S.anim * dt));

        const float lh = base * 1.4f;
        const float half = N * 0.5f + 0.6f;
        const float cy = p0.y + sz.y * 0.5f;
        int i0 = std::max(0, (int)floorf(scroll - half));
        int i1 = std::min((int)L.size() - 1, (int)ceilf(scroll + half));
        for (int i = i0; i <= i1; ++i)
        {
            const std::string &tx = L[i].text;
            if (tx.empty()) continue;
            float d = (float)i - scroll, ad = fabsf(d);
            if (ad > half) continue;
            float w = (cur >= 0) ? std::max(0.f, 1.f - ad) : 0.f;
            const float *from = (i < cur) ? S.past : S.next;
            float col[4];
            for (int k = 0; k < 4; ++k) col[k] = from[k] + (S.cur[k] - from[k]) * w;
            float a = std::clamp((half - ad) / 0.6f, 0.f, 1.f);
            float lsz = base * (S.pop ? 1.f + 0.08f * w : 1.f);
            float tw = ly_text_width(tx.c_str(), lsz);
            float avail = sz.x - 20.f;
            if (S.fit_width && tw > avail && avail > 10.f)
            {
                lsz *= avail / tw;
                tw = avail;
            }
            float x = (S.align == 0) ? p0.x + 10.f : (S.align == 2) ? p1.x - 10.f - tw : p0.x + (sz.x - tw) * 0.5f;
            float y = cy + d * lh - lsz * 0.5f;
            ly_text(dl, font, lsz, ImVec2(x, y), col, a, tx.c_str());
        }
    }

    // song name card at the start of each song
    float ta = editing ? 1.f : title_a;
    if (S.show_title && ta > 0.f && g_cur >= 0 && g_cur < (int)g_names.size())
    {
        const std::string &title = g_names[g_cur];
        float tsz = std::max(11.f, base * 0.45f);
        float tw = ly_text_width(title.c_str(), tsz);
        float avail = sz.x - 20.f;
        if (tw > avail && avail > 10.f) { tsz *= avail / tw; tw = avail; }
        float x = (S.align == 0) ? p0.x + 10.f : (S.align == 2) ? p1.x - 10.f - tw : p0.x + (sz.x - tw) * 0.5f;
        float y = p0.y + (editing ? 22.f : 6.f);
        ly_text(dl, font, tsz, ImVec2(x, y), S.cur, ta, title.c_str());
    }
    dl->PopClipRect();
}

// Called by ReShade every time it draws its overlay layer. That includes the moments when the
// menu is closed but the FPS / clock counter is switched on in ReShade's settings.
static void on_overlay(reshade::api::effect_runtime *)
{
    ly_ensure_loaded();
    ++g_overlay_calls;
    const bool menu_open = (g_frame - g_ui_frame) <= 3;
    if (!menu_open) ++g_overlay_calls_closed;

    const bool editing = g_ly_edit && menu_open;
    const bool playing = g_engine_ok && g_loaded;
    const float t = cur_seconds();
    const LyStyle &S = g_ly_style;

    float title_a = 0.f;
    if (S.show_title && playing && t < 5.f) title_a = (t < 3.5f) ? 1.f : (5.f - t) / 1.5f;

    if (!editing)
    {
        if (!g_ly_enabled || !playing) return;
        if (g_ly.empty() && title_a <= 0.f) return;
    }

    ImVec2 ds = ImGui::GetIO().DisplaySize;
    if (ds.x < 8.f || ds.y < 8.f) return;

    ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoNav;
    ImVec2 wpos(g_ly_rect[0] * ds.x, g_ly_rect[1] * ds.y);
    ImVec2 wsize(g_ly_rect[2] * ds.x, g_ly_rect[3] * ds.y);
    if (!editing)
    {
        // locked: click-through, always exactly where it was saved
        fl |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing;
        ImGui::SetNextWindowPos(wpos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(wsize, ImGuiCond_Always);
    }
    else
    {
        ImGuiCond c = g_ly_reset ? ImGuiCond_Always : ImGuiCond_Appearing;
        ImGui::SetNextWindowPos(wpos, c);
        ImGui::SetNextWindowSize(wsize, c);
        ImGui::SetNextWindowSizeConstraints(ImVec2(120.f, 40.f), ImVec2(FLT_MAX, FLT_MAX));
    }
    g_ly_reset = false;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    if (ImGui::Begin(editing ? "##ly_edit" : "##ly_view", nullptr, fl))
        ly_draw_contents(editing, t, title_a);
    ImGui::End();
    ImGui::PopStyleVar(3);
}

// ---------- LYRICS: menu tab ----------
static void draw_lyrics_tab()
{
    ly_ensure_loaded();
    LyStyle &S = g_ly_style;
    bool ch = false;

    ch |= ImGui::Checkbox("Show lyrics on screen (F10)", &g_ly_enabled);
    ImGui::SameLine();
    ImGui::Checkbox("Edit layout (move / resize the box)", &g_ly_edit);

    if (g_ly.empty())
        ImGui::TextDisabled("No lyrics for this song. Add them to the file's lyrics tag, or put a same-name .lrc file next to it.");
    else
        ImGui::Text("Lyrics: %s, %d lines (from %s)", g_ly_synced ? "synced" : "not synced", (int)g_ly.size(), g_ly_source.c_str());

    ImGui::TextDisabled("Overlay layer: drawn %llu times, %llu of them with this menu closed.",
                        g_overlay_calls, g_overlay_calls_closed);
    if (g_overlay_calls_closed == 0)
        ImGui::TextDisabled("If this stays 0 after closing the menu, tick 'Show FPS' (or clock) in ReShade's Settings so it keeps drawing.");

    if (ImGui::Button("Reload lyrics") && g_cur >= 0 && g_cur < (int)g_songs.size())
        lyrics_load_for(g_songs[g_cur]);
    ImGui::SameLine();
    if (ImGui::Button("Reset position"))
    {
        g_ly_rect[0] = 0.25f; g_ly_rect[1] = 0.76f; g_ly_rect[2] = 0.50f; g_ly_rect[3] = 0.17f;
        g_ly_reset = true;
        ch = true;
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Presets");
    const char *preset_names[4] = { "Minimal", "Karaoke", "Cinematic", "Neon" };
    for (int i = 0; i < 4; ++i)
    {
        if (i) ImGui::SameLine();
        if (ImGui::Button(preset_names[i])) ly_apply_preset(i);
    }
    ImGui::TextUnformatted("My slots");
    for (int i = 0; i < 3; ++i)
    {
        ImGui::PushID(100 + i);
        if (ImGui::SmallButton("Save")) { g_ly_slots[i] = ly_style_to_str(S); ly_mark_dirty(); }
        ImGui::SameLine();
        bool has = !g_ly_slots[i].empty();
        if (ImGui::SmallButton("Load") && has) { ly_style_from_str(g_ly_slots[i], S); ly_mark_dirty(); }
        ImGui::SameLine();
        ImGui::Text("Slot %d%s", i + 1, has ? "" : " (empty)");
        ImGui::PopID();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Colours");
    ch |= ImGui::ColorEdit4("Current line", S.cur);
    ch |= ImGui::ColorEdit4("Upcoming lines", S.next);
    ch |= ImGui::ColorEdit4("Past lines", S.past);
    ch |= ImGui::ColorEdit4("Outline", S.outc);
    ch |= ImGui::ColorEdit4("Background box", S.bgc);

    ImGui::Separator();
    ImGui::TextUnformatted("Text");
    const char *aligns[3] = { "Left", "Center", "Right" };
    ch |= ImGui::Combo("Alignment", &S.align, aligns, 3);
    ch |= ImGui::SliderInt("Visible lines", &S.lines, 1, 9);
    ch |= ImGui::Checkbox("Text size follows the box (resize box = resize text)", &S.fit_box);
    if (!S.fit_box) ch |= ImGui::SliderFloat("Text size", &S.scale, 0.4f, 4.0f, "%.2f");
    ch |= ImGui::Checkbox("Shrink long lines to fit the box width", &S.fit_width);
    ch |= ImGui::Checkbox("Current line slightly bigger", &S.pop);
    ch |= ImGui::SliderFloat("Outline", &S.outline, 0.0f, 4.0f, "%.1f px");
    ch |= ImGui::SliderFloat("Shadow", &S.shadow, 0.0f, 6.0f, "%.1f px");
    ch |= ImGui::SliderFloat("Opacity", &S.opacity, 0.1f, 1.0f, "%.2f");
    ch |= ImGui::SliderFloat("Scroll speed", &S.anim, 3.0f, 30.0f, "%.0f");

    ImGui::Separator();
    ch |= ImGui::Checkbox("Background box", &S.use_bg);
    if (S.use_bg) ch |= ImGui::SliderFloat("Corner rounding", &S.rounding, 0.0f, 40.0f, "%.0f");
    ch |= ImGui::Checkbox("Show song name for the first seconds", &S.show_title);
    ch |= ImGui::Checkbox("Plain lyrics (no timestamps) scroll with song progress", &g_ly_plain_scroll);
    ch |= ImGui::SliderInt("Sync offset (ms)", &g_ly_offset_ms, -3000, 3000);
    ImGui::TextDisabled("Sync offset: + shows lyrics later, - shows them earlier.");

    if (ch) ly_mark_dirty();
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
    ++g_frame;
    ly_tick();
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
         f8 = key_pressed_once(VK_F8), f9 = key_pressed_once(VK_F9),
         f10 = key_pressed_once(VK_F10);
    if (!focused) return;
    if (f5) set_volume(g_vol - 0.05f);
    if (f6) set_volume(g_vol + 0.05f);
    if (f7) prev_song();
    if (f8) toggle_pause();
    if (f9) next_song();
    if (f10) { g_ly_enabled = !g_ly_enabled; ly_mark_dirty(); }
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

    ImGui::TextDisabled("Hotkeys (menu closed): F5/F6 volume, F7 prev, F8 play/pause, F9 next, F10 lyrics on/off");
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
    g_ui_frame = g_frame; // tells the lyrics layer that the menu is open
    if (ImGui::BeginTabBar("##music_tabs"))
    {
        if (ImGui::BeginTabItem("Player"))
        {
            draw_player();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Lyrics"))
        {
            draw_lyrics_tab();
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
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Offline music player (search, playlist, repeat), on-screen lyrics overlay and per-game notes.";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID lpReserved)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(hModule))
            return FALSE;
        reshade::register_overlay("Music", draw_ui);
        reshade::register_event<reshade::addon_event::reshade_present>(on_present);
        reshade::register_event<reshade::addon_event::reshade_overlay>(on_overlay);
        break;
    case DLL_PROCESS_DETACH:
        if (g_notes_dirty) save_notes();
        if (g_ly_dirty) ly_save_settings();
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
