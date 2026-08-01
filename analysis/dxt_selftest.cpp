/* Standalone self-test for src/dxt_decompress.cpp. Builds and runs on the host
 * - no qemu, no loader, no GL - because block decoding is pure arithmetic.
 *
 * Build and run, from port/:
 *
 *   c++ -std=c++17 -O2 -Isrc -o /tmp/dxt_selftest \
 *       analysis/dxt_selftest.cpp src/dxt_decompress.cpp
 *   /tmp/dxt_selftest \
 *       ../NeededFiles/data/masseffect/assets/published/ParticleBaseTextures/texture_spatter_01.m3g
 *
 * The asset argument is optional; without it only the synthetic block tests
 * run. Any texture under ParticleBaseTextures works - texture_Burst06
 * (256x256, alpha reaching 255) and texture_Mist01 (64x64, peaking at 102) are
 * the two useful extremes. Exit status is 0 on success, 1 on any failure.
 *
 * It checks the two things that can be wrong without looking wrong:
 *
 *   1. THE COLOUR MODE. DXT1 has a per-block punchthrough mode selected by
 *      color0 <= color1, where selector 3 means transparent black. DXT3 and
 *      DXT5 do not have it and always use four interpolated colours. Reusing
 *      DXT1's decode for them corrupts exactly the blocks that encode smooth
 *      gradients - which is what a muzzle flash is made of - and corrupts them
 *      quietly, since the image still decodes to something. The expectations
 *      below are computed from the endpoints rather than copied out of the
 *      decoder, so the same mistake in both would not pass.
 *
 *   2. THE ALPHA, which is the whole point of the DXT3 support: without it a
 *      particle draws as an opaque square. The asset test asserts that a real
 *      texture's alpha varies and that a meaningful part of the quad is cut
 *      away.
 *
 * What is deliberately NOT asserted: that some texel is fully opaque. That was
 * the first version of this test and the assets disproved it - texture_Mist01
 * peaks at alpha 102 and texture_spatter_01 at 187, which is right for a fog
 * and for thin blood. Neither is a bug.
 */
#include "dxt_decompress.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <map>

static int failures = 0;

