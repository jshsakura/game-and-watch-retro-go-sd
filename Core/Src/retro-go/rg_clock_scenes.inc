/* Procedural animated pixel-scene backgrounds for the clock (ZERO RAM — drawn
 * straight to the framebuffer each repaint, theme-coloured via mix565). #included
 * into rg_clock.c after mix565/clock_theme_t are defined. Every rect goes through
 * srect(), which clamps to the screen (the device blitter does NOT clip). */

/* clamped fill: rect bounded to 0..319 x 0..239 (past the right edge would wrap). */
static void srect(int x, int y, int w, int h, uint16_t col)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= GW_LCD_WIDTH || y >= GW_LCD_HEIGHT) return;
    if (x + w > GW_LCD_WIDTH)  w = GW_LCD_WIDTH  - x;
    if (y + h > GW_LCD_HEIGHT) h = GW_LCD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    odroid_overlay_draw_fill_rect(x, y, w, h, col);
}
/* triangle wave, period 256, range -amp..amp (cheap stand-in for sine).
 * Kept out-of-line: it's the shared per-pixel primitive for every wavy scene,
 * so one copy in flash is far smaller than inlining it into each — and at the
 * clock's ~1.5 fps repaint the call overhead is immeasurable. */
static __attribute__((noinline)) int itri(int a, int amp)
{
    unsigned p = (unsigned)a & 255u;
    int tri = (p < 128) ? (int)p : 256 - (int)p;   /* 0..128..0 */
    return (tri - 64) * amp / 64;                  /* -amp..amp */
}
/* deterministic pseudo-random hash (no rand/state) */
static uint32_t shash(uint32_t x) { x *= 2654435761u; return x ^ (x >> 15); }

/* 1) mountains — layered ridges + moon + stars */
static void scene_mountains(uint32_t now, const clock_theme_t *t)
{
    uint16_t star = mix565(t->scr, t->ink, 8);
    uint16_t moon = mix565(t->scr, t->ink, 11);
    int mx = 250, my = 40;
    srect(mx - 6, my - 2, 12, 4, moon); srect(mx - 4, my - 4, 8, 2, moon);
    srect(mx - 4, my + 2, 8, 2, moon);  srect(mx - 2, my - 6, 4, 2, moon);
    srect(mx - 2, my + 4, 4, 2, moon);
    for (int k = 0; k < 24; k++) {
        int sx = (k * 73 + 11) % GW_LCD_WIDTH, sy = 8 + (k * 47) % 60;
        if (((now / 700) + k) % 5 < 3) srect(sx, sy, 1, 1, star);
    }
    for (int layer = 0; layer < 3; layer++) {
        uint16_t c = mix565(t->scr, t->ink, 2 + layer * 2);
        int base = 150 + layer * 30, amp = 30 - layer * 6;
        for (int x = 0; x < GW_LCD_WIDTH; x += 4) {
            int h = base + itri(x + layer * 40, amp) + itri(x * 3 + layer * 90, amp / 2);
            if (h < 40) h = 40;
            if (h > GW_LCD_HEIGHT - 1) h = GW_LCD_HEIGHT - 1;
            srect(x, h, 4, GW_LCD_HEIGHT - h, c);
        }
    }
}

/* 2) ocean — phase-offset sine swells, foam caps, moon + reflected glint path.
 * Horizon sits just below the digit block so the busy water never fights the
 * clock; the moon is pushed to the right so the glint column stays off-centre. */
