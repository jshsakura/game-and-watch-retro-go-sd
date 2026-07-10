/* Host tests for external/firmware_update/Core/Src/tar.c — the extractor that
 * unpacks retro-go_update.bin onto the SD card at boot.
 *
 * The REAL tar.c runs against the REAL FatFs from the firmware_update submodule,
 * on a RAM disk (tests/fw_tar_stubs/ram_diskio.c). Only FF_USE_MKFS is flipped
 * on, in a throw-away copy of the FatFs sources, so a volume can be formatted;
 * tests/run.sh diffs that copy's ffconf.h against the firmware's to prove no
 * other knob drifted.
 *
 * Why these tests exist
 * ---------------------
 * A device update died with "Firmware update extract failed /lang/ru_ru.bin".
 * In the shipped archive lang/ru_ru.bin is merely the last file BEFORE the one
 * that actually failed: update_bank2.bin, the only root-level file, extracted
 * last. Three defects met there, and each has a test below:
 *
 *   1. Disk full on the final file. f_write() short-writes, the write loop breaks
 *      WITHOUT clearing `success`, the next 512-byte block is the end-of-archive
 *      marker, and extract_tar() returns TRUE. The caller then flashes a
 *      TRUNCATED update_bank2.bin into internal flash bank 2.
 *
 *   2. Any f_open() failure on a root-level path sent create_file() into
 *      create_dir(""), which read temp_path[(size_t)0 - 1] and then walked past
 *      the NUL terminator. Built with -fsanitize=address,undefined so that read
 *      is fatal here rather than silent on the device.
 *
 *   3. The real FRESULT never reached the screen, so a failed update could not
 *      say whether the card was full, read-only, or throwing I/O errors. The
 *      tests assert on the tar_error_t the extractor now fills in.
 *
 * Compile + run (also recorded in tests/test_fw_tar.build):
 *   see tests/run.sh — needs the patched FatFs copy on the include path.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "ram_diskio.h"
#include "tar.h"

/* Mirrors firmware_update.c: the archive is <1 MiB app><4-byte size><tar>. */
#define APP_SIZE (1024 * 1024)
#define PACKAGE_PATH "/retro-go_update.bin"

#define TAR_BLOCK 512
#define VOLUME_SECTORS 16384 /* 8 MiB */
#define CLUSTER_BYTES 512    /* one sector per cluster: makes the disk-full edge exact */

/* Sizes lifted from the two real releases: update_bank2.bin grew by 532 bytes
 * between testbed-full-20260709-0804 (extracted fine) and -20260710-0446 (failed). */
#define BANK2_OLD_SIZE 261464
#define BANK2_NEW_SIZE 261996
#define CORES_SIZE 300000
#define LANG_SIZE 5327