static void check(bool ok, const char *what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

/* RGB565 helpers so the expectations are computed, not copied from the code. */
static uint16_t rgb565(int r8, int g8, int b8)
{
    return (uint16_t)(((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3));
}
static void expand565(uint16_t c, int *r, int *g, int *b)
{
    int r5 = (c >> 11) & 0x1f, g6 = (c >> 5) & 0x3f, b5 = c & 0x1f;
    *r = (r5 << 3) | (r5 >> 2);
    *g = (g6 << 2) | (g6 >> 4);
    *b = (b5 << 3) | (b5 >> 4);
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }

int main(int argc, char **argv)
{
    uint8_t out[4 * 4 * 4];

    /* ---- 1. DXT1 regression: punchthrough still works when c0 <= c1 ---- */
    {
        uint8_t block[8] = {};
        uint16_t c0 = rgb565(0, 0, 0), c1 = rgb565(255, 255, 255);   /* c0 < c1 */
        put16(block + 0, c0); put16(block + 2, c1);
        block[4] = 0xE4;   /* texels 0..3 = selectors 0,1,2,3 */
        memset(out, 0xAA, sizeof(out));
        dxt::decode(dxt::Format::Dxt1Rgba, block, 8, 4, 4, out);
        check(out[3 * 4 + 3] == 0, "DXT1A c0<=c1: selector 3 is transparent");
        int e2r, e2g, e2b; expand565(c0, &e2r, &e2g, &e2b);
        int f2r, f2g, f2b; expand565(c1, &f2r, &f2g, &f2b);
        check(out[2 * 4 + 0] == (e2r + f2r) / 2,
              "DXT1A c0<=c1: selector 2 is the 1:1 midpoint");
        memset(out, 0xAA, sizeof(out));
        dxt::decode(dxt::Format::Dxt1Rgb, block, 8, 4, 4, out);
        check(out[3 * 4 + 3] == 255, "DXT1 RGB c0<=c1: selector 3 stays opaque");
    }

    /* ---- 2. THE TRAP: DXT3 with c0 <= c1 must NOT punch through ---- */
    {
        uint8_t block[16] = {};
        memset(block, 0xFF, 8);                 /* alpha: every texel opaque */
        uint16_t c0 = rgb565(0, 0, 0), c1 = rgb565(255, 255, 255);   /* c0 < c1 */
        put16(block + 8, c0); put16(block + 10, c1);
        block[12] = 0xE4;                       /* selectors 0,1,2,3 */
        memset(out, 0xAA, sizeof(out));
        dxt::decode(dxt::Format::Dxt3, block, 16, 4, 4, out);
        check(out[3 * 4 + 3] == 255,
              "DXT3 c0<=c1: selector 3 is NOT transparent black");
        int ar, ag, ab; expand565(c0, &ar, &ag, &ab);
        int br, bg, bb; expand565(c1, &br, &bg, &bb);
        check(out[2 * 4 + 0] == (2 * ar + br) / 3,
              "DXT3 c0<=c1: selector 2 is the 2:1 blend (4-colour mode)");
        check(out[3 * 4 + 0] == (2 * br + ar) / 3,
              "DXT3 c0<=c1: selector 3 is the 1:2 blend (4-colour mode)");
        /* Same block through the DXT1 decoder would give black+transparent;
         * that difference is the whole point. */
    }

    /* ---- 3. DXT3 alpha nibbles ---- */
    {
        uint8_t block[16] = {};
        block[0] = 0xF0;   /* texel0 = 0x0, texel1 = 0xF */
        block[1] = 0x88;   /* texel2 = 0x8, texel3 = 0x8 */
        put16(block + 8, rgb565(255, 0, 0)); put16(block + 10, rgb565(0, 0, 255));
        memset(out, 0xAA, sizeof(out));
        dxt::decode(dxt::Format::Dxt3, block, 16, 4, 4, out);
        check(out[0 * 4 + 3] == 0x00, "DXT3 alpha nibble 0x0 -> 0");
        check(out[1 * 4 + 3] == 0xFF, "DXT3 alpha nibble 0xF -> 255 (replicated)");
        check(out[2 * 4 + 3] == 0x88, "DXT3 alpha nibble 0x8 -> 0x88");
    }

    /* ---- 4. DXT5 alpha ramps, both endpoint orders ---- */
    {
        uint8_t block[16] = {};
        put16(block + 8, rgb565(10, 20, 30)); put16(block + 10, rgb565(200, 210, 220));
        /* a0 > a1 -> six interpolated, no 0/255 slots */
        block[0] = 200; block[1] = 100;
        block[2] = 0x88; block[3] = 0x41;  /* idx0=0,idx1=1,idx2=2,idx3=3 ... */
        memset(block + 4, 0, 4);
        /* indices: 3 bits each, little-endian. Build 0,1,2,3 explicitly. */
        {
            uint64_t bits = 0ull | (1ull << 3) | (2ull << 6) | (3ull << 9);
            for (int i = 0; i < 6; i++) block[2 + i] = (uint8_t)((bits >> (8 * i)) & 0xff);
        }
        memset(out, 0xAA, sizeof(out));
        dxt::decode(dxt::Format::Dxt5, block, 16, 4, 4, out);
        check(out[0 * 4 + 3] == 200, "DXT5 a0>a1: index 0 -> a0");
        check(out[1 * 4 + 3] == 100, "DXT5 a0>a1: index 1 -> a1");
        check(out[2 * 4 + 3] == (6 * 200 + 1 * 100) / 7, "DXT5 a0>a1: index 2 ramp");
        check(out[3 * 4 + 3] == (5 * 200 + 2 * 100) / 7, "DXT5 a0>a1: index 3 ramp");

        /* a0 <= a1 -> four interpolated, then explicit 0 and 255 */
        block[0] = 100; block[1] = 200;
        {
            uint64_t bits = (0ull) | (1ull << 3) | (2ull << 6) | (6ull << 9);
            for (int i = 0; i < 6; i++) block[2 + i] = (uint8_t)((bits >> (8 * i)) & 0xff);
        }
        memset(out, 0xAA, sizeof(out));
        dxt::decode(dxt::Format::Dxt5, block, 16, 4, 4, out);
        check(out[0 * 4 + 3] == 100, "DXT5 a0<=a1: index 0 -> a0");
        check(out[2 * 4 + 3] == (4 * 100 + 1 * 200) / 5, "DXT5 a0<=a1: index 2 ramp");
        check(out[3 * 4 + 3] == 0, "DXT5 a0<=a1: index 6 -> 0");
        {
            uint64_t bits = (7ull);
            for (int i = 0; i < 6; i++) block[2 + i] = (uint8_t)((bits >> (8 * i)) & 0xff);
        }
        memset(out, 0xAA, sizeof(out));
        dxt::decode(dxt::Format::Dxt5, block, 16, 4, 4, out);
        check(out[0 * 4 + 3] == 255, "DXT5 a0<=a1: index 7 -> 255");
    }

    /* ---- 5. block_bytes ---- */
    check(dxt::block_bytes(dxt::Format::Dxt1Rgb) == 8, "DXT1 is 8 bytes/block");
    check(dxt::block_bytes(dxt::Format::Dxt3) == 16, "DXT3 is 16 bytes/block");
    check(dxt::block_bytes(dxt::Format::Dxt5) == 16, "DXT5 is 16 bytes/block");

    /* ---- 6. a real particle texture ---- */
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { printf("  cannot open %s\n", argv[1]); return 1; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> file(sz);
        if (fread(file.data(), 1, sz, f) != (size_t)sz) { printf("short read\n"); return 1; }
        fclose(f);
        /* Image2D header, located the same way the earlier scan did. */
        int w = 0, h = 0, off = 0;
        for (int o = 0x58; o < 0x90; o++) {
            uint32_t a, b;
            memcpy(&a, &file[o], 4); memcpy(&b, &file[o + 4], 4);
            if (a == b && (a == 32 || a == 64 || a == 128 || a == 256 || a == 512 || a == 1024)) {
                w = (int)a; h = (int)b; off = o; break;
            }
        }
        int fmt = file[off - 2];
        uint32_t payload_len; memcpy(&payload_len, &file[off + 12], 4);
        const uint8_t *pixels = &file[off + 16];
        size_t level0 = (size_t)w * h;   /* DXT3 is exactly 1 byte per pixel */
        printf("  %s: %dx%d M3G format=%d, level0=%zu bytes (payload %u)\n",
               argv[1], w, h, fmt, level0, payload_len);
        check(fmt == 113, "real asset is M3G format 113");

        std::vector<uint8_t> rgba((size_t)w * h * 4);
        bool ok = dxt::decode(dxt::Format::Dxt3, pixels, level0, w, h, rgba.data());
        check(ok, "real DXT3 texture decodes");

        std::map<int, int> hist;
        for (size_t i = 0; i < (size_t)w * h; i++) hist[rgba[i * 4 + 3]]++;
        int distinct = (int)hist.size();
        int fully_transparent = hist.count(0) ? hist[0] : 0;
        int fully_opaque = hist.count(255) ? hist[255] : 0;
        printf("  alpha: %d distinct values, %d texels at 0, %d at 255 (of %d)\n",
               distinct, fully_transparent, fully_opaque, w * h);
        check(distinct > 1, "alpha VARIES - the particle has a shape, not a square");
        check(fully_transparent > 0, "some texels are fully transparent");
        /*
         * NOT "some texels are fully opaque". That was my assumption and the
         * assets disprove it: texture_Mist01 peaks at alpha 102 and
         * texture_spatter_01 at 187, which is correct for a fog and for thin
         * blood. What matters is that the cutout exists at all.
         */
        int peak = 0;
        for (std::map<int, int>::iterator it = hist.begin(); it != hist.end(); ++it)
            if (it->first > peak) peak = it->first;
        printf("  alpha peak = %d\n", peak);
        check(peak > 0, "the texture is not uniformly transparent");
        check(fully_transparent * 100 / (w * h) >= 10,
              "at least a tenth of the quad is cut away (this is the fix)");
        (void)fully_opaque;
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