static void scene_ocean(uint32_t now, const clock_theme_t *t)
{
    const int hz = 152, mx = 250, my = 44;
    uint16_t star = mix565(t->scr, t->ink, 7);
    for (int k = 0; k < 14; k++) {
        int sx = (k * 83 + 7) % GW_LCD_WIDTH, sy = 6 + (k * 53) % (hz - 40);
        if (((now / 800) + k) % 4 < 2) srect(sx, sy, 1, 1, star);
    }
    /* round-ish moon (chunky, no sqrt) */
    uint16_t moon = mix565(t->scr, t->ink, 13);
    srect(mx - 4, my - 6, 8, 12, moon); srect(mx - 6, my - 4, 12, 8, moon);
    srect(mx - 5, my - 5, 10, 10, moon);
    /* still-dark sea body, then wavy swell lines on top */
    srect(0, hz, GW_LCD_WIDTH, GW_LCD_HEIGHT - hz, mix565(t->scr, t->ink, 2));
    uint16_t foam = mix565(t->scr, t->ink, 13);
    int span = GW_LCD_HEIGHT - hz;
    for (int i = 0; i < 6; i++) {
        int base = hz + 4 + i * (span - 4) / 6;
        int amp  = 2 + i;                          /* taller waves in the foreground */
        int ph   = (int)(now / 90) * (i + 2);      /* nearer layers scroll faster */
        uint16_t c = mix565(t->scr, t->ink, 4 + i);
        for (int x = 0; x <= GW_LCD_WIDTH; x += 3) {
            int y = base + itri(x / 2 + ph + i * 40, amp)
                         + itri(x / 5 - ph / 2 + i * 23, (amp + 1) / 2);
            srect(x, y, 3, 2, c);
            if ((x + i * 11 + (ph >> 2)) % 19 < 2) srect(x, y - 1, 2, 1, foam);  /* crest fleck */
        }
    }
    /* moonlight glint: shimmering dashes reflected straight down from the moon */
    for (int y = hz; y < GW_LCD_HEIGHT; y += 3) {
        int w = 7 + itri(y * 3 + (int)(now / 150), 5); if (w < 2) w = 2;
        int lvl = 12 - (y - hz) / 14; if (lvl < 5) lvl = 5;
        srect(mx - w / 2, y, w, 2, mix565(t->scr, t->ink, lvl));
    }
}

/* 3) starfield — varied-brightness twinkle + occasional shooting star */
static void scene_starfield(uint32_t now, const clock_theme_t *t)
{
    for (int k = 0; k < 90; k++) {
        uint32_t h = shash(k * 2749u);
        int sx = h % GW_LCD_WIDTH, sy = (h >> 9) % GW_LCD_HEIGHT;
        int br = 4 + (h >> 17) % 9;
        if (((now / 500) + (h >> 3)) % 7 < 4) {
            srect(sx, sy, 1, 1, mix565(t->scr, t->ink, br));
            if (br > 10) {
                uint16_t d = mix565(t->scr, t->ink, br - 4);
                srect(sx - 1, sy, 1, 1, d); srect(sx + 1, sy, 1, 1, d);
                srect(sx, sy - 1, 1, 1, d); srect(sx, sy + 1, 1, 1, d);
            }
        }
    }
    int prog = now % 2500;
    if (prog < 600) {
        uint32_t h = shash((now / 2500) * 911u);
        int sy = 10 + h % 120, sx = (h >> 7) % 200, p = prog / 40;
        for (int i = 0; i < 5; i++)
            srect(sx + (p - i) * 4, sy + (p - i) * 2, 2, 1, mix565(t->scr, t->ink, 12 - i * 2));
    }
}

/* 4) synthwave — classic retrowave: banded sun over a horizon glow, with a
 * perspective grid that scrolls toward the viewer and verticals fanning out
 * from the vanishing point. Horizon low so the grid stays below the digits. */
