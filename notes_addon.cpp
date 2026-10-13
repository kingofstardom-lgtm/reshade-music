// Notes tab as a separate ReShade add-on (independent from the music add-on).
// Notes are saved per game in game_notes.txt next to the game's exe.
// Build as 64-bit DLL, rename output to notes.addon64

#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#define NOMINMAX
#include <imgui.h>
#include <reshade.hpp>

#include <windows.h>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static const size_t NOTES_SIZE = 65536;
static char g_notes[NOTES_SIZE] = {};
static bool g_loaded = false;
static bool g_dirty = false;
static ULONGLONG g_last_change = 0;

static fs::path notes_path()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path() / "game_notes.txt";
}

static void load_notes()
{
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream f(notes_path(), std::ios::binary);
    if (!f) return;
    f.read(g_notes, NOTES_SIZE - 1);
    g_notes[f.gcount()] = '\0';
}

static void save_notes()
{
    std::ofstream f(notes_path(), std::ios::binary | std::ios::trunc);
    if (f) f.write(g_notes, (std::streamsize)strlen(g_notes));
    g_dirty = false;
}

// Called every frame: saves about 1 second after the last edit
static void on_present(reshade::api::effect_runtime *)
{
    load_notes();
    if (g_dirty && GetTickCount64() - g_last_change > 1000)
        save_notes();
}

static void draw_notes(reshade::api::effect_runtime *)
{
    load_notes();
    ImGui::TextUnformatted("Notes for this game (saved automatically in game_notes.txt)");
    if (ImGui::InputTextMultiline("##notes", g_notes, NOTES_SIZE, ImVec2(-1.0f, 420.0f),
                                  ImGuiInputTextFlags_AllowTabInput))
    {
        g_dirty = true;
        g_last_change = GetTickCount64();
    }
    ImGui::TextDisabled(g_dirty ? "Saving..." : "Saved");
}

extern "C" __declspec(dllexport) const char *NAME = "Game Notes";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Per-game notes tab with autosave.";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID lpReserved)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(hModule))
            return FALSE;
        reshade::register_overlay("Notes", draw_notes);
        reshade::register_event<reshade::addon_event::reshade_present>(on_present);
        break;
    case DLL_PROCESS_DETACH:
        if (g_dirty) save_notes();
        reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}
