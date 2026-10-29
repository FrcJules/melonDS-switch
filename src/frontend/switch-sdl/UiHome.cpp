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
        {UI_L " + " UI_R " + " UI_PLUS " + " UI_MINUS, "Quitter (état sauvegardé, repris au lancement)"},
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

std::string RunHome(Gfx* g, const std::string& configDir, const std::string& startDir, const std::string& version)
{
    const std::string recentFile = configDir + "/recent.txt", homeFile = configDir + "/home.txt", uiFile = configDir + "/ui.txt";
    KeyValues home = ReadKeyValues(homeFile);
    std::string dir = home.count("dir") && !home["dir"].empty() ? home["dir"] : startDir;
    struct stat st0;
    if (stat(dir.c_str(), &st0) != 0) dir = startDir;

    std::vector<Entry> recent, files = ListDir(dir);
    auto loadRecent = [&]() {
        recent.clear();
        for (const std::string& p : ReadRecent(recentFile))
            if (IsFile(p))
            {
                Entry e;
                e.Path = p;
                e.Name = BaseName(p);
                recent.push_back(e);
            }
    };
    loadRecent();
    int tab = recent.empty() ? 1 : 0;
    ListState recentSt, fileSt, settingsSt;
    KeyValues general = ReadKeyValues(uiFile);
    std::string settingsNote;
    static const char* tabs[] = {"Récents", "Jeux", "Réglages", "Aide"};

    while (appletMainLoop())
    {
        std::vector<Entry>& list = tab == 0 ? recent : files;
        ListState& st = tab == 0 ? recentSt : fileSt;
        Entry* sel = (tab <= 1 && !list.empty()) ? &list[std::clamp(st.Sel, 0, (int)list.size() - 1)] : nullptr;

        std::vector<std::string> hints;
        if (tab <= 1 && sel)
        {
            hints.push_back(sel->Dir ? UI_A " Ouvrir" : UI_A " Jouer");
            if (!sel->Dir) hints.push_back(UI_Y " Réglages du jeu");
            if (tab == 0) hints.push_back(UI_X " Retirer");
        }
        if (tab == 1 && dir.size() > 6) hints.push_back(UI_B " Dossier parent");
        if (tab == 2) hints.push_back(UI_A " / " UI_DPAD_LR " Changer");
        hints.push_back(UI_L " " UI_R " Onglet");
        hints.push_back(UI_PLUS " Quitter");

        GfxClear(g);
        Fill(g, 0, 0, 1280, 720, 35, 38, 44);
        DrawText(g, "melonDS Switch", 50, 24, Text, Big);
        DrawText(g, version, 50 + TextWidth("melonDS Switch", Big) + 12, 38, TextDim, Small);
        // tabs on the right of the header
        int tx = 1240;
        int tabX[4], tabW[4];
        for (int i = 3; i >= 0; i--)
        {
            tabW[i] = TextWidth(tabs[i], Normal) + 36;
            tx -= tabW[i];
            tabX[i] = tx;
        }
        for (int i = 0; i < 4; i++)
        {
            DrawText(g, tabs[i], tabX[i] + 18, 30, i == tab ? AccentText : TextDim, Normal);
            if (i == tab) Fill(g, tabX[i] + 10, 76, tabW[i] - 20, 4, 0, 200, 235);
        }
        Fill(g, 30, 88, 1220, 1, 90, 96, 106);
        Fill(g, 30, 646, 1220, 1, 90, 96, 106);

        std::vector<Item> settingItems;
        if (tab <= 1)
        {
            if (list.empty())
                DrawWrapped(g, tab == 0 ? "Aucun jeu lancé récemment. Choisissez un jeu dans l'onglet « Jeux »." : "Aucun jeu (.nds) dans ce dossier.", ListX + 30, ListY + 30, 720, TextDim, Normal);
            DrawGameList(g, list, st, configDir);
            DrawDetails(g, sel, configDir);
            DrawText(g, tab == 1 ? dir : std::to_string(recent.size()) + " jeu(x) récent(s)", 50, 666, TextDim, Small, HintsLeft(hints) - 90);
        }
        else if (tab == 2)
        {
            auto save = [&]() { settingsNote = WriteKeyValues(uiFile, general) ? "Enregistré : réglages de tous les jeux sans réglages propres." : "Impossible d'écrire " + uiFile; };
            settingItems = SettingItems(general, nullptr, true, save);
            DrawList(g, settingItems, settingsSt, ListX, ListY, 1200, 9, 58);
            DrawText(g, settingsNote.empty() ? "Un jeu peut avoir ses propres réglages : " UI_Y " dans la liste" : settingsNote, 50, 666, settingsNote.empty() ? TextDim : AccentText, Small, HintsLeft(hints) - 90);
        }
        else
            DrawHelp(g);
        DrawHints(g, hints);
        GfxPresent(g);
        Trim();

        Input in;
        if (!Poll(in, 100)) continue;
        if (in.Button == BtnPlus || in.Button == BtnMinus) return "";
        if (in.Button == BtnL || in.Button == BtnR || in.Button == BtnZL || in.Button == BtnZR)
        {
            tab = (tab + ((in.Button == BtnL || in.Button == BtnZL) ? 3 : 1)) % 4;
            settingsNote.clear();
            continue;
        }
        if (in.Tap && in.TouchY < 88)
        {
            for (int i = 0; i < 4; i++)
                if (in.TouchX >= tabX[i] && in.TouchX < tabX[i] + tabW[i]) tab = i;
            continue;
        }
        if (tab == 2) { ListInput(settingItems, settingsSt, in, ListX, ListY, 1200, 9, 58); continue; }
        if (tab == 3) continue;

        // game lists
        int n = (int)list.size();
        auto open = [&](Entry& e) -> bool {
            if (e.Dir)
            {
                dir = e.Path;
                files = ListDir(dir);
                fileSt = ListState();
                home["dir"] = dir;
                WriteKeyValues(homeFile, home);
                ForgetIcons();
                return false;
            }
            return true;
        };
        if (in.Button == BtnUp && n) st.Sel = (st.Sel + n - 1) % n;
        else if (in.Button == BtnDown && n) st.Sel = (st.Sel + 1) % n;
        else if (in.Button == BtnLeft && n) st.Sel = std::max(st.Sel - Rows, 0);
        else if (in.Button == BtnRight && n) st.Sel = std::min(st.Sel + Rows, n - 1);
        else if (in.Button == BtnA && sel) { if (open(*sel)) return sel->Path; }
        else if (in.Button == BtnB && tab == 1 && !files.empty() && files[0].Up) open(files[0]);
        else if (in.Button == BtnY && sel && !sel->Dir) GameSettings(g, *sel, configDir, general);
        else if (in.Button == BtnX && sel && tab == 0)
        {
            std::vector<std::string> paths = ReadRecent(recentFile);
            paths.erase(std::remove(paths.begin(), paths.end(), sel->Path), paths.end());
            if (FILE* f = fopen(recentFile.c_str(), "w")) { for (auto& p : paths) fprintf(f, "%s\n", p.c_str()); fclose(f); }
            loadRecent();
        }
        else if (in.DragY && in.TouchX < ListX + ListW)
        {
            static int acc = 0;
            acc += in.DragY;
            while (acc <= -RowH) { acc += RowH; st.Top = std::min(st.Top + 1, std::max(n - Rows, 0)); }
            while (acc >= RowH) { acc -= RowH; st.Top = std::max(st.Top - 1, 0); }
            st.Sel = std::clamp(st.Sel, st.Top, std::max(st.Top + Rows - 1, 0));
        }
        else if (in.Tap && in.TouchX >= ListX && in.TouchX < ListX + ListW && in.TouchY >= ListY && in.TouchY < ListY + Rows * RowH)
        {
            // tap: select, tap again: open / play
            int i = st.Top + (in.TouchY - ListY) / RowH;
            if (i < n)
            {
                if (i == st.Sel) { if (open(list[i])) return list[i].Path; }
                else st.Sel = i;
            }
        }
        else if (in.Tap && in.TouchX >= 810 && in.TouchY >= ListY && in.TouchY < 646 && sel)
        {
            if (open(*sel)) return sel->Path;   // tap on the details: play
        }
    }
    return "";
}