static void scene_synthwave(uint32_t now, const clock_theme_t *t)
{
    const int hz = 172, cx = 160, span = GW_LCD_HEIGHT - hz;
    uint16_t grid = mix565(t->scr, t->alarm, 7);
    /* horizon glow — wide bands warming toward the horizon line */
    for (int i = 0; i < 7; i++) {
        int lvl = 6 - i; if (lvl < 1) lvl = 1;
        srect(0, hz - 2 - i * 6, GW_LCD_WIDTH, 6, mix565(t->scr, t->alarm, lvl));
    }
    /* retro sun: banded disc sitting on the horizon, scanline gaps widen downward */
    for (int dy = -28; dy <= 2; dy++) {
        int q = 28 * 28 - dy * dy, hw = 0; while ((hw + 1) * (hw + 1) <= q) hw++;
        if (dy > -6) { int g = 2 + dy / 4; if (((dy + 28) % (g + 2)) < g) continue; }
        int lvl = 13 - (dy + 28) / 5; if (lvl < 6) lvl = 6;
        srect(cx - hw, hz - 2 + dy, 2 * hw, 1, mix565(t->scr, t->alarm, lvl));
    }
    /* horizontal grid lines flowing toward the viewer (perspective 1/depth) */
    int ph = (int)((now / 40) % 48);
    for (int i = 1; i <= 16; i++) {
        int d = i * 48 - ph; if (d <= 0) continue;
        int yy = hz + (span * 48) / d;
        if (yy < hz || yy >= GW_LCD_HEIGHT) continue;
        srect(0, yy, GW_LCD_WIDTH, 1, mix565(t->scr, t->alarm, 5 + (yy - hz) * 5 / span));
    }
    /* verticals fanning from the vanishing point out to the screen edges */
    for (int vx = -7; vx <= 7; vx++)
        for (int yy = hz; yy < GW_LCD_HEIGHT; yy += 3) {
            int x = cx + vx * (yy - hz) * 22 / span;
            if (x >= 0 && x < GW_LCD_WIDTH) srect(x, yy, 1, 1, grid);
        }
}

/* 5) rain — diagonal streaks + puddle glints */
static void scene_rain(uint32_t now, const clock_theme_t *t)
{
    uint16_t drop = mix565(t->scr, t->ink, 7);
    for (int k = 0; k < 70; k++) {
        uint32_t h = shash(k * 5417u);
        int speed = 6 + (h >> 5) % 5;
        int y = ((int)((h >> 3) + (now / 16) * speed)) % (GW_LCD_HEIGHT + 40) - 20;
        int x = ((h % GW_LCD_WIDTH) + (y > 0 ? y / 8 : 0)) % GW_LCD_WIDTH;
        srect(x, y, 1, 4, drop);
    }
    uint16_t g = mix565(t->scr, t->ink, 9);
    for (int k = 0; k < 10; k++) {
        int x = (k * 37 + (int)((now / 300) % 20)) % GW_LCD_WIDTH;
        if (((now / 250) + k) % 3 == 0) srect(x, 236, 3, 1, g);
    }
}

/* 6) snow — drifting swaying flakes */
static void scene_snow(uint32_t now, const clock_theme_t *t)
{
    uint16_t fl = mix565(t->scr, t->ink, 10);
    for (int k = 0; k < 60; k++) {
        uint32_t h = shash(k * 3313u);
        int speed = 2 + (h >> 6) % 3;
        int y = ((int)((h >> 3) + (now / 40) * speed)) % (GW_LCD_HEIGHT + 20) - 10;
        int sway = itri((int)(now / 200) + (h & 255), 4);
        int x = ((h % GW_LCD_WIDTH) + sway + GW_LCD_WIDTH) % GW_LCD_WIDTH;
        int sz = 1 + ((h >> 11) & 1);
        srect(x, y, sz, sz, fl);
    }
}

