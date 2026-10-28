// Host test of the banner reader: g++ -std=c++17 -o /tmp/rominfo_test tests/rominfo_test.cpp RomInfo.cpp && /tmp/rominfo_test <roms...>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "../RomInfo.h"

int main(int argc, char** argv)
{
    int fails = 0;
    // synthetic banner: icon pixel (0,0) = colour 1, title "Abc\nSub\nPub" in French, English only "Eng"
    std::vector<uint8_t> b(0x840, 0);
    b[0x20] = 0x01;                       // tile 0, pixel 0 = index 1, pixel 1 = index 0
    b[0x222] = 0x1F; b[0x223] = 0x00;     // colour 1 = red
    const char* fr = "Abc\nSub\nPub";
    for (int i = 0; fr[i]; i++) b[0x240 + 0x200 + i * 2] = fr[i];
    const char* en = "Eng";
    for (int i = 0; en[i]; i++) b[0x240 + 0x100 + i * 2] = en[i];
    b[0x240 + 0x200 + 22] = 0xE9;         // "é" after "Pub"
    RomInfo r;
    ParseBanner(b.data(), b.size(), r, 2);
    if (r.Title != "Abc" || r.Subtitle != "Sub - Pub\xC3\xA9") { printf("FAIL title [%s] [%s]\n", r.Title.c_str(), r.Subtitle.c_str()); fails++; }
    if (r.Icon[0] != 0xFF0000FFu || (r.Icon[1] >> 24) != 0) { printf("FAIL icon %08x %08x\n", r.Icon[0], r.Icon[1]); fails++; }
    ParseBanner(b.data(), b.size(), r, 3);   // German empty -> English
    if (r.Title != "Eng") { printf("FAIL fallback [%s]\n", r.Title.c_str()); fails++; }

    for (int i = 1; i < argc; i++)
    {
        RomInfo info;
        bool ok = ReadRomInfo(argv[i], info);
        int opaque = 0;
        for (int p = 0; p < 1024; p++) opaque += (info.Icon[p] >> 24) != 0;
        printf("%s %s | %s | %s | icon %d (%d opaque px)\n", ok ? "ok " : "ERR", info.GameCode.c_str(), info.Title.c_str(), info.Subtitle.c_str(), info.HasIcon, opaque);
    }
    // recent list
    remove("/tmp/recent_test.txt");
    AddRecent("/tmp/recent_test.txt", "a"); AddRecent("/tmp/recent_test.txt", "b"); AddRecent("/tmp/recent_test.txt", "a");
    auto rl = ReadRecent("/tmp/recent_test.txt");
    if (rl.size() != 2 || rl[0] != "a" || rl[1] != "b") { printf("FAIL recent\n"); fails++; }
    printf(fails ? "%d FAILURES\n" : "all tests passed\n", fails);
    return fails != 0;
}
