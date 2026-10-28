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

#include <string.h>

#include <algorithm>
#include <map>

#include <switch.h>

#include "Ui.h"

namespace Ui
{

const SDL_Color Text {240, 242, 245, 255}, TextDim {150, 158, 170, 255}, AccentText {0, 200, 235, 255}, Warn {255, 205, 90, 255};

static TTF_Font* Fonts[3] {};      // standard shared font: small, normal, big
static TTF_Font* ExtFonts[3] {};   // "Nintendo extended" shared font: button icons
static const int FontPx[3] = {22, 28, 38};

struct CachedText { GfxTex Tex; int Used = 0; };
static std::map<std::string, CachedText> TextCache;
static std::map<std::string, GfxTex> IconCache;
static int Frame = 0;

bool Init()
{
    PlFontData std {}, ext {};
    bool okStd = R_SUCCEEDED(plGetSharedFontByType(&std, PlSharedFontType_Standard));
    bool okExt = R_SUCCEEDED(plGetSharedFontByType(&ext, PlSharedFontType_NintendoExt));
    for (int i = 0; i < 3; i++)
    {
        if (okStd) Fonts[i] = TTF_OpenFontRW(SDL_RWFromConstMem(std.address, std.size), 1, FontPx[i]);
        if (okExt) ExtFonts[i] = TTF_OpenFontRW(SDL_RWFromConstMem(ext.address, ext.size), 1, FontPx[i]);
    }
    return Fonts[1] != nullptr;
}

void Shutdown()
{
    for (auto& kv : TextCache) GfxTexDestroy(&kv.second.Tex);
    TextCache.clear();
    ForgetIcons();
    for (int i = 0; i < 3; i++)
    {
        if (Fonts[i]) TTF_CloseFont(Fonts[i]);
        if (ExtFonts[i]) TTF_CloseFont(ExtFonts[i]);
        Fonts[i] = ExtFonts[i] = nullptr;
    }
}

void Trim()
{
    // textures not drawn for ~10 s (at the menu's ~20-60 redraws per second) are freed
    Frame++;
    if (TextCache.size() < 200) return;
    for (auto it = TextCache.begin(); it != TextCache.end();)
    {
        if (Frame - it->second.Used > 300) { GfxTexDestroy(&it->second.Tex); it = TextCache.erase(it); }
        else ++it;
    }
}

// splits UTF-8 text into runs of normal text and of button icons (private use area U+E000-U+E1FF)
struct Run { std::string Text; bool Icon; };
static std::vector<Run> Runs(const std::string& s)
{
    std::vector<Run> out;
    for (size_t i = 0; i < s.size();)
    {
        unsigned char c = s[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        bool icon = false;
        if (len == 3 && i + 2 < s.size())
        {
            unsigned cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
            icon = cp >= 0xE000 && cp < 0xE200;
        }
        if (out.empty() || out.back().Icon != icon) out.push_back({"", icon});
        out.back().Text += s.substr(i, len);
        i += len;
    }
    return out;
}

static TTF_Font* FontFor(const Run& r, FontSize size)
{
    return (r.Icon && ExtFonts[size]) ? ExtFonts[size] : Fonts[size];
}

int TextWidth(const std::string& text, FontSize size)
{
    int w = 0;
    for (const Run& r : Runs(text))
    {
        int rw = 0, rh = 0;
        if (TTF_Font* f = FontFor(r, size)) TTF_SizeUTF8(f, r.Text.c_str(), &rw, &rh);
        w += rw;
    }
    return w;
}

static std::string Ellipsize(const std::string& text, FontSize size, int maxWidth)
{
    if (maxWidth <= 0 || TextWidth(text, size) <= maxWidth) return text;
    std::string s = text;
    while (!s.empty())
    {
        // drop one UTF-8 character
        size_t i = s.size() - 1;
        while (i > 0 && (s[i] & 0xC0) == 0x80) i--;
        s.erase(i);
        if (TextWidth(s + "...", size) <= maxWidth) return s + "...";
    }
    return "";
}

static const GfxTex* RunTexture(const Run& r, FontSize size, SDL_Color c)
{
    char key[32];
    snprintf(key, sizeof(key), "%d/%02x%02x%02x%02x/%d/", (int)size, c.r, c.g, c.b, c.a, r.Icon ? 1 : 0);
    std::string k = key + r.Text;
    auto it = TextCache.find(k);
    if (it != TextCache.end()) { it->second.Used = Frame; return &it->second.Tex; }
    TTF_Font* f = FontFor(r, size);
    if (!f) return nullptr;
    SDL_Surface* surf = TTF_RenderUTF8_Blended(f, r.Text.c_str(), c);
    if (!surf) return nullptr;
    CachedText ct;
    ct.Tex = GfxTexFromSurface(surf);
    ct.Used = Frame;
    SDL_FreeSurface(surf);
    return &(TextCache[k] = ct).Tex;
}

int DrawText(Gfx* g, const std::string& text, int x, int y, SDL_Color c, FontSize size, int maxWidth, int align)
{
    if (text.empty()) return 0;
    std::string s = Ellipsize(text, size, maxWidth);
    int w = TextWidth(s, size);
    if (align == 1) x -= w / 2;
    else if (align == 2) x -= w;
    bool wasBlend = g->Blend;
    GfxBlendMode(g, true);
    for (const Run& r : Runs(s))
    {
        const GfxTex* t = RunTexture(r, size, c);
        if (!t || !t->Id) continue;
        // icons of the extended font sit a little lower than the text: nudge them up
        SDL_Rect d {x, y - (r.Icon ? FontPx[size] / 10 : 0), t->W, t->H};
        GfxCopy(g, t, nullptr, &d, 2);
        x += t->W;
    }
    GfxBlendMode(g, wasBlend);
    return w;
}

int DrawWrapped(Gfx* g, const std::string& text, int x, int y, int width, SDL_Color c, FontSize size, int maxLines)
{
    int lineH = FontPx[size] * 13 / 10, lines = 0;
    size_t p = 0;
    while (p < text.size())
    {
        size_t nl = text.find('\n', p);
        std::string para = text.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
        p = nl == std::string::npos ? text.size() : nl + 1;
        // greedy word wrap
        std::string line;
        size_t q = 0;
        do
        {
            size_t sp = para.find(' ', q);
            std::string word = para.substr(q, sp == std::string::npos ? std::string::npos : sp - q);
            q = sp == std::string::npos ? para.size() : sp + 1;
            std::string cand = line.empty() ? word : line + " " + word;
            if (!line.empty() && TextWidth(cand, size) > width)
            {
                if (maxLines && lines == maxLines - 1) { DrawText(g, line + " " + word, x, y + lines * lineH, c, size, width); return (lines + 1) * lineH; }
                DrawText(g, line, x, y + lines * lineH, c, size, width);
                lines++;
                line = word;
            }
            else line = cand;
        } while (q < para.size());
        if (maxLines && lines >= maxLines) break;
        DrawText(g, line, x, y + lines * lineH, c, size, width);
        lines++;
    }
    return lines * lineH;
}

void Fill(Gfx* g, int x, int y, int w, int h, int r, int gr, int b, int a)
{
    bool wasBlend = g->Blend;
    if (a < 255) GfxBlendMode(g, true);
    GfxColor(g, r, gr, b, a);
    SDL_Rect rc {x, y, w, h};
    GfxFill(g, &rc);
    if (a < 255) GfxBlendMode(g, wasBlend);
}

void SetColorBg(Gfx* g) { GfxColor(g, 35, 38, 44, 255); }

void DrawHints(Gfx* g, const std::vector<std::string>& hints, int y)
{
    int x = 1240;
    for (auto it = hints.rbegin(); it != hints.rend(); ++it)
    {
        int w = TextWidth(*it, Small);
        DrawText(g, *it, x - w, y, Text, Small);
        x -= w + 36;
    }
}

void DrawFrame(Gfx* g, const std::string& title, const std::vector<std::string>& hints)
{
    GfxClear(g);
    Fill(g, 0, 0, 1280, 720, 35, 38, 44);
    DrawText(g, title, 60, 26, Text, Big);
    Fill(g, 30, 88, 1220, 1, 90, 96, 106);
    Fill(g, 30, 646, 1220, 1, 90, 96, 106);
    DrawHints(g, hints);
}

// ---- input ----
static int HeldButton = -1;
static Uint32 HeldSince = 0, LastRepeat = 0;
static bool FingerDown = false, FingerMoved = false;
static float FingerX0 = 0, FingerY0 = 0, FingerLastY = 0;

static int MapButton(int b)
{
    // SDL (libnx joystick): 16-19 = left stick as directions, same as the D-pad
    switch (b)
    {
    case 16: return BtnLeft;
    case 17: return BtnUp;
    case 18: return BtnRight;
    case 19: return BtnDown;
    }
    return b <= BtnDown ? b : BtnNone;
}

bool Poll(Input& in, int timeoutMs)
{
    in = Input();
    in.Touching = FingerDown;
    SDL_Event ev;
    Uint32 start = SDL_GetTicks();
    for (;;)
    {
        int wait = timeoutMs - (int)(SDL_GetTicks() - start);
        // repeat of a held direction: 400 ms, then every 90 ms
        if (HeldButton >= 0)
        {
            Uint32 now = SDL_GetTicks();
            if (now - HeldSince >= 400 && now - LastRepeat >= 90)
            {
                LastRepeat = now;
                in.Button = HeldButton;
                return true;
            }
            wait = std::min(wait, 30);
        }
        if (wait <= 0) return false;
        if (!SDL_WaitEventTimeout(&ev, wait)) continue;
        switch (ev.type)
        {
        case SDL_JOYBUTTONDOWN:
        {
            int b = MapButton(ev.jbutton.button);
            if (b == BtnNone) break;
            in.Button = b;
            if (b >= BtnLeft) { HeldButton = b; HeldSince = LastRepeat = SDL_GetTicks(); }
            return true;
        }
        case SDL_JOYBUTTONUP:
            if (MapButton(ev.jbutton.button) == HeldButton) HeldButton = -1;
            break;
        case SDL_FINGERDOWN:
            FingerDown = true; FingerMoved = false;
            FingerX0 = ev.tfinger.x * 1280; FingerY0 = FingerLastY = ev.tfinger.y * 720;
            in.Touching = true; in.TouchX = (int)FingerX0; in.TouchY = (int)FingerY0;
            return true;
        case SDL_FINGERMOTION:
        {
            if (!FingerDown) break;
            float x = ev.tfinger.x * 1280, y = ev.tfinger.y * 720;
            if (std::abs(x - FingerX0) > 20 || std::abs(y - FingerY0) > 20) FingerMoved = true;
            in.Touching = true; in.TouchX = (int)x; in.TouchY = (int)y;
            in.DragY = FingerMoved ? (int)(y - FingerLastY) : 0;
            if (FingerMoved) FingerLastY = y;
            return true;
        }
        case SDL_FINGERUP:
            if (!FingerDown) break;
            FingerDown = false;
            in.Touching = false;
            in.TouchX = (int)(ev.tfinger.x * 1280); in.TouchY = (int)(ev.tfinger.y * 720);
            in.Tap = !FingerMoved;
            return true;
        }
    }
}

// ---- list ----
static void ScrollTo(ListState& st, int count, int rows)
{
    st.Sel = std::clamp(st.Sel, 0, std::max(count - 1, 0));
    if (st.Sel < st.Top) st.Top = st.Sel;
    if (st.Sel >= st.Top + rows) st.Top = st.Sel - rows + 1;
    st.Top = std::clamp(st.Top, 0, std::max(count - rows, 0));
}

static void Step(const std::vector<Item>& items, ListState& st, int dir)
{
    int n = (int)items.size();
    for (int k = 0; k < n; k++)
    {
        st.Sel = (st.Sel + dir + n) % n;
        if (!items[st.Sel].Header) return;
    }
}

void DrawList(Gfx* g, const std::vector<Item>& items, ListState& st, int x, int y, int w, int rows, int rowH)
{
    int n = (int)items.size();
    if (n && items[std::clamp(st.Sel, 0, n - 1)].Header) Step(items, st, 1);
    ScrollTo(st, n, rows);
    for (int i = 0; i < rows && st.Top + i < n; i++)
    {
        const Item& it = items[st.Top + i];
        int ry = y + i * rowH;
        if (it.Header)
        {
            DrawText(g, it.Label, x + 16, ry + rowH - 34, AccentText, Small, w - 32);
            continue;
        }
        bool sel = st.Top + i == st.Sel;
        if (sel)
        {
            // Switch-style selection: bright frame
            Fill(g, x, ry + 2, w, rowH - 4, 50, 56, 66);
            Fill(g, x, ry + 2, w, 3, 0, 200, 235);
            Fill(g, x, ry + rowH - 5, w, 3, 0, 200, 235);
            Fill(g, x, ry + 2, 3, rowH - 4, 0, 200, 235);
            Fill(g, x + w - 3, ry + 2, 3, rowH - 4, 0, 200, 235);
        }
        else
            Fill(g, x + 16, ry + rowH - 1, w - 32, 1, 60, 65, 74);
        int ty = ry + (rowH - 36) / 2;
        int vw = it.Value.empty() ? 0 : TextWidth(it.Value, Normal);
        DrawText(g, it.Label, x + 20, ty, Text, Normal, w - 60 - std::min(vw, w / 2));
        if (!it.Value.empty())
            DrawText(g, it.Value, x + w - 20, ty, AccentText, Normal, w / 2, 2);
    }
    // scroll bar
    if (n > rows)
    {
        int h = rows * rowH;
        Fill(g, x + w + 8, y, 4, h, 60, 65, 74);
        Fill(g, x + w + 8, y + h * st.Top / n, 4, std::max(h * rows / n, 20), 150, 158, 170);
    }
}

bool ListInput(const std::vector<Item>& items, ListState& st, const Input& in, int x, int y, int w, int rows, int rowH)
{
    int n = (int)items.size();
    if (!n) return false;
    switch (in.Button)
    {
    case BtnUp: Step(items, st, -1); ScrollTo(st, n, rows); return true;
    case BtnDown: Step(items, st, 1); ScrollTo(st, n, rows); return true;
    case BtnLeft: case BtnRight:
        if (items[st.Sel].Do && !items[st.Sel].Value.empty()) { items[st.Sel].Do(in.Button == BtnRight ? 1 : -1, false); return true; }
        return false;
    case BtnA:
        if (items[st.Sel].Do) items[st.Sel].Do(0, true);
        return true;
    }
    if (in.DragY && in.TouchX >= x && in.TouchX < x + w)
    {
        // drag: scroll by whole rows
        static int acc = 0;
        acc += in.DragY;
        while (acc <= -rowH) { acc += rowH; st.Top = std::min(st.Top + 1, std::max(n - rows, 0)); }
        while (acc >= rowH) { acc -= rowH; st.Top = std::max(st.Top - 1, 0); }
        st.Sel = std::clamp(st.Sel, st.Top, st.Top + rows - 1);
        return true;
    }
    if (in.Tap && in.TouchX >= x && in.TouchX < x + w && in.TouchY >= y && in.TouchY < y + rows * rowH)
    {
        int i = st.Top + (in.TouchY - y) / rowH;
        if (i < n && !items[i].Header)
        {
            st.Sel = i;
            if (items[i].Do) items[i].Do(0, true);
        }
        return true;
    }
    return false;
}

void Message(Gfx* g, const std::string& title, const std::string& body, const std::string& button)
{
    for (;;)
    {
        DrawFrame(g, title, {UI_A " " + button});
        DrawWrapped(g, body, 60, 130, 1160, Text, Normal);
        GfxPresent(g);
        Trim();
        Input in;
        if (!appletMainLoop()) return;
        if (!Poll(in, 100)) continue;
        if (in.Button == BtnA || in.Button == BtnB || in.Button == BtnPlus || in.Tap) return;
    }
}

void DrawIcon(Gfx* g, const std::string& key, const uint32_t* rgba, int x, int y, int size)
{
    auto it = IconCache.find(key);
    if (it == IconCache.end())
    {
        GfxTex t = GfxTexCreate(32, 32, false);
        glBindTexture(GL_TEXTURE_2D, t.Id);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        it = IconCache.emplace(key, t).first;
    }
    SDL_Rect d {x, y, size, size};
    bool wasBlend = g->Blend;
    GfxBlendMode(g, true);
    GfxCopy(g, &it->second, nullptr, &d, 2);
    GfxBlendMode(g, wasBlend);
}

void ForgetIcons()
{
    for (auto& kv : IconCache) GfxTexDestroy(&kv.second);
    IconCache.clear();
}

}