/* 7) aurora — undulating accent bands + stars */
static void scene_aurora(uint32_t now, const clock_theme_t *t)
{
    uint16_t star = mix565(t->scr, t->ink, 7);
    for (int k = 0; k < 20; k++) {
        int sx = (k * 67 + 9) % GW_LCD_WIDTH, sy = 6 + (k * 41) % 90;
        if (((now / 700) + k) % 4 < 2) srect(sx, sy, 1, 1, star);
    }
    /* XMB-style bands, lower area. Slow, LONG-period waves + wide overlapping
     * ribbons so the flow reads soft and round (동글동글), not pointy. */
    for (int b = 0; b < 3; b++) {
        uint16_t c = mix565(t->scr, t->alarm, 11 - b * 3);
        int base = 120 + b * 26;
        for (int x = 0; x < GW_LCD_WIDTH; x += 2) {
            /* two gentle waves summed → rounded crest, then a slow half-wave
             * added on top rounds the corners further */
            int y = base + itri(x / 4 + (int)(now / 190) + b * 60, 26)
                         + itri(x / 7 + (int)(now / 320) + b * 40, 13)
                         + itri(x / 12 + (int)(now / 260), 6);
            int hgt = 16 + itri(x / 3 + (int)(now / 150) + b * 30, 9); if (hgt < 6) hgt = 6;
            srect(x - 2, y, 6, hgt, c);   /* wide, overlapping = soft edges */
        }
    }
}

/* 8) forest — pine silhouettes + fireflies */
static void scene_forest(uint32_t now, const clock_theme_t *t)
{
    uint16_t star = mix565(t->scr, t->ink, 7);
    for (int k = 0; k < 16; k++) {
        int sx = (k * 79 + 5) % GW_LCD_WIDTH, sy = 8 + (k * 37) % 50;
        if (((now / 800) + k) % 4 < 2) srect(sx, sy, 1, 1, star);
    }
    for (int i = 0, x = 0; x < GW_LCD_WIDTH; i++) {
        uint32_t h = shash(i * 6151u);
        int th = 40 + (h % 40), tw = 18 + (h >> 4) % 14, layer = (h >> 8) % 3;
        uint16_t c = mix565(t->scr, t->ink, 2 + layer * 2);
        int baseY = GW_LCD_HEIGHT, topY = baseY - th;
        for (int y = topY; y < baseY - 4; y += 2) {
            int prog = (y - topY) * tw / th;
            srect(x + tw / 2 - prog / 2, y, prog + 1, 2, c);
        }
        srect(x + tw / 2 - 1, baseY - 4, 2, 4, mix565(t->scr, t->ink, 3));
        if (((now / 300) + i) % 9 < 2)
            srect(x + tw / 2 + (int)((h >> 2) % 10) - 5, topY + th / 2, 1, 1, mix565(t->scr, t->alarm, 12));
        x += tw - 2;
    }
}

/* 9) desert — setting sun behind smooth dunes */
static void scene_desert(uint32_t now, const clock_theme_t *t)
{
    uint16_t sun = mix565(t->scr, t->alarm, 11);
    int sx = 160, sy = 172;
    for (int dy = -24; dy <= 24; dy++) {
        int q = 24 * 24 - dy * dy, ww = 0;
        while ((ww + 1) * (ww + 1) <= q) ww++;
        srect(sx - ww, sy + dy, ww * 2, 1, sun);
    }
    for (int layer = 0; layer < 3; layer++) {
        uint16_t c = mix565(t->scr, t->ink, 2 + layer * 2);
        int base = 180 + layer * 20;
        for (int x = 0; x < GW_LCD_WIDTH; x += 4) {
            int y = base + itri(x / 2 + layer * 70, 18);
            if (y < base - 18) y = base - 18;
            srect(x, y, 4, GW_LCD_HEIGHT - y, c);
        }
    }
    (void)now;
}

