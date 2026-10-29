// Host test of the control profiles and of the right stick -> touch modes:
//   g++ -std=c++17 -o /tmp/controls_test tests/controls_test.cpp Controls.cpp && /tmp/controls_test
#include <stdio.h>
#include <map>
#include <string>
#include "../Controls.h"

static int Fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); Fails++; } } while (0)

int main()
{
    auto b = BuiltinProfiles();
    CHECK(b.size() == 3 && b[0].Id == "standard" && b[1].Id == "aim" && b[2].Id == "stylus");
    // standard = the 0.2 mapping (melonDS key bits)
    const ControlProfile& s = b[0];
    CHECK(s.MaskFor(SwA) == 1u << 0 && s.MaskFor(SwB) == 1u << 1 && s.MaskFor(SwX) == 1u << 10 && s.MaskFor(SwY) == 1u << 11);
    CHECK(s.MaskFor(SwL) == 1u << 9 && s.MaskFor(SwR) == 1u << 8 && s.MaskFor(SwPlus) == 1u << 3 && s.MaskFor(SwMinus) == 1u << 2);
    CHECK(s.MaskFor(SwLeft) == 1u << 5 && s.MaskFor(SwUp) == 1u << 6 && s.MaskFor(SwRight) == 1u << 4 && s.MaskFor(SwDown) == 1u << 7);
    CHECK(s.MaskFor(SwZL) == 0 && s.MaskFor(SwZR) == 0 && s.MaskFor(SwRStick) == 0);
    // aim: R shoots (DS L)
    CHECK(b[1].MaskFor(SwR) == 1u << 9 && b[1].MaskFor(SwL) == 1u << 8 && b[1].RightStick == RightAim);
    // round trip
    ControlProfile p = b[1];
    p.Button[DsA] = SwZR; p.Sensitivity = 8; p.InvertY = true; p.LeftStickDpad = false; p.Name = "Mon profil";
    std::map<std::string, std::string> kv;
    std::string w = p.Write();
    for (size_t i = 0; i < w.size();) { size_t nl = w.find('\n', i), eq = w.find('=', i); kv[w.substr(i, eq - i)] = w.substr(eq + 1, nl - eq - 1); i = nl + 1; }
    ControlProfile r; r.Read(kv);
    CHECK(r.Name == "Mon profil" && r.Button[DsA] == SwZR && r.Sensitivity == 8 && r.InvertY && !r.LeftStickDpad && r.RightStick == RightAim && r.MaskFor(SwR) == 1u << 9);
    // auto profiles from the banner title
    CHECK(AutoProfileFor("Metroid Prime Hunters") == "aim" && AutoProfileFor("STAR FOX COMMAND") == "stylus" && AutoProfileFor("MARIO KART DS") == "");

    // stylus: centre = no touch, full right = right edge, full up = top edge
    StickTouch t;
    CHECK(!t.Update(RightStylus, 0.05f, 0, false, 5, false).Touch);
    auto o = t.Update(RightStylus, 1, 0, false, 5, false);
    CHECK(o.Touch && o.X == 255 && o.Y == 96);
    o = t.Update(RightStylus, 0, 0.92f, false, 5, false);
    CHECK(o.Touch && o.X == 128 && o.Y == 0);
    o = t.Update(RightStylus, -0.5f, -0.5f, false, 5, false);
    CHECK(o.Touch && o.X < 128 && o.Y > 96);

    // aim: first frame at the centre, then dragged right; lifted near the edge, back to the centre 2 frames later
    StickTouch a;
    o = a.Update(RightAim, 1, 0, false, 5, false);
    CHECK(o.Touch && o.X == 128 && o.Y == 96);
    o = a.Update(RightAim, 1, 0, false, 5, false);
    CHECK(o.Touch && o.X == 134);
    int frames = 0, lifts = 0, lastX = o.X;
    bool sawRecentre = false;
    for (int i = 0; i < 100; i++)
    {
        o = a.Update(RightAim, 1, 0, false, 5, false);
        if (!o.Touch) lifts++;
        else { if (o.X < lastX) { CHECK(o.X == 128); sawRecentre = true; } lastX = o.X; }
        frames++;
    }
    CHECK(sawRecentre && lifts >= 2);
    // stick up = stylus up (y decreases), inverted = down
    StickTouch u; u.Update(RightAim, 0, 1, false, 5, false); o = u.Update(RightAim, 0, 1, false, 5, false); CHECK(o.Y < 96);
    StickTouch v; v.Update(RightAim, 0, 1, false, 5, true); o = v.Update(RightAim, 0, 1, false, 5, true); CHECK(o.Y > 96);
    // released: stylus up
    CHECK(!a.Update(RightAim, 0, 0, false, 5, false).Touch);
    // sensitivity doubles the speed
    StickTouch f; f.Update(RightAim, 1, 0, false, 10, false); o = f.Update(RightAim, 1, 0, false, 10, false); CHECK(o.X == 140);

    // cursor: moves, touches only with R3
    StickTouch c;
    o = c.Update(RightCursor, 1, 0, false, 5, false);
    CHECK(!o.Touch && o.X > 128 && o.ShowMarker);
    CHECK(c.Update(RightCursor, 0, 0, true, 5, false).Touch);

    printf(Fails ? "%d FAILURES\n" : "all tests passed\n", Fails);
    return Fails != 0;
}
