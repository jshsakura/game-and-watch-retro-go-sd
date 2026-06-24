// Host unit tests for music_id3 (ID3v2 tag/cover reader, encoding conversion).
// Builds synthetic ID3v2.3 tags in /tmp and verifies parsing to UTF-8.
// Build+run: see tests/run.sh
#include "music_id3.h"
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)
#define CHECK_STR(a, b, msg) do { checks++; if (strcmp((a), (b)) != 0) { \
    printf("FAIL: %s : got '%s' want '%s'\n", msg, (a), (b)); fails++; } } while (0)

// --- frame accumulation buffer ---
static unsigned char F[8192];
static int FN;
static void f_reset(void) { FN = 0; }
static void f_be32(unsigned int v) { F[FN++] = v >> 24; F[FN++] = v >> 16; F[FN++] = v >> 8; F[FN++] = v; }
static void f_frame(const char *id, const unsigned char *data, int n)
{
    memcpy(F + FN, id, 4); FN += 4;
    f_be32((unsigned)n);
    F[FN++] = 0; F[FN++] = 0;           // flags
    memcpy(F + FN, data, n); FN += n;
}
static void f_text(const char *id, int enc, const unsigned char *txt, int txtlen)
{
    unsigned char d[1024];
    d[0] = (unsigned char)enc;
    memcpy(d + 1, txt, txtlen);
    f_frame(id, d, 1 + txtlen);
}

// Write "ID3" v2.3 header + accumulated frames to a file.
static void write_tag(const char *path)
{
    FILE *fp = fopen(path, "wb");
    unsigned char hdr[10] = { 'I', 'D', '3', 3, 0, 0,
        (FN >> 21) & 0x7f, (FN >> 14) & 0x7f, (FN >> 7) & 0x7f, FN & 0x7f };
    fwrite(hdr, 1, 10, fp);
    fwrite(F, 1, FN, fp);
    // a little audio-ish padding so the file isn't pure tag
    unsigned char pad[32] = { 0 };
    fwrite(pad, 1, sizeof(pad), fp);
    fclose(fp);
}

static void test_text_frames(void)
{
    f_reset();
    // UTF-8 title: "노래" = EB85B8 EB9E98
    const unsigned char title_utf8[] = { 0xEB, 0x85, 0xB8, 0xEB, 0x9E, 0x98 };
    f_text("TIT2", 3, title_utf8, sizeof(title_utf8));

    // UTF-16 (BOM LE) artist: "Hi"
    const unsigned char artist_u16[] = { 0xFF, 0xFE, 'H', 0, 'i', 0 };
    f_text("TPE1", 1, artist_u16, sizeof(artist_u16));

    // latin1 album with a high byte: "Caf" + 0xE9 (é) => "Café"
    const unsigned char album_l1[] = { 'C', 'a', 'f', 0xE9 };
    f_text("TALB", 0, album_l1, sizeof(album_l1));

    f_text("TYER", 0, (const unsigned char *)"2026", 4);
    f_text("TRCK", 0, (const unsigned char *)"3/12", 4);

    // USLT lyrics: enc(0) + lang(eng) + desc('\0') + text
    unsigned char uslt[64]; int un = 0;
    uslt[un++] = 0; uslt[un++] = 'e'; uslt[un++] = 'n'; uslt[un++] = 'g'; uslt[un++] = 0;
    memcpy(uslt + un, "la la", 5); un += 5;
    f_frame("USLT", uslt, un);

    write_tag("/tmp/t_id3_text.mp3");

    music_tags_t t;
    id3_read_tags("/tmp/t_id3_text.mp3", &t);
    CHECK(t.found, "tag found");
    CHECK_STR(t.title, "\xEB\x85\xB8\xEB\x9E\x98", "UTF-8 title preserved");
    CHECK_STR(t.artist, "Hi", "UTF-16 LE artist -> ascii");
    CHECK_STR(t.album, "Caf\xC3\xA9", "latin1 0xE9 -> UTF-8 C3 A9");
    CHECK_STR(t.year, "2026", "year");
    CHECK_STR(t.track, "3/12", "track");
    CHECK(t.has_lyrics, "USLT lyrics present");
    CHECK_STR(t.lyrics, "la la", "USLT lyrics text (lang+desc skipped)");
}

static void test_utf16_be(void)
{
    f_reset();
    // UTF-16BE (enc 2, no BOM): "OK"
    const unsigned char be[] = { 0, 'O', 0, 'K' };
    f_text("TIT2", 2, be, sizeof(be));
    write_tag("/tmp/t_id3_be.mp3");

    music_tags_t t;
    id3_read_tags("/tmp/t_id3_be.mp3", &t);
    CHECK_STR(t.title, "OK", "UTF-16BE title");
}

static void test_cover(void)
{
    f_reset();
    f_text("TIT2", 3, (const unsigned char *)"Song", 4);

    // APIC: enc(0) + "image/jpeg"\0 + pictype(3) + desc \0 + JPEG bytes
    unsigned char ap[128]; int an = 0;
    ap[an++] = 0;
    memcpy(ap + an, "image/jpeg", 10); an += 10; ap[an++] = 0;
    ap[an++] = 3;                       // front cover
    ap[an++] = 0;                       // empty description
    const unsigned char jpg[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0, 1 };
    memcpy(ap + an, jpg, sizeof(jpg)); an += sizeof(jpg);
    f_frame("APIC", ap, an);
    write_tag("/tmp/t_id3_cover.mp3");

    // New API locates the APIC bytes (offset+length) without reading them.
    long off = 0, len = 0;
    bool png_b = true;
    int found = id3_locate_cover("/tmp/t_id3_cover.mp3", &off, &len, &png_b);
    CHECK(found == 1, "cover located");
    CHECK(len == 12, "cover byte count == jpeg length");
    CHECK(!png_b, "cover detected as JPEG (not png)");
    unsigned char hd[2] = { 0, 0 };
    FILE *cf = fopen("/tmp/t_id3_cover.mp3", "rb");
    if (cf) { fseek(cf, off, SEEK_SET); if (fread(hd, 1, 2, cf) != 2) hd[0] = 0; fclose(cf); }
    CHECK(hd[0] == 0xFF && hd[1] == 0xD8, "cover starts with JPEG SOI");
}

static void test_no_tag(void)
{
    FILE *fp = fopen("/tmp/t_id3_none.mp3", "wb");
    unsigned char x[16] = { 0xFF, 0xFB, 0x90, 0x00 };   // raw MP3, no ID3
    fwrite(x, 1, sizeof(x), fp);
    fclose(fp);

    music_tags_t t;
    id3_read_tags("/tmp/t_id3_none.mp3", &t);
    CHECK(!t.found, "no ID3 -> not found");
    CHECK_STR(t.title, "", "no ID3 -> empty title");

    long o2 = 0, l2 = 0;
    bool p2 = false;
    CHECK(id3_locate_cover("/tmp/t_id3_none.mp3", &o2, &l2, &p2) == 0, "no ID3 -> no cover");
}

int main(void)
{
    test_text_frames();
    test_utf16_be();
    test_cover();
    test_no_tag();
    printf("music_id3: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