/* 10) meteor — calm stars + periodic diagonal streaks */
static void scene_meteor(uint32_t now, const clock_theme_t *t)
{
    for (int k = 0; k < 40; k++) {
        uint32_t h = shash(k * 1123u);
        int sx = h % GW_LCD_WIDTH, sy = (h >> 9) % GW_LCD_HEIGHT;
        if (((now / 600) + k) % 5 < 3) srect(sx, sy, 1, 1, mix565(t->scr, t->ink, 5 + (h >> 3) % 6));
    }
    for (int m = 0; m < 2; m++) {
        int prog = (int)((now + m * 900) % 1800);
        if (prog < 500) {
            uint32_t h = shash(((now / 1800) + m * 7) * 733u + m);
            int x0 = h % 260, y0 = 10 + (h >> 7) % 80, p = prog / 25;
            for (int i = 0; i < 6; i++)
                srect(x0 + (p - i) * 5, y0 + (p - i) * 3, 2, 1, mix565(t->scr, t->ink, 12 - i * 2));
        }
    }
}

/* 11) matrix — falling dot columns, bright head */
static void scene_matrix(uint32_t now, const clock_theme_t *t)
{
    int cols = GW_LCD_WIDTH / 8;
    for (int c = 0; c < cols; c++) {
        uint32_t h = shash(c * 4271u);
        int speed = 2 + (h % 4);
        int head = (int)((h >> 3) + (now / 50) * speed) % (GW_LCD_HEIGHT + 80);
        for (int i = 0; i < 10; i++) {
            int y = head - i * 8;
            if (y < 0 || y >= GW_LCD_HEIGHT) continue;
            int f = (i == 0) ? 13 : 11 - i;
            if (f < 1) continue;
            srect(c * 8 + 2, y, 2, 2, mix565(t->scr, t->alarm, f));
        }
    }
}

/* 12) bubbles — hollow squares rising + wobble */
static void scene_bubbles(uint32_t now, const clock_theme_t *t)
{
    uint16_t c = mix565(t->scr, t->ink, 7);
    for (int k = 0; k < 28; k++) {
        uint32_t h = shash(k * 9199u);
        int speed = 2 + (h >> 5) % 4;
        int y = GW_LCD_HEIGHT - (int)((h >> 3) + (now / 40) * speed) % (GW_LCD_HEIGHT + 30);
        int wob = itri((int)(now / 150) + (h & 255), 4);
        int x = ((h % GW_LCD_WIDTH) + wob + GW_LCD_WIDTH) % GW_LCD_WIDTH;
        int sz = 2 + (h >> 9) % 4;
        srect(x, y, sz, 1, c); srect(x, y + sz - 1, sz, 1, c);
        srect(x, y, 1, sz, c); srect(x + sz - 1, y, 1, sz, c);
    }
}

/* 13) fireworks — staggered expanding rings that fade */
static void scene_fireworks(uint32_t now, const clock_theme_t *t)
{
    uint16_t star = mix565(t->scr, t->ink, 6);
    for (int k = 0; k < 14; k++) {
        int sx = (k * 91 + 3) % GW_LCD_WIDTH, sy = 6 + (k * 57) % 110;
        srect(sx, sy, 1, 1, star);
    }
    for (int b = 0; b < 3; b++) {
        int prog = (int)((now + b * 800) % 2200);
        uint32_t h = shash(((now + b * 800) / 2200) * 617u + b);
        int cx = 30 + h % 260, cy = 30 + (h >> 8) % 120;
        if (prog < 900) {
            int rad = prog / 40;
            uint16_t c = mix565(t->scr, t->alarm, 13 - prog / 80);
            for (int a = 0; a < 16; a++) {
                int dx = itri(a * 16, rad), dy = itri(a * 16 + 64, rad);
                srect(cx + dx, cy + dy, 1, 1, c);
            }
        }
    }
}

/* 14) grid pulse — dot grid, brightness ripple from centre */
static void scene_grid_pulse(uint32_t now, const clock_theme_t *t)
{
    int cx = 160, cy = 120, pulse = (int)((now / 40) % 120);
    for (int y = 8; y < GW_LCD_HEIGHT; y += 12)
        for (int x = 8; x < GW_LCD_WIDTH; x += 12) {
            int dx = x - cx, dy = y - cy;
            int a = dx < 0 ? -dx : dx, bb = dy < 0 ? -dy : dy;
            int d = a > bb ? a + bb / 2 : bb + a / 2;    /* octagonal dist approx */
            int diff = d - pulse; if (diff < 0) diff = -diff;
            int br = diff < 10 ? 12 - diff : 3;
            srect(x, y, 2, 2, mix565(t->scr, t->alarm, br));
        }
}

