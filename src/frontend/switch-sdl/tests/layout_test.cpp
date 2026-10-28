// Host test of the Switch screen layouts (no console needed):
//   g++ -std=c++17 -O1 -o /tmp/layout_test tests/layout_test.cpp Layout.cpp ../ScreenLayout.cpp && /tmp/layout_test
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <vector>

#include "../Layout.h"

static int Failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); Failures++; } } while (0)

struct Box { float x0, y0, x1, y1; };

static Box Bounds(const ScreenQuad& q)
{
    float xs[4], ys[4];
    float px[4] = {0, 256, 0, 256}, py[4] = {0, 0, 192, 192};
    for (int i = 0; i < 4; i++)
    {
        xs[i] = q.Mtx[0] * px[i] + q.Mtx[2] * py[i] + q.Mtx[4];
        ys[i] = q.Mtx[1] * px[i] + q.Mtx[3] * py[i] + q.Mtx[5];
    }
    Box b {xs[0], ys[0], xs[0], ys[0]};
    for (int i = 1; i < 4; i++) { b.x0 = fminf(b.x0, xs[i]); b.x1 = fmaxf(b.x1, xs[i]); b.y0 = fminf(b.y0, ys[i]); b.y1 = fmaxf(b.y1, ys[i]); }
    return b;
}

static bool Near(float a, float b) { return fabsf(a - b) < 0.6f; }
static bool Inside(const Box& b) { return b.x0 > -0.6f && b.y0 > -0.6f && b.x1 < 1280.6f && b.y1 < 720.6f; }

static const ScreenQuad* Find(const SwitchLayout& l, int screen, int nth = 0)
{
    for (int i = 0; i < l.NumQuads; i++)
        if (l.Quads[i].Screen == screen && nth-- == 0) return &l.Quads[i];
    return nullptr;
}

static void Dump(const char* name, const SwitchLayout& l)
{
    printf("%s:", name);
    for (int i = 0; i < l.NumQuads; i++)
    {
        Box b = Bounds(l.Quads[i]);
        printf("  [%s %.0f,%.0f-%.0f,%.0f]", l.Quads[i].Screen ? "bas" : "haut", b.x0, b.y0, b.x1, b.y1);
    }
    printf("\n");
}

