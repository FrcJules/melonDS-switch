// Host test of the launch arguments: g++ -std=c++17 -o /tmp/launchargs_test tests/launchargs_test.cpp && /tmp/launchargs_test
#include <stdio.h>
#include "../LaunchArgs.h"

static int fails = 0;
static void check(bool ok, const char* what) { if (!ok) { printf("FAIL %s\n", what); fails++; } }

int main()
{
    {
        char* v[] = {(char*)"/switch/melonDS-Switch/melonDS-Switch.nro"};
        LaunchArgs a = ParseLaunchArgs(1, v);
        check(a.Rom.empty() && !a.Browse, "no argument");
    }
    {
        char* v[] = {(char*)"x.nro", (char*)"/roms/DS/Mario Kart DS.nds"};
        LaunchArgs a = ParseLaunchArgs(2, v);
        check(a.Rom == "sdmc:/roms/DS/Mario Kart DS.nds" && !a.Browse, "SD path gets sdmc:");
    }
    {
        char* v[] = {(char*)"x.nro", (char*)"sdmc:/roms/DS/a.nds"};
        check(ParseLaunchArgs(2, v).Rom == "sdmc:/roms/DS/a.nds", "sdmc: path kept");
    }
    {
        char* v[] = {(char*)"x.nro", (char*)"--browse"};
        LaunchArgs a = ParseLaunchArgs(2, v);
        check(a.Browse && a.Rom.empty(), "--browse alone");
    }
    {
        char* v[] = {(char*)"x.nro", (char*)"--browse", (char*)"/roms/a.nds", (char*)"/roms/b.nds"};
        LaunchArgs a = ParseLaunchArgs(4, v);
        check(a.Browse && a.Rom == "sdmc:/roms/a.nds", "browse + first ROM wins");
    }
    {
        char* v[] = {(char*)"x.nro", (char*)"--unknown", (char*)""};
        check(ParseLaunchArgs(3, v).Rom.empty(), "unknown option and empty argument ignored");
    }
    check(NormalizeSdPath(nullptr).empty(), "null path");
    printf(fails ? "%d failure(s)\n" : "launchargs_test OK\n", fails);
    return fails != 0;
}