/* ---- tiny test harness (matches tests/test_clock_alarm.c conventions) ---- */
static int g_failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            printf("  FAIL %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            g_failures++;                                       \
        } else {                                                \
            printf("  ok   %s\n", (msg));                       \
        }                                                       \
    } while (0)

/* ---- ustar writer ------------------------------------------------------- */

static void octal(char *dst, size_t width, unsigned long value)
{
    /* width includes the trailing NUL, as GNU tar writes it. */
    snprintf(dst, width, "%0*lo", (int)width - 1, value);
}

static void tar_header(unsigned char *block, const char *name, unsigned long size, char typeflag)
{
    memset(block, 0, TAR_BLOCK);
    strncpy((char *)block, name, 99);
    octal((char *)block + 100, 8, 0000644);
    octal((char *)block + 108, 8, 0);
    octal((char *)block + 116, 8, 0);
    octal((char *)block + 124, 12, size);
    octal((char *)block + 136, 12, 0);
    block[156] = (unsigned char)typeflag;
    memcpy(block + 257, "ustar", 6);
    memcpy(block + 263, "00", 2);

    memset(block + 148, ' ', 8);
    unsigned checksum = 0;
    for (int i = 0; i < TAR_BLOCK; i++)
        checksum += block[i];
    snprintf((char *)block + 148, 8, "%06o", checksum);
    block[155] = ' ';
}

/* Deterministic body so a truncated extraction is detectable. */
static unsigned char body_byte(size_t index) { return (unsigned char)(index * 31u + 7u); }

static size_t tar_append(unsigned char *tar, size_t off, const char *name, size_t size, char typeflag)
{
    tar_header(tar + off, name, typeflag == '5' ? 0 : size, typeflag);
    off += TAR_BLOCK;
    if (typeflag == '5')
        return off;

    for (size_t i = 0; i < size; i++)
        tar[off + i] = body_byte(i);
    off += (size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
    return off;
}

/* The shipped archive's shape: directories, files nested under them, and exactly
 * one root-level file (update_bank2.bin) written last. */
static size_t build_tar(unsigned char *tar, size_t bank2_size)
{
    size_t off = 0;
    off = tar_append(tar, off, "cores/", 0, '5');
    off = tar_append(tar, off, "cores/big.bin", CORES_SIZE, '0');
    off = tar_append(tar, off, "lang/", 0, '5');
    off = tar_append(tar, off, "lang/ru_ru.bin", LANG_SIZE, '0');
    off = tar_append(tar, off, "music/", 0, '5');
    off = tar_append(tar, off, "update_bank2.bin", bank2_size, '0');
    off += 2 * TAR_BLOCK; /* end-of-archive: two zero blocks, buffer is calloc'd */
    return off;
}

/* ---- volume helpers ----------------------------------------------------- */

static FATFS g_fs;

static void mount_fresh_volume(void)
{
    static BYTE work[4096];
    const MKFS_PARM opt = { FM_FAT | FM_SFD, 1, 0, 0, CLUSTER_BYTES };

    ramdisk_create(VOLUME_SECTORS);
    FRESULT res = f_mkfs("", &opt, work, sizeof(work));
    if (res != FR_OK) {
        printf("  FATAL f_mkfs failed: %d\n", res);
        exit(1);
    }
    res = f_mount(&g_fs, "", 1);
    if (res != FR_OK) {
        printf("  FATAL f_mount failed: %d\n", res);
        exit(1);
    }
}

static void unmount_volume(void)
{
    f_unmount("");
    ramdisk_destroy();
}

/* Write `size` bytes of the deterministic body pattern. Returns bytes written. */
static size_t write_file(const char *path, size_t size)
{
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return SIZE_MAX;

    static unsigned char chunk[4096];
    size_t written = 0;
    while (written < size) {
        size_t n = size - written;
        if (n > sizeof(chunk))
            n = sizeof(chunk);
        for (size_t i = 0; i < n; i++)
            chunk[i] = body_byte(written + i);
        UINT bw = 0;
        if (f_write(&f, chunk, (UINT)n, &bw) != FR_OK || bw != n) {
            written += bw;
            break;
        }
        written += n;
    }
    f_close(&f);
    return written;
}

static void write_package(size_t bank2_size)
{
    const size_t tar_capacity = 1u << 20;
    unsigned char *tar = calloc(1, tar_capacity);
    size_t tar_size = build_tar(tar, bank2_size);

    FIL f;
    FRESULT res = f_open(&f, PACKAGE_PATH, FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) {
        printf("  FATAL cannot create package: %d\n", res);
        exit(1);
    }

    static unsigned char zeros[4096];
    UINT bw;
    for (size_t left = APP_SIZE; left; ) {
        UINT n = (UINT)(left > sizeof(zeros) ? sizeof(zeros) : left);
        f_write(&f, zeros, n, &bw);
        left -= n;
    }
    const unsigned char size_le[4] = { 0x00, 0x00, 0x10, 0x00 }; /* unused by extract_tar */
    f_write(&f, size_le, 4, &bw);
    f_write(&f, tar, (UINT)tar_size, &bw);
    f_close(&f);
    free(tar);
}

static bool file_has_size(const char *path, size_t expected)
{
    FILINFO info;
    if (f_stat(path, &info) != FR_OK)
        return false;
    return (size_t)info.fsize == expected;
}

/* Fill every remaining free cluster so the next allocation must fail. */
static void fill_free_space(void)
{
    FATFS *fs;
    DWORD free_clusters;
    if (f_getfree("", &free_clusters, &fs) != FR_OK)
        return;
    write_file("/filler.bin", (size_t)free_clusters * CLUSTER_BYTES);
}

/* ---- progress callback: records the last name extract_tar announced -------- */

static char g_last_announced[256];

static void record_progress(unsigned int percentage, const char *file_name)
{
    (void)percentage;
    if (file_name)
        snprintf(g_last_announced, sizeof(g_last_announced), "%s", file_name);
}

/* Recreate the "same files as the previous update" state the device SD was in. */
static void preseed_previous_update(size_t bank2_size)
{
    f_mkdir("/cores");
    f_mkdir("/lang");
    f_mkdir("/music");
    write_file("/cores/big.bin", CORES_SIZE);
    write_file("/lang/ru_ru.bin", LANG_SIZE);
    write_file("/update_bank2.bin", bank2_size);
}

/* ---- tests -------------------------------------------------------------- */

static void test_happy_path(void)
{
    printf("test_happy_path\n");
    mount_fresh_volume();
    write_package(BANK2_NEW_SIZE);

    tar_error_t err;
    bool ok = extract_tar(PACKAGE_PATH, "", APP_SIZE + sizeof(uint32_t), record_progress, &err);

    CHECK(ok, "extract_tar succeeds on a healthy volume");
    CHECK(err.stage == TAR_STAGE_NONE, "no error is reported on success");
    CHECK(file_has_size("/cores/big.bin", CORES_SIZE), "cores/big.bin fully extracted");
    CHECK(file_has_size("/lang/ru_ru.bin", LANG_SIZE), "lang/ru_ru.bin fully extracted");
    CHECK(file_has_size("/update_bank2.bin", BANK2_NEW_SIZE), "root-level update_bank2.bin fully extracted");
    unmount_volume();
}

/* The brick path. The volume has room for every file at its previous size;
 * update_bank2.bin then grows by 532 bytes and no longer fits. f_write() short-
 * writes on the LAST file, so the next block read is the end-of-archive marker
 * and the loop exits looking successful — with a truncated image on disk that
 * firmware_update.c goes on to flash into internal flash bank 2. */
static void test_disk_full_on_last_file_is_not_silently_accepted(void)
{
    printf("test_disk_full_on_last_file_is_not_silently_accepted\n");
    mount_fresh_volume();
    write_package(BANK2_NEW_SIZE);
    preseed_previous_update(BANK2_OLD_SIZE);
    fill_free_space();

    tar_error_t err;
    bool ok = extract_tar(PACKAGE_PATH, "", APP_SIZE + sizeof(uint32_t), record_progress, &err);

    FILINFO info;
    FSIZE_t on_disk = (f_stat("/update_bank2.bin", &info) == FR_OK) ? info.fsize : 0;
    printf("  (update_bank2.bin on disk: %lu of %d bytes, extract_tar said %s)\n",
           (unsigned long)on_disk, BANK2_NEW_SIZE, ok ? "OK" : "failed");

    CHECK(!ok, "extract_tar reports failure when the last file does not fit");
    CHECK(!(ok && on_disk != BANK2_NEW_SIZE),
          "a truncated update_bank2.bin is never reported as a successful extract");
    CHECK(err.stage == TAR_STAGE_WRITE, "the failure is reported as a write failure");
    CHECK(strcmp(err.path, "/update_bank2.bin") == 0, "the write failure names update_bank2.bin");
    unmount_volume();
}

/* A root-level entry whose f_open() fails used to drive create_file() into
 * create_dir(""), which indexes temp_path[(size_t)-1] and then iterates past the
 * NUL terminator. Built with -fsanitize=address,undefined so the read is fatal.
 *
 * The root directory of a FAT16 volume is a fixed 512-entry table; filling it
 * makes f_open() on a NEW root file return FR_DENIED while the pre-existing
 * subdirectories still accept files. */
static void test_root_level_open_failure_does_not_read_out_of_bounds(void)
{
    printf("test_root_level_open_failure_does_not_read_out_of_bounds\n");
    mount_fresh_volume();
    write_package(BANK2_NEW_SIZE);
    f_mkdir("/cores");
    f_mkdir("/lang");
    f_mkdir("/music");

    /* Consume every remaining root-directory slot with empty 8.3-named files. */
    int created = 0;
    for (int i = 0; i < 600; i++) {
        char name[24];
        snprintf(name, sizeof(name), "/F%04d.BIN", i);
        FIL f;
        if (f_open(&f, name, FA_WRITE | FA_CREATE_NEW) != FR_OK)
            break;
        f_close(&f);
        created++;
    }
    CHECK(created > 0 && created < 600, "root directory filled to its FAT16 limit");

    g_last_announced[0] = '\0';
    tar_error_t err;
    bool ok = extract_tar(PACKAGE_PATH, "", APP_SIZE + sizeof(uint32_t), record_progress, &err);
    printf("  (stage=%s res=%d path=%s, last announced=%s)\n",
           tar_stage_name(err.stage), err.res, err.path, g_last_announced);

    CHECK(!ok, "extract_tar reports failure when the root-level file cannot be created");
    /* The screen's only clue. It must name the file that failed, not the one
     * before it, and it must carry the FatFs code that explains why. */
    CHECK(err.stage == TAR_STAGE_CREATE, "the failure is reported as a create failure");
    CHECK(strcmp(err.path, "/update_bank2.bin") == 0, "the failing file is the one reported");
    CHECK(err.res == FR_DENIED, "the underlying FRESULT survives to the caller");
    unmount_volume();
}

/* A directory that cannot be created must fail the extract, not end it quietly:
 * `if (res != FR_OK) break;` left success == true. */
static void test_mkdir_failure_is_not_silently_accepted(void)
{
    printf("test_mkdir_failure_is_not_silently_accepted\n");
    mount_fresh_volume();
    write_package(BANK2_NEW_SIZE);
    fill_free_space(); /* no free cluster => f_mkdir("/cores") cannot allocate */

    tar_error_t err;
    bool ok = extract_tar(PACKAGE_PATH, "", APP_SIZE + sizeof(uint32_t), record_progress, &err);

    CHECK(!ok, "extract_tar reports failure when a directory cannot be created");
    CHECK(err.stage == TAR_STAGE_MKDIR, "the failure is reported as a mkdir failure");
    unmount_volume();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0); /* a sanitizer abort must not eat prior results */

    test_happy_path();
    test_disk_full_on_last_file_is_not_silently_accepted();
    test_root_level_open_failure_does_not_read_out_of_bounds();
    test_mkdir_failure_is_not_silently_accepted();

    printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
