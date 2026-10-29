/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>

#include <algorithm>
#include <memory>
#include <vector>

#include <switch.h>

#include "Layout.h"
#include "RomInfo.h"
#include "Ui.h"
#include "UiHome.h"

using namespace Ui;

KeyValues ReadKeyValues(const std::string& path)
{
    KeyValues kv;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return kv;
    char line[1024];
    while (fgets(line, sizeof(line), f))
    {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        size_t eq = s.find('=');
        if (eq != std::string::npos) kv[s.substr(0, eq)] = s.substr(eq + 1);
    }
    fclose(f);
    return kv;
}

bool WriteKeyValues(const std::string& path, const KeyValues& kv)
{
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;
    for (auto& p : kv) fprintf(f, "%s=%s\n", p.first.c_str(), p.second.c_str());
    return fclose(f) == 0;
}

static std::string LowerCase(std::string s)
{
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static std::string StripExt(const std::string& name)
{
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

static std::string BaseName(const std::string& path)
{
    size_t sl = path.find_last_of('/');
    return sl == std::string::npos ? path : path.substr(sl + 1);
}

static bool IsFile(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// ---- settings: the same rows for the general settings (ui.txt) and the settings of one game (<rom>.ui) ----
struct Choice { const char* Value; const char* Label; };
struct SettingDef { const char* Key; const char* Label; std::vector<Choice> Choices; const char* Default; };

static const SettingDef* FindDef(const char* key);

static const std::vector<SettingDef>& Defs()
{
    static const std::vector<SettingDef> defs = {
        {"layout", "Disposition", {{"0", "verticale"}, {"1", "horizontale"}, {"2", "hybride"}}, "0"},
        {"sizing", "Taille des écrans", {{"0", "égale"}, {"1", "haut plus grand"}, {"2", "bas plus grand"}, {"3", "haut seul"}, {"4", "bas seul"}}, "0"},
        {"swap", "Inverser haut et bas", {{"0", "non"}, {"1", "oui"}}, "0"},
        {"rotation", "Rotation", {{"0", "normale"}, {"1", "livre (90°)"}, {"3", "livre (270°)"}}, "0"},
        {"intscale", "Échelle", {{"0", "ajustée à l'écran"}, {"1", "entière"}}, "0"},
        {"gap", "Écart entre les écrans", {{"0", "0"}, {"8", "8"}, {"16", "16"}, {"32", "32"}, {"64", "64"}}, "0"},
        {"autotop", "Grand écran si le bas est noir", {{"1", "oui"}, {"0", "non"}}, "1"},
        {"gpu", "Rendu 3D", {{"1", "GPU (GLES 3)"}, {"0", "logiciel"}}, "1"},
        {"gpuscale", "Résolution GPU", {{"1", "1x"}, {"2", "2x"}, {"3", "3x"}}, "2"},
        {"threaded", "3D sur un 2e cœur (logiciel)", {{"1", "oui"}, {"0", "non"}}, "1"},
        {"cpu", "Fréquence CPU", {{"0", "auto"}, {"1020", "1020 MHz"}, {"1224", "1224 MHz"}, {"1581", "1581 MHz"}, {"1785", "1785 MHz"}}, "0"},
        {"filter", "Filtre", {{"0", "pixels nets"}, {"1", "lissé"}}, "0"},
        {"showfps", "Afficher les fps", {{"0", "non"}, {"1", "oui"}}, "0"},
    };
    return defs;
}

static const SettingDef* FindDef(const char* key)
{
    for (const SettingDef& d : Defs())
        if (!strcmp(d.Key, key)) return &d;
    return nullptr;
}

// value of a setting: kv, else the fallback (general settings), else the default
static std::string Value(const KeyValues& kv, const KeyValues* fallback, const SettingDef& d)
{
    auto it = kv.find(d.Key);
    if (it != kv.end()) return it->second;
    if (fallback && (it = fallback->find(d.Key)) != fallback->end()) return it->second;
    return d.Default;
}

static void AddSetting(std::vector<Item>& items, KeyValues& kv, const KeyValues* fallback, const char* key,
                       const std::function<void()>& changed, const char* label = nullptr)
{
    const SettingDef* d = FindDef(key);
    std::string v = Value(kv, fallback, *d);
    int idx = 0;
    for (size_t i = 0; i < d->Choices.size(); i++)
        if (v == d->Choices[i].Value) idx = (int)i;
    items.push_back({label ? label : d->Label, d->Choices[idx].Label, [&kv, d, idx, changed](int dir, bool) {
        int n = (int)d->Choices.size();
        kv[d->Key] = d->Choices[(idx + (dir < 0 ? n - 1 : 1)) % n].Value;
        changed();
    }});
}

static std::vector<Item> SettingItems(KeyValues& kv, const KeyValues* fallback, bool general, const std::function<void()>& changed)
{
    std::vector<Item> items;
    auto header = [&](const char* t) { Item it; it.Label = t; it.Header = true; items.push_back(it); };
    header("Écrans");
    AddSetting(items, kv, fallback, "layout", changed);
    bool hybrid = Value(kv, fallback, *FindDef("layout")) == "2";
    if (hybrid)
    {
        // hybrid: "swap" chooses the big screen
        const SettingDef* d = FindDef("swap");
        bool bottom = Value(kv, fallback, *d) == "1";
        items.push_back({"Grand écran", bottom ? "bas" : "haut", [&kv, bottom, changed](int, bool) { kv["swap"] = bottom ? "0" : "1"; changed(); }});
    }
    else
    {
        AddSetting(items, kv, fallback, "sizing", changed);
        AddSetting(items, kv, fallback, "swap", changed);
    }
    AddSetting(items, kv, fallback, "rotation", changed);
    AddSetting(items, kv, fallback, "intscale", changed);
    AddSetting(items, kv, fallback, "gap", changed);
    AddSetting(items, kv, fallback, "autotop", changed);
    header("Rendu");
    AddSetting(items, kv, fallback, "gpu", changed);
    AddSetting(items, kv, fallback, "gpuscale", changed);
    AddSetting(items, kv, fallback, "threaded", changed);
    header("Processeur");
    AddSetting(items, kv, fallback, "cpu", changed);
    if (general)
    {
        header("Affichage");
        AddSetting(items, kv, fallback, "filter", changed);
        AddSetting(items, kv, fallback, "showfps", changed);
    }
    return items;
}

// ---- game lists ----
struct Entry
{
    std::string Path, Name;
    std::string Title, Subtitle, Code;   // from the library cache (before the ROM is read)
    bool Favorite = false;
    bool Dir = false, Up = false;
    bool Loaded = false;
    bool HasGameSettings = false;
    std::shared_ptr<RomInfo> Info;
};

static void LoadInfo(Entry& e, const std::string& configDir)
{
    if (e.Loaded || e.Dir) return;
    e.Loaded = true;
    e.Info = std::make_shared<RomInfo>();
    ReadRomInfo(e.Path, *e.Info, 2);
    e.HasGameSettings = IsFile(configDir + "/" + StripExt(BaseName(e.Path)) + ".ui");
}

static std::vector<Entry> ListDir(const std::string& dir)
{
    std::vector<Entry> dirs, files;
    auto join = [&](const std::string& n) { return dir.back() == '/' ? dir + n : dir + "/" + n; };
    if (DIR* d = opendir(dir.c_str()))
    {
        while (dirent* e = readdir(d))
        {
            std::string n = e->d_name;
            if (n.empty() || n[0] == '.') continue;
            struct stat st;
            bool isDir = e->d_type == DT_DIR || (e->d_type == DT_UNKNOWN && stat(join(n).c_str(), &st) == 0 && S_ISDIR(st.st_mode));
            Entry en;
            en.Path = join(n);
            en.Name = n;
            en.Dir = isDir;
            if (isDir) dirs.push_back(en);
            else
            {
                std::string l = LowerCase(n);
                if (l.size() > 4 && (l.compare(l.size() - 4, 4, ".nds") == 0 || l.compare(l.size() - 4, 4, ".srl") == 0 || l.compare(l.size() - 4, 4, ".dsi") == 0))
                    files.push_back(en);
            }
        }
        closedir(d);
    }
    auto byName = [](const Entry& a, const Entry& b) { return LowerCase(a.Name) < LowerCase(b.Name); };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    std::vector<Entry> out;
    if (dir.size() > 6)
    {
        Entry up;
        up.Name = "Dossier parent";
        up.Dir = up.Up = true;
        size_t sl = dir.find_last_of('/');
        up.Path = sl <= 5 ? "sdmc:/" : dir.substr(0, sl);
        out.push_back(up);
    }
    out.insert(out.end(), dirs.begin(), dirs.end());
    out.insert(out.end(), files.begin(), files.end());
    return out;
}

static const int ListX = 30, ListY = 104, ListW = 760, RowH = 90, Rows = 6;

static void DrawFolderIcon(Gfx* g, int x, int y, bool up)
{
    Fill(g, x + 4, y + 10, 26, 10, 0, 160, 190);
    Fill(g, x + 4, y + 16, 56, 38, 0, 200, 235);
    if (up)
    {
        // arrow up: head made of bars getting wider, then the shaft
        for (int i = 0; i < 8; i++)
            Fill(g, x + 31 - i * 2, y + 22 + i * 2, 2 + i * 4, 2, 35, 38, 44);
        Fill(g, x + 29, y + 38, 6, 12, 35, 38, 44);
    }
}

static void DrawGameList(Gfx* g, std::vector<Entry>& list, ListState& st, const std::string& configDir)
{
    int n = (int)list.size();
    st.Sel = std::clamp(st.Sel, 0, std::max(n - 1, 0));
    if (st.Sel < st.Top) st.Top = st.Sel;
    if (st.Sel >= st.Top + Rows) st.Top = st.Sel - Rows + 1;
    st.Top = std::clamp(st.Top, 0, std::max(n - Rows, 0));
    for (int i = 0; i < Rows && st.Top + i < n; i++)
    {
        Entry& e = list[st.Top + i];
        int y = ListY + i * RowH;
        bool sel = st.Top + i == st.Sel;
        if (sel)
        {
            Fill(g, ListX, y + 2, ListW, RowH - 4, 50, 56, 66);
            Fill(g, ListX, y + 2, ListW, 3, 0, 200, 235);
            Fill(g, ListX, y + RowH - 5, ListW, 3, 0, 200, 235);
            Fill(g, ListX, y + 2, 3, RowH - 4, 0, 200, 235);
            Fill(g, ListX + ListW - 3, y + 2, 3, RowH - 4, 0, 200, 235);
        }
        else
            Fill(g, ListX + 16, y + RowH - 1, ListW - 32, 1, 60, 65, 74);
        if (e.Dir)
        {
            DrawFolderIcon(g, ListX + 16, y + 13, e.Up);
            DrawText(g, e.Name, ListX + 100, y + (RowH - 36) / 2, Text, Normal, ListW - 120);
            continue;
        }
        LoadInfo(e, configDir);
        if (e.Info->HasIcon) DrawIcon(g, e.Path, e.Info->Icon, ListX + 16, y + 13, 64);
        else Fill(g, ListX + 16, y + 13, 64, 64, 60, 65, 74);
        DrawText(g, e.Info->Title, ListX + 100, y + 12, Text, Normal, ListW - 120);
        DrawText(g, e.Info->Subtitle.empty() ? e.Name : e.Info->Subtitle, ListX + 100, y + 50, TextDim, Small, ListW - 120);
    }
    if (n > Rows)
    {
        int h = Rows * RowH;
        Fill(g, ListX + ListW + 8, ListY, 4, h, 60, 65, 74);
        Fill(g, ListX + ListW + 8, ListY + h * st.Top / n, 4, std::max(h * Rows / n, 20), 150, 158, 170);
    }
}

// details of the selected game, on the right
static void DrawDetails(Gfx* g, Entry* e, const std::string& configDir)
{
    const int x = 830, w = 400;
    Fill(g, x - 20, ListY, 1, Rows * RowH, 60, 65, 74);
    if (!e) return;
    if (e->Dir)
    {
        DrawFolderIcon(g, x + 120, ListY + 40, e->Up);
        DrawWrapped(g, e->Up ? "Revenir au dossier parent" : e->Name, x, ListY + 130, w, Text, Normal, 3);
        return;
    }
    LoadInfo(*e, configDir);
    const RomInfo& r = *e->Info;
    if (r.HasIcon) DrawIcon(g, e->Path, r.Icon, x + 104, ListY + 10, 192);
    else Fill(g, x + 104, ListY + 10, 192, 192, 60, 65, 74);
    int y = ListY + 222;
    y += DrawWrapped(g, r.Title, x, y, w, Text, Big, 2);
    if (!r.Subtitle.empty()) y += DrawWrapped(g, r.Subtitle, x, y + 4, w, TextDim, Small, 2) + 4;
    y += 10;
    DrawText(g, r.FileName, x, y, TextDim, Small, w);
    y += 32;
    std::string s = r.GameCode.empty() ? "" : "Code " + r.GameCode + "   ";
    s += e->HasGameSettings ? "Réglages propres à ce jeu" : (BuiltinSoftware(r.GameCode) ? "Rendu logiciel (par défaut pour ce jeu)" : "Réglages généraux");
    DrawText(g, s, x, y, e->HasGameSettings ? AccentText : TextDim, Small, w);
}

// settings of one game: <rom>.ui, written with every value (like the in-game menu does)
static void GameSettings(Gfx* g, Entry& e, const std::string& configDir, const KeyValues& general)
{
    LoadInfo(e, configDir);
    const std::string file = configDir + "/" + StripExt(BaseName(e.Path)) + ".ui";
    KeyValues kv = ReadKeyValues(file);
    KeyValues fallback = general;
    if (BuiltinSoftware(e.Info->GameCode)) fallback["gpu"] = "0";
    ListState st;
    std::string note;
    bool done = false;
    while (!done && appletMainLoop())
    {
        auto save = [&]() {
            // every setting of the game, its own value or the general one
            KeyValues out;
            for (const char* k : {"cpu", "threaded", "gpu", "gpuscale", "layout", "sizing", "swap", "rotation", "intscale", "gap", "autotop"})
                out[k] = Value(kv, &fallback, *FindDef(k));
            kv = out;
            note = WriteKeyValues(file, out) ? "" : "Impossible d'écrire " + file;
            e.HasGameSettings = true;
        };
        std::vector<Item> items = SettingItems(kv, &fallback, false, save);
        Item reset;
        reset.Label = "Revenir aux réglages généraux";
        reset.Do = [&](int, bool act) {
            if (!act) return;
            remove(file.c_str());
            kv.clear();
            e.HasGameSettings = false;
            note = "Ce jeu utilise maintenant les réglages généraux.";
        };
        items.push_back(reset);

        DrawFrame(g, "Réglages : " + e.Info->Title, {UI_A " / " UI_DPAD_LR " Changer", UI_B " Retour"});
        DrawList(g, items, st, ListX, ListY, 1200, 9, 58);
        if (!note.empty()) DrawText(g, note, 60, 666, Warn, Small, 700);
        else DrawText(g, e.HasGameSettings ? "Réglages propres à ce jeu" : "Ce jeu suit les réglages généraux", 60, 666, TextDim, Small, 700);
        GfxPresent(g);
        Trim();
        Input in;
        if (!Poll(in, 100)) continue;
        if (in.Button == BtnB) done = true;
        else ListInput(items, st, in, ListX, ListY, 1200, 9, 58);
    }
}

static void DrawHelp(Gfx* g)
{
    struct Line { const char* Keys; const char* What; };
    static const Line lines[] = {
        {UI_DPAD " / " UI_LSTICK "   " UI_A " " UI_B " " UI_X " " UI_Y "   " UI_L " " UI_R "   " UI_PLUS " " UI_MINUS, "Boutons de la DS (" UI_PLUS " = Start, " UI_MINUS " = Select)"},
        {"Toucher l'écran", "Écran tactile de la DS (mode portable)"},
        {UI_RSTICK " puis " UI_RSTICK " enfoncé", "Curseur tactile sans toucher l'écran (mode TV)"},
        {UI_ZL " + " UI_ZR " + " UI_PLUS, "Menu : affichage, rendu, états, changer de jeu, quitter"},
        {UI_ZL " + " UI_ZR " + " UI_X " / " UI_Y, "Sauvegarder / charger l'état"},
        {UI_ZL " + " UI_ZR " + " UI_DPAD_LR, "Choisir l'emplacement d'état (1 à 4)"},
        {UI_ZL " + " UI_ZR " + " UI_UP, "Disposition suivante (verticale, horizontale, hybride)"},
        {UI_ZL " + " UI_ZR " + " UI_MINUS, "Inverser les écrans (hybride : grand écran haut/bas)"},
        {UI_ZL " + " UI_ZR " + " UI_B, "Quitter (état sauvegardé, repris au lancement)"},
        {UI_L " + " UI_R " + " UI_PLUS " + " UI_MINUS, "Va au jeu (retour à l'écran titre dans beaucoup de jeux)"},
    };
    int y = ListY + 6;
    for (const Line& l : lines)
    {
        DrawText(g, l.Keys, 60, y, AccentText, Small, 480);
        DrawText(g, l.What, 560, y, Text, Small, 680);
        y += 50;
    }
    DrawWrapped(g, "Pas de BIOS ni de firmware nécessaires. Les jeux (fichiers .nds de vos propres cartouches) peuvent être n'importe où sur la carte SD. "
                   "Réglages, sauvegardes et états : sdmc:/switch/melonDS-Switch/.", 60, y + 10, 1160, TextDim, Small);
}

bool WriteThumb(const std::string& path, const std::vector<unsigned char>& rgba, int w, int h)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    uint16_t dims[2] = {(uint16_t)w, (uint16_t)h};
    bool ok = fwrite("MDTH", 1, 4, f) == 4 && fwrite(dims, 2, 2, f) == 2 && fwrite(rgba.data(), 1, rgba.size(), f) == rgba.size();
    return (fclose(f) == 0) && ok;
}

bool ReadThumb(const std::string& path, std::vector<uint32_t>& rgba, int& w, int& h)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[4];
    uint16_t dims[2];
    bool ok = fread(magic, 1, 4, f) == 4 && !memcmp(magic, "MDTH", 4) && fread(dims, 2, 2, f) == 2 && dims[0] && dims[1] && dims[0] <= 512 && dims[1] <= 512;
    if (ok)
    {
        w = dims[0]; h = dims[1];
        rgba.resize((size_t)w * h);
        ok = fread(rgba.data(), 4, rgba.size(), f) == rgba.size();
    }
    fclose(f);
    return ok;
}

// ---- library: every game of the library folders (and their subfolders), titles cached in library.txt ----
static bool IsRomName(const std::string& n)
{
    std::string l = LowerCase(n);
    return l.size() > 4 && (l.compare(l.size() - 4, 4, ".nds") == 0 || l.compare(l.size() - 4, 4, ".srl") == 0 || l.compare(l.size() - 4, 4, ".dsi") == 0);
}

static void ScanDir(const std::string& dir, int depth, std::vector<std::string>& out)
{
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> subdirs;
    while (dirent* e = readdir(d))
    {
        std::string n = e->d_name;
        if (n.empty() || n[0] == '.') continue;
        std::string p = dir.back() == '/' ? dir + n : dir + "/" + n;
        struct stat st;
        bool isDir = e->d_type == DT_DIR || (e->d_type == DT_UNKNOWN && stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
        if (isDir) { if (depth > 0) subdirs.push_back(p); }
        else if (IsRomName(n)) out.push_back(p);
    }
    closedir(d);
    for (const std::string& s : subdirs) ScanDir(s, depth - 1, out);
}

static std::vector<std::string> SplitList(const std::string& s)
{
    std::vector<std::string> out;
    size_t p = 0;
    while (p < s.size())
    {
        size_t q = s.find('|', p);
        if (q == std::string::npos) q = s.size();
        if (q > p) out.push_back(s.substr(p, q - p));
        p = q + 1;
    }
    return out;
}

static std::string JoinList(const std::vector<std::string>& v)
{
    std::string s;
    for (size_t i = 0; i < v.size(); i++) s += (i ? "|" : "") + v[i];
    return s;
}

struct LibCacheEntry { long long Size = 0, MTime = 0; std::string Code, Title, Subtitle; };

static std::map<std::string, LibCacheEntry> ReadLibCache(const std::string& file)
{
    std::map<std::string, LibCacheEntry> m;
    FILE* f = fopen(file.c_str(), "r");
    if (!f) return m;
    char line[2048];
    while (fgets(line, sizeof(line), f))
    {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        std::vector<std::string> col;
        size_t p = 0;
        for (;;)
        {
            size_t q = s.find('\t', p);
            col.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
            if (q == std::string::npos) break;
            p = q + 1;
        }
        if (col.size() < 6) continue;
        LibCacheEntry e;
        e.Size = atoll(col[1].c_str()); e.MTime = atoll(col[2].c_str());
        e.Code = col[3]; e.Title = col[4]; e.Subtitle = col[5];
        m[col[0]] = e;
    }
    fclose(f);
    return m;
}

static std::string NoTabs(std::string s)
{
    for (auto& c : s) if (c == '\t' || c == '\n') c = ' ';
    return s;
}

// library entries, favourites first, then by title. Titles come from the cache when the file did not change.
static std::vector<Entry> BuildLibrary(const std::vector<std::string>& dirs, const std::string& configDir, const std::vector<std::string>& favs)
{
    const std::string cacheFile = configDir + "/library.txt";
    auto cache = ReadLibCache(cacheFile);
    std::vector<std::string> paths;
    for (const std::string& d : dirs) ScanDir(d, 4, paths);
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    std::vector<Entry> out;
    std::map<std::string, LibCacheEntry> newCache;
    for (const std::string& p : paths)
    {
        struct stat st;
        if (stat(p.c_str(), &st) != 0) continue;
        Entry e;
        e.Path = p;
        e.Name = BaseName(p);
        auto it = cache.find(p);
        LibCacheEntry ce;
        if (it != cache.end() && it->second.Size == (long long)st.st_size && it->second.MTime == (long long)st.st_mtime)
            ce = it->second;
        else
        {
            RomInfo r;
            ReadRomInfo(p, r, 2);
            ce.Size = st.st_size; ce.MTime = st.st_mtime; ce.Code = r.GameCode; ce.Title = NoTabs(r.Title); ce.Subtitle = NoTabs(r.Subtitle);
        }
        newCache[p] = ce;
        e.Title = ce.Title; e.Subtitle = ce.Subtitle; e.Code = ce.Code;
        e.Favorite = std::find(favs.begin(), favs.end(), p) != favs.end();
        out.push_back(e);
    }
    if (FILE* f = fopen(cacheFile.c_str(), "w"))
    {
        for (auto& kv : newCache)
            fprintf(f, "%s\t%lld\t%lld\t%s\t%s\t%s\n", kv.first.c_str(), kv.second.Size, kv.second.MTime, kv.second.Code.c_str(), kv.second.Title.c_str(), kv.second.Subtitle.c_str());
        fclose(f);
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        if (a.Favorite != b.Favorite) return a.Favorite;
        return LowerCase(a.Title) < LowerCase(b.Title);
    });
    return out;
}

// ---- grid ----
static const int GridX = 58, GridY = 100, TileW = 220, TileH = 262, TileGap = 16, GridCols = 5, GridRows = 2;

static void DrawStar(Gfx* g, int x, int y)
{
    // small yellow diamond with a dark outline
    for (int i = 0; i < 9; i++)
    {
        int w = (i < 5 ? i : 8 - i) * 2 + 1;
        Fill(g, x + 8 - w / 2 - 1, y + i * 2 - 1, w + 2, 4, 30, 30, 30);
    }
    for (int i = 0; i < 9; i++)
    {
        int w = (i < 5 ? i : 8 - i) * 2 + 1;
        Fill(g, x + 8 - w / 2, y + i * 2, w, 2, 255, 205, 60);
    }
}

static void DrawGrid(Gfx* g, std::vector<Entry>& list, ListState& st, const std::string& configDir)
{
    int n = (int)list.size();
    st.Sel = std::clamp(st.Sel, 0, std::max(n - 1, 0));
    int selRow = st.Sel / GridCols;
    if (selRow < st.Top) st.Top = selRow;
    if (selRow >= st.Top + GridRows) st.Top = selRow - GridRows + 1;
    int rows = (n + GridCols - 1) / GridCols;
    st.Top = std::clamp(st.Top, 0, std::max(rows - GridRows, 0));
    for (int r = 0; r < GridRows; r++)
        for (int c = 0; c < GridCols; c++)
        {
            int i = (st.Top + r) * GridCols + c;
            if (i >= n) break;
            Entry& e = list[i];
            int x = GridX + c * (TileW + TileGap), y = GridY + r * (TileH + TileGap);
            bool sel = i == st.Sel;
            if (sel)
            {
                Fill(g, x - 4, y - 4, TileW + 8, TileH + 8, 0, 200, 235);
                Fill(g, x, y, TileW, TileH, 58, 64, 75);
            }
            else
                Fill(g, x, y, TileW, TileH, 46, 50, 58);
            LoadInfo(e, configDir);
            if (e.Info->HasIcon) DrawIcon(g, e.Path, e.Info->Icon, x + 46, y + 16, 128);
            else Fill(g, x + 46, y + 16, 128, 128, 60, 65, 74);
            DrawWrapped(g, e.Info->Title, x + 12, y + 156, TileW - 24, sel ? Text : TextDim, Small, 2);
            if (!e.Info->Subtitle.empty())
            {
                std::string pub = e.Info->Subtitle;
                size_t dash = pub.rfind(" - ");
                if (dash != std::string::npos) pub = pub.substr(dash + 3);
                DrawText(g, pub, x + 12, y + 226, TextDim, Small, TileW - 24);
            }
            if (e.Favorite) DrawStar(g, x + TileW - 30, y + 10);
        }
    if (rows > GridRows)
    {
        int h = GridRows * (TileH + TileGap) - TileGap;
        Fill(g, 1252, GridY, 4, h, 60, 65, 74);
        Fill(g, 1252, GridY + h * st.Top / rows, 4, std::max(h * GridRows / rows, 20), 150, 158, 170);
    }
}

// ---- game page: continue, start without a state, states 1-4 (with their screenshot), settings, favourite ----
static std::string StateFile(const std::string& configDir, const std::string& rom, int slot)
{
    return configDir + "/" + StripExt(BaseName(rom)) + ".ss" + std::to_string(slot);
}

static std::string FileDate(const std::string& path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return "";
    time_t t = st.st_mtime;
    struct tm* tm = localtime(&t);
    char buf[32];
    if (!tm) return "";
    strftime(buf, sizeof(buf), "%d/%m %H:%M", tm);
    return buf;
}

// returns the state to load (-1 none, 0..4) or -3 to go back; favourite / recent changes are applied to the files
static int GamePage(Gfx* g, Entry& e, const std::string& configDir, const KeyValues& general, bool fromRecent,
                    std::vector<std::string>& favs, const std::string& homeFile, KeyValues& home, bool& changed)
{
    LoadInfo(e, configDir);
    ListState st;
    int result = -4;
    while (result == -4 && appletMainLoop())
    {
        struct Action { int Slot; };   // slot of the state shown on the right (-1: none)
        std::vector<Item> items;
        std::vector<int> itemSlot;
        auto add = [&](const std::string& label, const std::string& value, int slot, std::function<void(int, bool)> fn) {
            items.push_back({label, value, fn});
            itemSlot.push_back(slot);
        };
        bool hasAuto = IsFile(StateFile(configDir, e.Path, 0));
        if (hasAuto)
            add("Continuer", FileDate(StateFile(configDir, e.Path, 0)), 0, [&](int, bool act) { if (act) result = 0; });
        add(hasAuto ? "Recommencer (sans charger d'état)" : "Jouer", "", -1, [&](int, bool act) { if (act) result = -1; });
        for (int s = 1; s <= 4; s++)
        {
            std::string f = StateFile(configDir, e.Path, s);
            if (IsFile(f)) add("Charger l'état " + std::to_string(s), FileDate(f), s, [&, s](int, bool act) { if (act) result = s; });
        }
        add("Réglages de ce jeu", e.HasGameSettings ? "propres" : "généraux", -1, [&](int, bool act) { if (act) GameSettings(g, e, configDir, general); });
        add("Favori", e.Favorite ? "oui" : "non", -1, [&](int, bool) {
            e.Favorite = !e.Favorite;
            favs.erase(std::remove(favs.begin(), favs.end(), e.Path), favs.end());
            if (e.Favorite) favs.push_back(e.Path);
            home["favorites"] = JoinList(favs);
            WriteKeyValues(homeFile, home);
            changed = true;
        });
        if (fromRecent)
            add("Retirer des jeux récents", "", -1, [&](int, bool act) {
                if (!act) return;
                std::string rf = configDir + "/recent.txt";
                std::vector<std::string> paths = ReadRecent(rf);
                paths.erase(std::remove(paths.begin(), paths.end(), e.Path), paths.end());
                if (FILE* f = fopen(rf.c_str(), "w")) { for (auto& p : paths) fprintf(f, "%s\n", p.c_str()); fclose(f); }
                changed = true;
                result = -3;
            });
        add("Retour", "", -1, [&](int, bool act) { if (act) result = -3; });

        GfxClear(g);
        Fill(g, 0, 0, 1280, 720, 35, 38, 44);
        const RomInfo& r = *e.Info;
        if (r.HasIcon) DrawIcon(g, e.Path, r.Icon, 50, 40, 160);
        else Fill(g, 50, 40, 160, 160, 60, 65, 74);
        int ty = 46;
        ty += DrawWrapped(g, r.Title, 236, ty, 560, Text, Big, 2);
        if (!r.Subtitle.empty()) ty += DrawWrapped(g, r.Subtitle, 236, ty + 2, 560, TextDim, Small, 2);
        DrawText(g, r.FileName, 236, ty + 8, TextDim, Small, 560);
        if (e.Favorite) DrawStar(g, 186, 44);
        DrawList(g, items, st, 30, 226, 760, 7, 56);

        // right: screenshot of the selected state (or of the automatic state)
        int shown = itemSlot.empty() ? -1 : itemSlot[std::clamp(st.Sel, 0, (int)itemSlot.size() - 1)];
        if (shown < 0 && hasAuto) shown = 0;
        Fill(g, 818, 40, 1, 600, 60, 65, 74);
        std::vector<uint32_t> px;
        int tw = 0, th = 0;
        std::string thumb = shown >= 0 ? StateFile(configDir, e.Path, shown) + ".thumb" : "";
        if (shown >= 0 && ReadThumb(thumb, px, tw, th))
        {
            DrawImage(g, thumb + FileDate(thumb), px.data(), tw, th, 924, 50, 256, 384, true);
            DrawText(g, (shown ? "État " + std::to_string(shown) : std::string("État automatique")) + "  " + FileDate(thumb), 1052, 446, TextDim, Small, 380, 1);
        }
        else
        {
            Fill(g, 924, 50, 256, 384, 46, 50, 58);
            DrawWrapped(g, shown >= 0 ? "Pas d'image pour cet état (sauvé par une ancienne version)" : "Aucun état sauvegardé", 944, 220, 216, TextDim, Small, 3);
        }
        DrawHints(g, {UI_A " Valider", UI_X " Favori", UI_Y " Réglages", UI_B " Retour"});
        GfxPresent(g);
        Trim();

        Input in;
        if (!Poll(in, 100)) continue;
        if (in.Button == BtnB) result = -3;
        else if (in.Button == BtnX) { for (size_t i = 0; i < items.size(); i++) if (items[i].Label == "Favori") items[i].Do(0, true); }
        else if (in.Button == BtnY) GameSettings(g, e, configDir, general);
        else ListInput(items, st, in, 30, 226, 760, 7, 56);
    }
    return result == -4 ? -3 : result;
}

HomeChoice RunHome(Gfx* g, const std::string& configDir, const std::string& startDir, const std::string& version)
{
    const std::string recentFile = configDir + "/recent.txt", homeFile = configDir + "/home.txt", uiFile = configDir + "/ui.txt";
    KeyValues home = ReadKeyValues(homeFile);
    std::string dir = home.count("dir") && !home["dir"].empty() ? home["dir"] : startDir;
    struct stat st0;
    if (stat(dir.c_str(), &st0) != 0) dir = startDir;
    // library folders: home.txt dirs=a|b; at first, the games folder of config.txt / sdmc:/roms/nds (not the whole card)
    std::vector<std::string> libDirs = home.count("dirs") ? SplitList(home["dirs"]) : std::vector<std::string>();
    if (!home.count("dirs") && startDir != "sdmc:/") libDirs.push_back(startDir);
    std::vector<std::string> favs = SplitList(home["favorites"]);
    bool gridView = !home.count("view") || home["view"] != "list";
    auto saveHome = [&]() { home["dirs"] = JoinList(libDirs); home["favorites"] = JoinList(favs); home["view"] = gridView ? "grid" : "list"; WriteKeyValues(homeFile, home); };

    std::vector<Entry> recent, library, files = ListDir(dir);
    auto loadRecent = [&]() {
        recent.clear();
        for (const std::string& p : ReadRecent(recentFile))
            if (IsFile(p))
            {
                Entry e;
                e.Path = p;
                e.Name = BaseName(p);
                e.Favorite = std::find(favs.begin(), favs.end(), p) != favs.end();
                recent.push_back(e);
            }
    };
    auto loadLibrary = [&]() {
        // first scan of a big folder: say so (titles are read from every ROM once, then cached)
        GfxClear(g);
        Fill(g, 0, 0, 1280, 720, 35, 38, 44);
        DrawText(g, "Lecture de la bibliothèque...", 640, 340, TextDim, Normal, 0, 1);
        GfxPresent(g);
        library = BuildLibrary(libDirs, configDir, favs);
    };
    loadRecent();
    loadLibrary();
    enum { TabRecent, TabLibrary, TabFiles, TabSettings, TabHelp, TabCount };
    int tab = recent.empty() ? TabLibrary : TabRecent;
    ListState recentSt, libSt, fileSt, settingsSt;
    KeyValues general = ReadKeyValues(uiFile);
    std::string note;
    static const char* tabs[] = {"Récents", "Bibliothèque", "Fichiers", "Réglages", "Aide"};

    while (appletMainLoop())
    {
        bool gameTab = tab == TabRecent || tab == TabLibrary || tab == TabFiles;
        std::vector<Entry>& list = tab == TabRecent ? recent : tab == TabLibrary ? library : files;
        ListState& st = tab == TabRecent ? recentSt : tab == TabLibrary ? libSt : fileSt;
        bool grid = gridView && tab != TabFiles;
        Entry* sel = (gameTab && !list.empty()) ? &list[std::clamp(st.Sel, 0, (int)list.size() - 1)] : nullptr;

        std::vector<std::string> hints;
        if (gameTab && sel)
        {
            hints.push_back(sel->Dir ? UI_A " Ouvrir" : UI_A " Choisir");
            if (!sel->Dir) hints.push_back(UI_X " Favori");
        }
        if (tab == TabFiles) hints.push_back(UI_Y " Ajouter le dossier");
        if (tab == TabFiles && dir.size() > 6) hints.push_back(UI_B " Parent");
        if (tab == TabSettings) hints.push_back(UI_A " / " UI_DPAD_LR " Changer");
        hints.push_back(UI_L " " UI_R " Onglet");
        hints.push_back(UI_PLUS " Quitter");

        GfxClear(g);
        Fill(g, 0, 0, 1280, 720, 35, 38, 44);
        DrawText(g, "melonDS", 44, 26, Text, Big);
        DrawText(g, version, 44 + TextWidth("melonDS", Big) + 10, 40, TextDim, Small);
        int tx = 1250;
        int tabX[TabCount], tabW[TabCount];
        for (int i = TabCount - 1; i >= 0; i--)
        {
            tabW[i] = TextWidth(tabs[i], Normal) + 30;
            tx -= tabW[i];
            tabX[i] = tx;
        }
        for (int i = 0; i < TabCount; i++)
        {
            DrawText(g, tabs[i], tabX[i] + 15, 30, i == tab ? AccentText : TextDim, Normal);
            if (i == tab) Fill(g, tabX[i] + 8, 76, tabW[i] - 16, 4, 0, 200, 235);
        }
        Fill(g, 30, 88, 1220, 1, 90, 96, 106);
        Fill(g, 30, 646, 1220, 1, 90, 96, 106);

        std::vector<Item> settingItems;
        std::string footer;
        if (gameTab)
        {
            if (list.empty())
                DrawWrapped(g, tab == TabRecent ? "Aucun jeu lancé récemment." :
                               tab == TabLibrary ? (libDirs.empty() ? "La bibliothèque est vide : dans l'onglet « Fichiers », ouvrez le dossier de vos jeux et appuyez sur " UI_Y "." : "Aucun jeu (.nds) dans les dossiers de la bibliothèque.")
                               : "Aucun jeu (.nds) dans ce dossier.", 80, 140, 1100, TextDim, Normal);
            if (grid) DrawGrid(g, list, st, configDir);
            else
            {
                DrawGameList(g, list, st, configDir);
                DrawDetails(g, sel, configDir);
            }
            footer = tab == TabFiles ? dir : tab == TabLibrary ? std::to_string(library.size()) + " jeu(x)" : std::to_string(recent.size()) + " jeu(x) récent(s)";
        }
        else if (tab == TabSettings)
        {
            auto save = [&]() { note = WriteKeyValues(uiFile, general) ? "Enregistré : réglages de tous les jeux sans réglages propres." : "Impossible d'écrire " + uiFile; };
            Item h; h.Label = "Bibliothèque"; h.Header = true; settingItems.push_back(h);
            settingItems.push_back({"Présentation", gridView ? "grille" : "liste", [&](int, bool) { gridView = !gridView; saveHome(); }});
            for (size_t i = 0; i < libDirs.size(); i++)
                settingItems.push_back({libDirs[i], "retirer", [&, i](int, bool act) {
                    if (!act) return;
                    libDirs.erase(libDirs.begin() + i);
                    saveHome();
                    loadLibrary();
                    note = "Dossier retiré de la bibliothèque.";
                }});
            for (Item& it : SettingItems(general, nullptr, true, save)) settingItems.push_back(it);
            DrawList(g, settingItems, settingsSt, ListX, ListY, 1200, 9, 58);
            footer = note.empty() ? "Un jeu peut avoir ses propres réglages : sa page, puis « Réglages de ce jeu »" : note;
        }
        else
            DrawHelp(g);
        if (!footer.empty()) DrawText(g, footer, 50, 666, note.empty() || gameTab ? TextDim : AccentText, Small, HintsLeft(hints) - 90);
        DrawHints(g, hints);
        GfxPresent(g);
        Trim();

        Input in;
        if (!Poll(in, 100)) continue;
        if (in.Button == BtnPlus || in.Button == BtnMinus) return HomeChoice();
        if (in.Button == BtnL || in.Button == BtnR || in.Button == BtnZL || in.Button == BtnZR)
        {
            tab = (tab + ((in.Button == BtnL || in.Button == BtnZL) ? TabCount - 1 : 1)) % TabCount;
            note.clear();
            continue;
        }
        if (in.Tap && in.TouchY < 88)
        {
            for (int i = 0; i < TabCount; i++)
                if (in.TouchX >= tabX[i] && in.TouchX < tabX[i] + tabW[i]) tab = i;
            continue;
        }
        if (tab == TabSettings) { ListInput(settingItems, settingsSt, in, ListX, ListY, 1200, 9, 58); continue; }
        if (tab == TabHelp) continue;

        int n = (int)list.size();
        // A on a game: its page (continue, states, settings); on a folder: open it
        auto open = [&](Entry& e) -> HomeChoice {
            HomeChoice c;
            if (e.Dir)
            {
                dir = e.Path;
                files = ListDir(dir);
                fileSt = ListState();
                home["dir"] = dir;
                WriteKeyValues(homeFile, home);
                ForgetIcons();
                return c;
            }
            bool changed = false;
            int slot = GamePage(g, e, configDir, general, tab == TabRecent, favs, homeFile, home, changed);
            if (changed)
            {
                std::string keep = e.Path;
                loadRecent();
                for (Entry& x : library) x.Favorite = std::find(favs.begin(), favs.end(), x.Path) != favs.end();
                std::stable_sort(library.begin(), library.end(), [](const Entry& a, const Entry& b) {
                    if (a.Favorite != b.Favorite) return a.Favorite;
                    return LowerCase(a.Title) < LowerCase(b.Title);
                });
                for (int i = 0; i < (int)library.size(); i++) if (library[i].Path == keep) libSt.Sel = i;
            }
            if (slot >= -1) { c.Path = e.Path; c.LoadSlot = slot; }
            return c;
        };
        auto toggleFav = [&](Entry& e) {
            if (e.Dir) return;
            bool on = std::find(favs.begin(), favs.end(), e.Path) == favs.end();
            favs.erase(std::remove(favs.begin(), favs.end(), e.Path), favs.end());
            if (on) favs.push_back(e.Path);
            saveHome();
            std::string keep = e.Path;
            for (Entry& x : library) x.Favorite = std::find(favs.begin(), favs.end(), x.Path) != favs.end();
            for (Entry& x : recent) x.Favorite = std::find(favs.begin(), favs.end(), x.Path) != favs.end();
            std::stable_sort(library.begin(), library.end(), [](const Entry& a, const Entry& b) {
                if (a.Favorite != b.Favorite) return a.Favorite;
                return LowerCase(a.Title) < LowerCase(b.Title);
            });
            if (tab == TabLibrary) for (int i = 0; i < (int)library.size(); i++) if (library[i].Path == keep) libSt.Sel = i;
        };
        const int step = grid ? GridCols : 1, page = grid ? GridCols * GridRows : Rows;
        if (in.Button == BtnUp && n)
            st.Sel = grid ? (st.Sel >= step ? st.Sel - step : st.Sel) : (st.Sel + n - 1) % n;
        else if (in.Button == BtnDown && n)
        {
            if (!grid) st.Sel = (st.Sel + 1) % n;
            else if (st.Sel / step < (n - 1) / step) st.Sel = std::min(st.Sel + step, n - 1);   // next row (last game if shorter)
        }
        else if (in.Button == BtnLeft && n) st.Sel = grid ? std::max(st.Sel - 1, 0) : std::max(st.Sel - page, 0);
        else if (in.Button == BtnRight && n) st.Sel = grid ? std::min(st.Sel + 1, n - 1) : std::min(st.Sel + page, n - 1);
        else if (in.Button == BtnA && sel) { HomeChoice c = open(*sel); if (!c.Path.empty()) return c; }
        else if (in.Button == BtnX && sel) toggleFav(*sel);
        else if (in.Button == BtnB && tab == TabFiles && !files.empty() && files[0].Up) open(files[0]);
        else if (in.Button == BtnY && tab == TabFiles)
        {
            if (std::find(libDirs.begin(), libDirs.end(), dir) == libDirs.end()) libDirs.push_back(dir);
            saveHome();
            loadLibrary();
            tab = TabLibrary;
        }
        else if (in.DragY)
        {
            static int acc = 0;
            const int unit = grid ? TileH + TileGap : RowH;
            acc += in.DragY;
            int rowsTotal = grid ? (n + GridCols - 1) / GridCols : n, vis = grid ? GridRows : Rows;
            while (acc <= -unit) { acc += unit; st.Top = std::min(st.Top + 1, std::max(rowsTotal - vis, 0)); }
            while (acc >= unit) { acc -= unit; st.Top = std::max(st.Top - 1, 0); }
            if (grid) st.Sel = std::clamp(st.Sel, st.Top * GridCols, std::min((st.Top + GridRows) * GridCols - 1, std::max(n - 1, 0)));
            else st.Sel = std::clamp(st.Sel, st.Top, std::max(st.Top + Rows - 1, 0));
        }
        else if (in.Tap && n)
        {
            // tap: select, tap again: open
            int i = -1;
            if (grid && in.TouchY >= GridY && in.TouchX >= GridX)
            {
                int c = (in.TouchX - GridX) / (TileW + TileGap), r = (in.TouchY - GridY) / (TileH + TileGap);
                if (c < GridCols && r < GridRows) i = (st.Top + r) * GridCols + c;
            }
            else if (!grid && in.TouchX >= ListX && in.TouchX < ListX + ListW && in.TouchY >= ListY && in.TouchY < ListY + Rows * RowH)
                i = st.Top + (in.TouchY - ListY) / RowH;
            else if (!grid && in.TouchX >= 810 && in.TouchY >= ListY && in.TouchY < 646 && sel)
                i = st.Sel;
            if (i >= 0 && i < n)
            {
                if (i == st.Sel) { HomeChoice c = open(list[i]); if (!c.Path.empty()) return c; }
                else st.Sel = i;
            }
        }
    }
    return HomeChoice();
}