/* 15) clouds — soft blobs drifting sideways */
static void scene_clouds(uint32_t now, const clock_theme_t *t)
{
    uint16_t star = mix565(t->scr, t->ink, 6);
    for (int k = 0; k < 12; k++) {
        int sx = (k * 97 + 7) % GW_LCD_WIDTH, sy = 6 + (k * 43) % 70;
        srect(sx, sy, 1, 1, star);
    }
    /* Fewer clouds, spread across the sky in generous bands so it reads calm and
     * airy rather than crowded (device feedback). Each band holds one cloud. */
    for (int i = 0; i < 4; i++) {
        uint32_t h = shash(i * 7717u);
        int speed = 1 + (h % 2);
        int cx = (int)((h % GW_LCD_WIDTH) + (now / 320) * speed) % (GW_LCD_WIDTH + 90) - 45;
        int cy = 16 + i * 30 + (int)((h >> 7) % 12);   /* one per band: ~16/46/76/106 */
        uint16_t c = mix565(t->scr, t->ink, 3 + (h >> 3) % 3);
        srect(cx, cy, 34, 9, c);          srect(cx + 9, cy - 6, 22, 9, c);
        srect(cx + 20, cy + 2, 24, 8, c); srect(cx - 5, cy + 5, 18, 7, c);
    }
}

/* 16) campfire — flickering flame + rising sparks + stars */
static void scene_campfire(uint32_t now, const clock_theme_t *t)
{
    uint16_t star = mix565(t->scr, t->ink, 7);
    for (int k = 0; k < 18; k++) {
        int sx = (k * 71 + 5) % GW_LCD_WIDTH, sy = 8 + (k * 47) % 90;
        if (((now / 700) + k) % 4 < 2) srect(sx, sy, 1, 1, star);
    }
    int fx = 160, fy = 228;
    /* wide log base */
    uint16_t lg = mix565(t->scr, t->ink, 4), lg2 = mix565(t->scr, t->ink, 3);
    srect(fx - 32, fy, 64, 8, lg); srect(fx - 24, fy + 6, 48, 5, lg2);
    int fl = (int)(now / 70);
    /* outer flame — tall, wide, tapering stack */
    for (int i = 0; i < 20; i++) {
        int y = fy - 2 - i * 4, jit = itri(fl + i * 18, 5 + i / 3);
        int w = 48 - i * 2; if (w < 3) w = 3;
        int f = 11 - i / 2; if (f < 2) f = 2;
        srect(fx - w / 2 + jit, y, w, 4, mix565(t->scr, t->alarm, f));
    }
    /* inner hot core — brighter, narrower, blends toward the light ink */
    for (int i = 0; i < 15; i++) {
        int y = fy - 4 - i * 4, jit = itri(fl + i * 26 + 40, 4 + i / 4);
        int w = 26 - i * 2; if (w < 2) w = 2;
        int f = 6 + i / 3; if (f > 12) f = 12;
        srect(fx - w / 2 + jit, y, w, 4, mix565(t->alarm, t->ink, f));
    }
    /* bigger rising sparks */
    for (int k = 0; k < 14; k++) {
        uint32_t h = shash(k * 3517u);
        int life = (int)(now / 28 + (h >> 2)) % 95;
        int x = fx + (int)(h % 44) - 22 + itri(life * 3, 8), y = fy - 16 - life * 2;
        int sz = 1 + (int)((h >> 9) & 1);
        if (y > 0) srect(x, y, sz, sz, mix565(t->scr, t->alarm, 11 - life / 12));
    }
}

