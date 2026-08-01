/* Standalone self-test for the RGBA8888 -> RGBA4444 quantiser that
 * src/symtab_glprobe.cpp uses when a DXT3/DXT5 texture is over the software
 * decode cap. Host build, no qemu, no GL, no game data - the property being
 * checked is arithmetic and holds for every possible byte:
 *
 *   c++ -std=c++17 -O2 -o /tmp/rgba4444_selftest analysis/rgba4444_selftest.cpp
 *   /tmp/rgba4444_selftest
 *
 * THE TRAP, which cost a wrong first implementation and which a picture would
 * not have shown:
 *
 * The obvious quantiser is (v + 8) >> 4. It rounds onto a grid of sixteens. But
 * a driver expanding a 4-bit channel back to 8 replicates the bits - nibble n
 * returns as (n << 4) | n - which is a grid of SEVENTEENS. Rounding to the wrong
 * grid is not a rounding error, it is a systematic one that grows towards white:
 * 232 lands on nibble 15 and returns as 255, off by 23 of 255. Every bright end
 * of every gradient shifts, and DXT3's alpha - already four bits, so it should
 * survive untouched - stops being exact.
 *
 * Two properties, then:
 *
 *   1. ROUND-TRIP ERROR is at most 8/255, which is what quantising to 4 bits
 *      costs and nothing more.
 *   2. THE SIXTEEN REPLICATED VALUES ARE FIXED POINTS. This is the one that
 *      matters for this port: DXT3 stores alpha as 4 bits and the decoder
 *      expands it by replication, so every alpha value in the game is one of
 *      these sixteen. If they are fixed points, RGBA4444 is lossless on alpha -
 *      which is the argument for using it on the big world textures at all.
 *
 * Exit status is 0 on success, 1 on any failure.
 */
#include <cstdio>
#include <cstdlib>

/* Must match pack_rgba4444() in src/symtab_glprobe.cpp. */
static unsigned quantise4(unsigned v)
{
    return (v * 15u + 127u) / 255u;
}

/* What GL does on the way back: bit replication, not a shift. */
static unsigned expand4(unsigned n)
{
    return (n << 4) | n;
}

int main(void)
{
    int failures = 0;
    int worst = 0, worst_at = 0;

    for (unsigned v = 0; v <= 255; v++) {
        unsigned n = quantise4(v);
        if (n > 15) {
            printf("FAIL: %u quantises to %u, outside 0..15\n", v, n);
            failures++;
            continue;
        }
        int err = (int)expand4(n) - (int)v;
        if (err < 0)
            err = -err;
        if (err > worst) {
            worst = err;
            worst_at = (int)v;
        }
        if (err > 8) {
            printf("FAIL: %u -> nibble %u -> %u, off by %d (max is 8)\n",
                   v, n, expand4(n), err);
            failures++;
        }
    }

    for (unsigned n = 0; n <= 15; n++) {
        unsigned v = expand4(n);
        unsigned back = quantise4(v);
        if (back != n) {
            printf("FAIL: replicated value %u (nibble %u) quantises to %u - "
                   "DXT3 alpha would not survive\n", v, n, back);
            failures++;
        }
    }

    printf("worst round-trip error %d/255 at %d; "
           "all 16 replicated values are fixed points\n", worst, worst_at);
    printf("%s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
