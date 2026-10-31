// Host test of the PNG writer: g++ -std=c++17 -o /tmp/png_test tests/png_test.cpp Png.cpp -lpng && /tmp/png_test
#include <png.h>
#include <stdio.h>
#include <vector>
#include "../Png.h"

int main()
{
    int fails = 0;
    const int w = 5, h = 3;
    std::vector<unsigned char> img(w * h * 4);
    for (int i = 0; i < w * h; i++) { img[i * 4] = i * 10; img[i * 4 + 1] = 255 - i; img[i * 4 + 2] = i; img[i * 4 + 3] = 7; }
    const char* path = "/tmp/png_test.png";
    if (!WritePng(path, img, w, h)) { printf("FAIL write\n"); return 1; }
    FILE* f = fopen(path, "rb");
    png_image im = {};
    im.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_stdio(&im, f) || im.width != w || im.height != h) { printf("FAIL header %u x %u\n", im.width, im.height); return 1; }
    im.format = PNG_FORMAT_RGBA;
    std::vector<unsigned char> back(PNG_IMAGE_SIZE(im));
    if (!png_image_finish_read(&im, nullptr, back.data(), 0, nullptr)) { printf("FAIL read\n"); return 1; }
    fclose(f);
    for (int i = 0; i < w * h; i++)
        if (back[i * 4] != img[i * 4] || back[i * 4 + 1] != img[i * 4 + 1] || back[i * 4 + 2] != img[i * 4 + 2] || back[i * 4 + 3] != 255)
        { printf("FAIL pixel %d\n", i); fails++; }
    if (WritePng("/nonexistent-dir/x.png", img, w, h)) { printf("FAIL bad path accepted\n"); fails++; }
    if (WritePng(path, std::vector<unsigned char>(4), w, h)) { printf("FAIL short buffer accepted\n"); fails++; }
    printf(fails ? "%d failure(s)\n" : "png_test OK\n", fails);
    return fails != 0;
}
