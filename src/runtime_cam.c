/* Direct mouse camera for Bloodborne 1.09.
 *
 * The stick path cannot turn faster than a full right-stick deflection, and a
 * normal mouse speed already pins that deflection, so the camera moves at the
 * controller's fixed rate. This writes the camera's own yaw and pitch instead
 * (the hook from the Windows port's bloodborne_cam.cpp, aimed at this image).
 * While a target is locked, the stick path stays in charge so a flick can
 * change targets. BB_MOUSE_CAMERA=stick, or mouse_camera=stick in bbport.ini,
 * keeps the stick. The hook is installed only while the window holds the mouse,
 * so a free cursor leaves the right stick on the camera. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif

static const unsigned char k_main[18] = {
    0xC4, 0xC1, 0x7A, 0x10, 0x85, 0x40, 0x01, 0x00, 0x00,
    0xC4, 0xC1, 0x7A, 0x10, 0x8D, 0x50, 0x01, 0x00, 0x00};
/* vmovss stores the game uses to publish pitch/yaw. Offsets from k_main. */
static const int k_nop_delta[4] = {-0x7C2, -0x526, -0x63A, 0xF3C};
static const unsigned char k_nop_bytes[4][9] = {
    {0xC4, 0xC1, 0x7A, 0x11, 0x85, 0x44, 0x01, 0x00, 0x00},
    {0xC4, 0xC1, 0x7A, 0x11, 0x8D, 0x30, 0x01, 0x00, 0x00},
    {0xC4, 0xC1, 0x7A, 0x11, 0xAD, 0x44, 0x01, 0x00, 0x00},
    {0xC4, 0xC1, 0x7A, 0x11, 0x85, 0x94, 0x02, 0x00, 0x00}};
static const float k_counts_per_radian = 900.0f;

static unsigned char *cam_image;
static size_t cam_image_size;
static unsigned char *cam_main;
static unsigned char *cam_nop[4];
static unsigned char cam_main_orig[18];
static unsigned char cam_nop_orig[4][9];
static unsigned char *cam_tramp;
static int cam_ready;
static int cam_hooked;
static int cam_told;

static int cam_disabled(void) {
    const char *env = getenv("BB_MOUSE_CAMERA");
    if (env && *env && strcmp(env, "direct") != 0 && strcmp(env, "1") != 0) return 1;
    const char *path = getenv("BB_CONFIG");
    FILE *f = path && *path ? fopen(path, "r") : NULL;
    if (!f) return 0;
    char line[256];
    int off = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "mouse_camera=", 13) != 0) continue;
        off = strcmp(line + 13, "stick\n") == 0 || strcmp(line + 13, "stick") == 0 ||
              line[13] == '0';
    }
    fclose(f);
    return off;
}

static int cam_pages(int writable) {
#ifndef _WIN32
    uintptr_t lo = (uintptr_t)cam_main, hi = lo + sizeof k_main;
    for (int i = 0; i < 4; ++i) {
        uintptr_t a = (uintptr_t)cam_nop[i];
        if (a < lo) lo = a;
        if (a + 9 > hi) hi = a + 9;
    }
    const long page = sysconf(_SC_PAGESIZE);
    uintptr_t start = lo & ~(uintptr_t)(page - 1);
    uintptr_t end = (hi + (uintptr_t)page - 1) & ~(uintptr_t)(page - 1);
    int prot = PROT_READ | PROT_EXEC | (writable ? PROT_WRITE : 0);
    if (mprotect((void *)start, end - start, prot) != 0) return 0;
    __builtin___clear_cache((char *)start, (char *)end);
    return 1;
#else
    (void)writable;
    return 0;
#endif
}

static void cam_write_jump(unsigned char *site) {
    memset(site, 0x90, 18);
    site[0] = 0xFF;
    site[1] = 0x25; /* jmp qword ptr [rip+0]; the address sits in the next 8 bytes */
    site[2] = site[3] = site[4] = site[5] = 0;
    uint64_t dest = (uint64_t)(uintptr_t)cam_tramp;
    memcpy(site + 6, &dest, 8);
}

static int cam_build_tramp(void) {
#ifndef _WIN32
    unsigned char *t = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) return 0;
    memset(t, 0, 0x48);
    t[0x00] = 0x4D; t[0x01] = 0x85; t[0x02] = 0xED;                         /* test r13, r13 */
    t[0x03] = 0x74; t[0x04] = 0x2B;                                         /* jz cam_skip */
    t[0x05] = 0x4C; t[0x06] = 0x89; t[0x07] = 0x2D;                         /* mov [rip+0x2C], r13 */
    t[0x08] = 0x2C;
    t[0x0C] = 0x41; t[0x0D] = 0xC7; t[0x0E] = 0x85;                         /* mov dword [r13+0x130], 0.7f */
    t[0x0F] = 0x30; t[0x10] = 0x01;
    t[0x13] = 0x33; t[0x14] = 0x33; t[0x15] = 0x33; t[0x16] = 0x3F;
    memcpy(t + 0x17, k_main, sizeof k_main);
    t[0x29] = 0xFF; t[0x2A] = 0x25; t[0x2B] = 0x11;                         /* jmp [return] */
    t[0x30] = 0xFF; t[0x31] = 0x25; t[0x32] = 0x0A;                         /* cam_skip: same */
    uint64_t ret = (uint64_t)(uintptr_t)(cam_main + sizeof k_main);
    memcpy(t + 0x40, &ret, 8);
    __builtin___clear_cache((char *)t, (char *)t + 0x48);
    cam_tramp = t;
    return 1;