int main()
{
    SwitchLayout l;
    DisplayOptions o;

    // vertical, same size: identical to the 0.2 "stacked" layout (480x360 each, centred)
    l.Setup(o, false);
    Dump("verticale", l);
    CHECK(l.NumQuads == 2);
    {
        Box t = Bounds(*Find(l, 0)), b = Bounds(*Find(l, 1));
        CHECK(Near(t.x0, 400) && Near(t.y0, 0) && Near(t.x1, 880) && Near(t.y1, 360));
        CHECK(Near(b.x0, 400) && Near(b.y0, 360) && Near(b.x1, 880) && Near(b.y1, 720));
        int x, y;
        CHECK(l.Touch(400, 360, x, y) && x == 0 && y == 0);
        CHECK(l.Touch(879, 719, x, y) && x == 255 && y == 191);
        CHECK(!l.Touch(640, 100, x, y));   // on the top screen
        float cx[3], cy[3], cs[3];
        CHECK(l.BottomToScreen(128, 96, cx, cy, cs) == 1 && Near(cx[0], 640) && Near(cy[0], 540));
    }

    // swapped: bottom screen above
    o.Swap = true;
    l.Setup(o, false);
    Dump("verticale inversée", l);
    CHECK(Near(Bounds(*Find(l, 1)).y0, 0) && Near(Bounds(*Find(l, 0)).y0, 360));
    o.Swap = false;

    // horizontal: identical to the 0.2 "side by side" layout (640x480 each)
    o.Layout = 1;
    l.Setup(o, false);
    Dump("horizontale", l);
    {
        Box t = Bounds(*Find(l, 0)), b = Bounds(*Find(l, 1));
        CHECK(Near(t.x0, 0) && Near(t.y0, 120) && Near(t.x1, 640) && Near(t.y1, 600));
        CHECK(Near(b.x0, 640) && Near(b.x1, 1280));
    }

    // hybrid: big top screen on the left, both screens small on the right, stacked
    o.Layout = 2;
    l.Setup(o, false);
    Dump("hybride", l);
    CHECK(l.NumQuads == 3);
    {
        const ScreenQuad* big = nullptr;
        for (int i = 0; i < l.NumQuads; i++)
        {
            Box b = Bounds(l.Quads[i]);
            CHECK(Inside(b));
            if (b.x1 - b.x0 > 600) big = &l.Quads[i];
        }
        CHECK(big && big->Screen == 0);
        Box bb = Bounds(*big);
        Box st {}, sb {};
        for (int i = 0; i < l.NumQuads; i++)
            if (&l.Quads[i] != big) (l.Quads[i].Screen ? sb : st) = Bounds(l.Quads[i]);
        CHECK(Near(bb.x0, 0) || bb.x0 >= 0);
        CHECK(st.x0 >= bb.x1 - 0.6f && sb.x0 >= bb.x1 - 0.6f);   // small screens on the right
        CHECK(Near(st.x0, sb.x0) && st.y1 <= sb.y0 + 0.6f);        // stacked, top above
        int x, y;
        CHECK(l.Touch((int)(sb.x0 + 1), (int)(sb.y0 + 1), x, y) && x == 0 && y == 0);
        CHECK(!l.Touch((int)(bb.x0 + 50), (int)(bb.y0 + 50), x, y));   // big screen = top screen: no touch
    }

    // hybrid, bottom screen big: touch works on the big and on the small bottom screen
    o.Swap = true;
    l.Setup(o, false);
    Dump("hybride (bas en grand)", l);
    {
        int bigIdx = -1;
        for (int i = 0; i < l.NumQuads; i++)
            if (Bounds(l.Quads[i]).x1 - Bounds(l.Quads[i]).x0 > 600) bigIdx = i;
        CHECK(bigIdx >= 0 && l.Quads[bigIdx].Screen == 1);
        Box bb = Bounds(l.Quads[bigIdx]);
        int x, y;
        CHECK(l.Touch((int)(bb.x0 + (bb.x1 - bb.x0) / 2), (int)(bb.y0 + (bb.y1 - bb.y0) / 2), x, y) && abs(x - 128) <= 1 && abs(y - 96) <= 1);
        Box sb {};
        for (int i = 0; i < l.NumQuads; i++)
            if (i != bigIdx && l.Quads[i].Screen == 1) sb = Bounds(l.Quads[i]);
        CHECK(l.Touch((int)sb.x0 + 1, (int)sb.y0 + 1, x, y) && x <= 1 && y <= 1);
        float cx[3], cy[3], cs[3];
        CHECK(l.BottomToScreen(0, 0, cx, cy, cs) == 2);
    }
    o.Swap = false;

    // automatic big screen: top screen alone, as big as possible (3.75x: 960x720), no touch
    o.Layout = 0;
    l.Setup(o, true);
    Dump("haut seul (auto)", l);
    CHECK(l.NumQuads == 1 && l.Quads[0].Screen == 0);
    {
        Box t = Bounds(l.Quads[0]);
        CHECK(Near(t.x0, 160) && Near(t.x1, 1120) && Near(t.y0, 0) && Near(t.y1, 720));
        int x, y;
        CHECK(!l.Touch(640, 360, x, y));
    }
    o.Layout = 2;
    l.Setup(o, true);
    CHECK(l.NumQuads == 1 && l.Quads[0].Screen == 0);
    o.Layout = 0;

    // integer scale: 1x per screen in vertical (720/384 = 1.875 -> 1)
    o.IntegerScale = true;
    l.Setup(o, false);
    Dump("verticale entière", l);
    CHECK(Near(Bounds(*Find(l, 0)).x1 - Bounds(*Find(l, 0)).x0, 256));
    o.Layout = 1;
    l.Setup(o, false);
    Dump("horizontale entière", l);
    CHECK(Near(Bounds(*Find(l, 0)).x1 - Bounds(*Find(l, 0)).x0, 512));
    o.Layout = 0;
    o.IntegerScale = false;

    // book: rotated 90 degrees, both screens inside the display, side by side, touch still maps to 0..255 x 0..191
    for (int rot : {1, 3})
    {
        o.Rotation = rot;
        l.Setup(o, false);
        Dump(rot == 1 ? "livre 90" : "livre 270", l);
        Box t = Bounds(*Find(l, 0)), b = Bounds(*Find(l, 1));
        CHECK(Inside(t) && Inside(b));
        CHECK(Near(t.y1 - t.y0, 256 * (t.x1 - t.x0) / 192));   // portrait screens
        CHECK(t.x1 <= b.x0 + 0.6f || b.x1 <= t.x0 + 0.6f);     // side by side
        int x, y;
        CHECK(l.Touch((int)((b.x0 + b.x1) / 2), (int)((b.y0 + b.y1) / 2), x, y) && abs(x - 128) <= 1 && abs(y - 96) <= 1);
        float cx[3], cy[3], cs[3];
        CHECK(l.BottomToScreen(128, 96, cx, cy, cs) == 1 && Near(cx[0], (b.x0 + b.x1) / 2) && Near(cy[0], (b.y0 + b.y1) / 2));
    }
    o.Rotation = 0;

    // sizes: emphasised screen bigger, every layout and option inside the display
    for (int lay = 0; lay < 3; lay++)
        for (int siz = 0; siz < 5; siz++)
            for (int rot : {0, 1, 3})
                for (int sw = 0; sw < 2; sw++)
                    for (int is = 0; is < 2; is++)
                    {
                        DisplayOptions p;
                        p.Layout = lay; p.Sizing = siz; p.Rotation = rot; p.Swap = sw; p.IntegerScale = is; p.Gap = 8;
                        l.Setup(p, false);
                        CHECK(l.NumQuads >= 1);
                        for (int i = 0; i < l.NumQuads; i++)
                        {
                            Box b = Bounds(l.Quads[i]);
                            if (!Inside(b) || b.x1 - b.x0 < 100)
                            {
                                printf("  layout %d sizing %d rot %d swap %d int %d: quad %d %.0f,%.0f-%.0f,%.0f\n", lay, siz, rot, sw, is, i, b.x0, b.y0, b.x1, b.y1);
                                Failures++;
                            }
                        }
                    }
    o.Sizing = 1;
    l.Setup(o, false);
    Dump("haut plus grand", l);
    CHECK(Bounds(*Find(l, 0)).x1 - Bounds(*Find(l, 0)).x0 > Bounds(*Find(l, 1)).x1 - Bounds(*Find(l, 1)).x0);

    // settings round trip
    {
        DisplayOptions a;
        a.Layout = 2; a.Sizing = 3; a.Swap = true; a.Rotation = 3; a.IntegerScale = true; a.Gap = 16; a.AutoTop = false;
        std::map<std::string, std::string> kv;
        std::string s = a.Write();
        size_t p = 0;
        while (p < s.size())
        {
            size_t nl = s.find('\n', p), eq = s.find('=', p);
            kv[s.substr(p, eq - p)] = s.substr(eq + 1, nl - eq - 1);
            p = nl + 1;
        }
        DisplayOptions r;
        r.Read(kv);
        CHECK(r.Layout == 2 && r.Sizing == 3 && r.Swap && r.Rotation == 3 && r.IntegerScale && r.Gap == 16 && !r.AutoTop);
        DisplayOptions d;
        d.Read({{"layout", "1"}});   // 0.2 ui.txt: only layout=
        CHECK(d.Layout == 1 && d.AutoTop);
    }

    // automatic big screen: 30 black frames before hiding, back at once, no flicker on short fades
    {
        BlackScreenHysteresis h;
        for (int i = 0; i < 29; i++) CHECK(!h.Update(true));
        CHECK(h.Update(true) && h.Hidden);
        CHECK(!h.Update(true) && h.Hidden);
        CHECK(h.Update(false) && !h.Hidden);
        for (int k = 0; k < 10; k++) { for (int i = 0; i < 20; i++) h.Update(true); h.Update(false); }
        CHECK(!h.Hidden);
    }
    {
        std::vector<unsigned> px(256 * 192, 0xFF000000u);
        CHECK(SoftScreenIsBlack(px.data()));
        px[256 * 191 + 255] = 0xFF040404u;
        CHECK(SoftScreenIsBlack(px.data()));
        px[256 * 191 + 255] = 0xFF000800u;
        CHECK(!SoftScreenIsBlack(px.data()));
    }

    printf(Failures ? "%d FAILURES\n" : "all tests passed\n", Failures);
    return Failures ? 1 : 0;
}