/* 17) equalizer — spectrum bars bouncing along the bottom, bright peak caps */
static void scene_equalizer(uint32_t now, const clock_theme_t *t)
{
    int bars = 20, bw = GW_LCD_WIDTH / bars;      /* 16px per bar */
    int floorY = GW_LCD_HEIGHT;
    for (int b = 0; b < bars; b++) {
        uint32_t h = shash(b * 2237u);
        int ph = (int)(now / 30) + (int)(h % 128);
        int amp = 50 + (int)((h >> 5) % 90);
        int hgt = 20 + amp / 2 + itri(ph, amp / 2) + itri(ph * 3 + 40, amp / 6);
        if (hgt < 4) hgt = 4;
        if (hgt > floorY - 6) hgt = floorY - 6;
        int x = b * bw + 2, w = bw - 3;
        for (int y = floorY - hgt; y < floorY; y += 4) {  /* brighter toward top */
            int frac = (floorY - y) * 12 / hgt;
            int br = 4 + frac; if (br > 13) br = 13;
            srect(x, y, w, 4, mix565(t->scr, t->alarm, br));
        }
        int peak = itri(ph - 30, amp / 2);            /* lagging peak cap */
        int py = floorY - hgt - 4 - (peak > 0 ? peak / 6 : 0);
        srect(x, py, w, 2, mix565(t->scr, t->ink, 12));
    }
}

/* 18) plasma — demoscene interference field in chunky blocks (theme-tinted) */
static void scene_plasma(uint32_t now, const clock_theme_t *t)
{
    const int bs = 10;                            /* 32×24 blocks */
    int tt = (int)(now / 40);
    for (int y = 0; y < GW_LCD_HEIGHT; y += bs)
        for (int x = 0; x < GW_LCD_WIDTH; x += bs) {
            int v = itri(x + tt, 16) + itri(y - tt, 16)
                  + itri((x + y) / 2 + tt, 16) + itri((x - y) / 2 - tt * 2, 12);
            int br = (v + 60) * 16 / 120;          /* v≈-60..60 → 0..16 */
            if (br < 0) br = 0; if (br > 16) br = 16;
            uint16_t col = (br < 8) ? mix565(t->scr, t->ink, 8 - br)
                                    : mix565(t->scr, t->alarm, br - 8);
            srect(x, y, bs, bs, col);
        }
}

/* 19) helix — twin sine strands weaving with connecting rungs + stars */
static void scene_helix(uint32_t now, const clock_theme_t *t)
{
    uint16_t star = mix565(t->scr, t->ink, 6);
    for (int k = 0; k < 14; k++) {
        int sx = (k * 89 + 5) % GW_LCD_WIDTH, sy = (k * 53 + 9) % GW_LCD_HEIGHT;
        if (((now / 700) + k) % 4 < 2) srect(sx, sy, 1, 1, star);
    }
    int cx = GW_LCD_WIDTH / 2, amp = 70, tt = (int)(now / 40);
    uint16_t rung = mix565(t->scr, t->ink, 3);            /* faint connecting rung */
    for (int y = 0; y < GW_LCD_HEIGHT; y += 5) {
        int a = itri(y * 2 + tt, amp);
        int x1 = cx + a, x2 = cx - a, front = (a >= 0);
        /* sparse rung: only ~every 20px of height, so it never becomes a bar */
        if ((y / 5) % 4 == 0) {
            int lx = x1 < x2 ? x1 : x2, rw = (x1 < x2 ? x2 - x1 : x1 - x2);
            srect(lx, y + 2, rw, 1, rung);
        }
        uint16_t c1 = mix565(t->scr, t->alarm, front ? 13 : 6);
        uint16_t c2 = mix565(t->scr, t->ink,   front ? 6 : 13);
        srect(x1 - 3, y, 6, 5, c1);                        /* thicker strand dots */
        srect(x2 - 3, y, 6, 5, c2);
    }
}