#else
    return 0;
#endif
}

void runtime_cam_prepare(const void *image, size_t size) {
    cam_image = (unsigned char *)image;
    cam_image_size = size;
    cam_ready = 0;
    if (!image || size < sizeof k_main || cam_disabled()) {
        if (!cam_told++) printf("Mouse camera: stick mode\n");
        return;
    }
    const unsigned char *base = cam_image;
    const unsigned char *end = base + size - sizeof k_main;
    const unsigned char *found = NULL;
    for (const unsigned char *p = base; p <= end; ++p) {
        if (memcmp(p, k_main, sizeof k_main) != 0) continue;
        found = p;
        break;
    }
    if (!found) {
        if (!cam_told++) printf("Mouse camera: camera code not found; stick mode\n");
        return;
    }
    cam_main = (unsigned char *)found;
    memcpy(cam_main_orig, cam_main, sizeof k_main);
    for (int i = 0; i < 4; ++i) {
        unsigned char *site = cam_main + k_nop_delta[i];
        if (site < cam_image || site + 9 > cam_image + cam_image_size ||
            memcmp(site, k_nop_bytes[i], 9) != 0) {
            if (!cam_told++) printf("Mouse camera: camera stores changed; stick mode\n");
            cam_main = NULL;
            return;
        }
        cam_nop[i] = site;
        memcpy(cam_nop_orig[i], site, 9);
    }
    if (!cam_build_tramp()) {
        if (!cam_told++) printf("Mouse camera: trampoline failed; stick mode\n");
        return;
    }
    cam_ready = 1;
    if (!cam_told++) printf("Mouse camera: direct mode ready (1 count of sensitivity 1 = 1/900 rad)\n");
}

int runtime_cam_set_hook(int on) {
    if (!cam_ready) return 0;
    if (on == cam_hooked) return cam_hooked;
    if (!cam_pages(1)) {
        cam_ready = 0;
        printf("Mouse camera: code page not writable; stick mode\n");
        return 0;
    }
    if (on) {
        cam_write_jump(cam_main);
        for (int i = 0; i < 4; ++i) memset(cam_nop[i], 0x90, 9);
        printf("Mouse camera: direct mode on\n");
    } else {
        memcpy(cam_main, cam_main_orig, sizeof k_main);
        for (int i = 0; i < 4; ++i) memcpy(cam_nop[i], cam_nop_orig[i], 9);
        printf("Mouse camera: direct mode off\n");
    }
    cam_pages(0);
    cam_hooked = on;
    return cam_hooked;
}

static float cam_load(uintptr_t address) {
    float value;
    memcpy(&value, (const void *)address, sizeof value);
    return value;
}
static void cam_store(uintptr_t address, float value) {
    memcpy((void *)address, &value, sizeof value);
}

int runtime_cam_motion(float dx, float dy, float sensitivity, int invert_y, int *locked) {
    if (locked) *locked = 0;
    if (!cam_hooked || !cam_tramp) return 0;
    uint64_t cam = 0;
    memcpy(&cam, cam_tramp + 0x38, sizeof cam);
    if (!cam || !runtime_memory_is_mapped((uintptr_t)cam, 0x280)) return 0;
    const float stamp = cam_load(cam + 0x130);
    if (!(stamp > 0.699f && stamp < 0.701f)) return 0;
    if (cam_load(cam + 0x154) == 1.0f) {
        if (locked) *locked = 1;
        return 0;
    }
    if (dx == 0.0f && dy == 0.0f) return 1;
    if (!(sensitivity > 0.0f) || sensitivity > 20.0f) sensitivity = 1.0f;
    const float scale = sensitivity / k_counts_per_radian;
    const float delta_yaw = dx * scale;
    const float delta_pitch = (invert_y ? -dy : dy) * scale;
    float pitch = cam_load(cam + 0x140) + delta_pitch;
    float yaw = cam_load(cam + 0x144) + delta_yaw;
    float min_pitch = cam_load(cam + 0x1F0);
    float max_pitch = cam_load(cam + 0x1FC);
    if (!(min_pitch < max_pitch) || min_pitch < -3.0f || max_pitch > 3.0f) {
        min_pitch = -1.94f;
        max_pitch = 1.71f;
    }
    if (pitch > max_pitch) pitch = max_pitch;
    else if (pitch < min_pitch) pitch = min_pitch;
    const float turn = 6.28318531f;
    if (yaw > 3.14159265f) yaw -= turn;
    else if (yaw < -3.14159265f) yaw += turn;
    cam_store(cam + 0x140, pitch);
    cam_store(cam + 0x150, pitch);
    cam_store(cam + 0x26C, pitch);
    cam_store(cam + 0x144, yaw);
    cam_store(cam + 0x270, yaw);
    return 1;
}
