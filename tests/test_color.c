// Host unit tests for color math used in music_ui.c.
// Validates RGB565 packing, ui_dim (dimming toward black), ui_mix (blend),
// and the darken_all formula (two successive halvings → 1/4 brightness).
// Build+run: see tests/run.sh
#include <stdio.h>
#include <stdint.h>

static int fails = 0, checks = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)
#define CHECK_INT(a, b, msg) do { checks++; if ((a) != (b)) { printf("FAIL: %s: got %d want %d\n", msg, (int)(a), (int)(b)); fails++; } } while (0)

#define RGB565(r, g, b) (uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))

static uint16_t ui_dim(uint16_t c, int num, int den)
{
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    r = r * num / den; g = g * num / den; b = b * num / den;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint16_t ui_mix(uint16_t a, uint16_t b, int t)
{
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = ar + (br - ar) * t / 16;
    int g = ag + (bg - ag) * t / 16;
    int bl = ab + (bb - ab) * t / 16;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static int r5(uint16_t c) { return (c >> 11) & 0x1F; }
static int g6(uint16_t c) { return (c >> 5) & 0x3F; }
static int b5(uint16_t c) { return c & 0x1F; }

// --- RGB565 packing ---

static void test_rgb565_white(void)
{
    uint16_t c = RGB565(255, 255, 255);
    CHECK_INT(r5(c), 31, "white R=31");
    CHECK_INT(g6(c), 63, "white G=63");
    CHECK_INT(b5(c), 31, "white B=31");
}

static void test_rgb565_black(void)
{
    uint16_t c = RGB565(0, 0, 0);
    CHECK(c == 0, "black == 0");
}

static void test_rgb565_red(void)
{
    uint16_t c = RGB565(255, 0, 0);
    CHECK_INT(r5(c), 31, "red R=31");
    CHECK_INT(g6(c), 0, "red G=0");
    CHECK_INT(b5(c), 0, "red B=0");
}

static void test_rgb565_green(void)
{
    uint16_t c = RGB565(0, 255, 0);
    CHECK_INT(r5(c), 0, "green R=0");
    CHECK_INT(g6(c), 63, "green G=63");
    CHECK_INT(b5(c), 0, "green B=0");
}

static void test_rgb565_blue(void)
{
    uint16_t c = RGB565(0, 0, 255);
    CHECK_INT(r5(c), 0, "blue R=0");
    CHECK_INT(g6(c), 0, "blue G=0");
    CHECK_INT(b5(c), 31, "blue B=31");
}

static void test_rgb565_roundtrip(void)
{
    // 248, 252, 248 are the max representable (31<<3, 63<<2, 31<<3)
    uint16_t c = RGB565(248, 252, 248);
    CHECK_INT(r5(c), 31, "roundtrip R");
    CHECK_INT(g6(c), 63, "roundtrip G");
    CHECK_INT(b5(c), 31, "roundtrip B");
}

static void test_rgb565_low_values(void)
{
    uint16_t c = RGB565(8, 8, 8);
    CHECK_INT(r5(c), 1, "low R=1");
    CHECK_INT(g6(c), 2, "low G=2");
    CHECK_INT(b5(c), 1, "low B=1");
}

// --- ui_dim ---

static void test_dim_half(void)
{
    uint16_t c = RGB565(200, 200, 200);
    uint16_t d = ui_dim(c, 1, 2);
    CHECK(r5(d) == r5(c) / 2, "half dim R");
    CHECK(g6(d) == g6(c) / 2, "half dim G");
    CHECK(b5(d) == b5(c) / 2, "half dim B");
}

static void test_dim_zero(void)
{
    uint16_t c = RGB565(255, 255, 255);
    uint16_t d = ui_dim(c, 0, 1);
    CHECK(d == 0, "dim to 0 = black");
}

static void test_dim_full(void)
{
    uint16_t c = RGB565(128, 128, 128);
    uint16_t d = ui_dim(c, 1, 1);
    CHECK(d == c, "dim *1 = unchanged");
}

static void test_dim_black_stays_black(void)
{
    uint16_t d = ui_dim(0, 3, 4);
    CHECK(d == 0, "dim black = black");
}

static void test_dim_quarter(void)
{
    uint16_t c = RGB565(248, 252, 248);
    uint16_t d = ui_dim(c, 1, 4);
    CHECK_INT(r5(d), 31 / 4, "quarter R");
    CHECK_INT(g6(d), 63 / 4, "quarter G");
    CHECK_INT(b5(d), 31 / 4, "quarter B");
}

// --- darken_all simulation (two successive half-dims = 1/4 brightness) ---

static uint16_t darken_step(uint16_t c)
{
    return (uint16_t)((c >> 1) & 0x7BEF);
}

static void test_darken_halves(void)
{
    uint16_t c = RGB565(255, 255, 255);
    uint16_t d1 = darken_step(c);
    uint16_t d2 = darken_step(d1);
    CHECK_INT(r5(d1), 15, "first darken R halved (31->15)");
    CHECK_INT(g6(d1), 31, "first darken G halved (63->31)");
    CHECK_INT(b5(d1), 15, "first darken B halved (31->15)");
    CHECK_INT(r5(d2), 7, "second darken R quartered (31->7)");
    CHECK_INT(g6(d2), 15, "second darken G quartered (63->15)");
    CHECK_INT(b5(d2), 7, "second darken B quartered (31->7)");
}

static void test_darken_preserves_zero(void)
{
    uint16_t d1 = darken_step(0);
    uint16_t d2 = darken_step(d1);
    CHECK(d2 == 0, "darken black stays black");
}

// --- ui_mix ---

static void test_mix_zero_t(void)
{
    uint16_t a = RGB565(255, 0, 0);
    uint16_t b = RGB565(0, 255, 0);
    uint16_t m = ui_mix(a, b, 0);
    CHECK(m == a, "mix t=0 returns a");
}

static void test_mix_full_t(void)
{
    uint16_t a = RGB565(255, 0, 0);
    uint16_t b = RGB565(0, 255, 0);
    uint16_t m = ui_mix(a, b, 16);
    CHECK(m == b, "mix t=16 returns b");
}

static void test_mix_midpoint(void)
{
    uint16_t a = RGB565(0, 0, 0);
    uint16_t b = RGB565(248, 252, 248);
    uint16_t m = ui_mix(a, b, 8);
    CHECK_INT(r5(m), 15, "midpoint R ~ 31/2");
    CHECK_INT(g6(m), 31, "midpoint G ~ 63/2");
    CHECK_INT(b5(m), 15, "midpoint B ~ 31/2");
}

static void test_mix_same(void)
{
    uint16_t c = RGB565(100, 150, 100);
    uint16_t m = ui_mix(c, c, 7);
    CHECK(m == c, "mix same color = same");
}

static void test_mix_near_symmetry(void)
{
    uint16_t a = RGB565(200, 100, 50);
    uint16_t b = RGB565(50, 200, 100);
    uint16_t m1 = ui_mix(a, b, 4);
    uint16_t m2 = ui_mix(b, a, 12);
    int dr = r5(m1) - r5(m2), dg = g6(m1) - g6(m2), db = b5(m1) - b5(m2);
    CHECK(dr >= -1 && dr <= 1, "mix symmetry R within ±1");
    CHECK(dg >= -1 && dg <= 1, "mix symmetry G within ±1");
    CHECK(db >= -1 && db <= 1, "mix symmetry B within ±1");
}

static void test_mix_small_t(void)
{
    uint16_t bg = RGB565(16, 18, 28);
    uint16_t accent = RGB565(90, 200, 255);
    uint16_t m = ui_mix(bg, accent, 2);
    CHECK(r5(m) > r5(bg), "slight tint R increases");
    CHECK(r5(m) < r5(accent), "slight tint R < accent");
    CHECK(g6(m) > g6(bg), "slight tint G increases");
    CHECK(b5(m) > b5(bg), "slight tint B increases");
}

int main(void)
{
    test_rgb565_white();
    test_rgb565_black();
    test_rgb565_red();
    test_rgb565_green();
    test_rgb565_blue();
    test_rgb565_roundtrip();
    test_rgb565_low_values();
    test_dim_half();
    test_dim_zero();
    test_dim_full();
    test_dim_black_stays_black();
    test_dim_quarter();
    test_darken_halves();
    test_darken_preserves_zero();
    test_mix_zero_t();
    test_mix_full_t();
    test_mix_midpoint();
    test_mix_same();
    test_mix_near_symmetry();
    test_mix_small_t();
    printf("color: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
