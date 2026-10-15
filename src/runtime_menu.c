/* Mouse pointer in Bloodborne 1.09 menus.
 *
 * Every list the menus navigate — title, System, pause, inventory, dialogs —
 * is one component. Its update reads the pad. Dark Souls III does the same and
 * then hit-tests the row under the pointer. This does that hit test: the row's
 * Cursor clip, on the live display object, and moves the highlight with the
 * game's own cursor function. A click confirms only the row it landed on.
 * The cursor is shown while the title or a menu has the screen, and the camera
 * takes the mouse back when that ends.
 *
 * Addresses are image offsets (the eboot's link address minus 0x400000).
 * BB_MOUSE_MENU=0 still reports "in a menu" but does not move a highlight.
 */
#define _GNU_SOURCE
#include "runtime.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

enum {
    OFF_LIST_UPDATE = 0x1acff30,
    OFF_LIST_VTABLE = 0x5399fb0,
    OFF_VTABLE_SLOT = 3,
    OFF_PAD = 0x1ac1e02,
    OFF_SET_CURSOR = 0x1acf6b0,
    OFF_SCROLL = 0x1ad0dd0,
    OFF_HIDE = 0x1acf0e0,
    OFF_CHILD = 0x1adb220,
    OFF_CURSOR_NAME = 0x4934044,
    OFF_WORLD = 0x2040,
    OFF_FLOW = 0x5580e38,
    OFF_REGION = 0x5128948,
    OFF_BLOCK_B = 0x5562878,
    OFF_MENU_SE = 0x557fc60,
    OFF_MENU_SE_LABEL = 0x557fc68,
    OFF_MOVE_LABEL = 0x49344e4,
    OFF_EXEC_END = 0x50d96dc,
    FLOW_STARTED = 0xb11,
    FLOW_MENU = 0xa88,
    FLOW_BLOCK = 0xa78,
    BLOCK_B = 0x2e4
};

static const struct { uint32_t at; uint8_t op; } k_calls[] = {
    {0x1b25226, 0xe8}, {0x1b26066, 0xe8}, {0x1b26f28, 0xe8}, {0x1b72c80, 0xe8}, {0x1b108c7, 0xe9}};
static const unsigned char k_pad_bytes[14] = {
    0x66, 0x66, 0x66, 0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};

enum {
    F_LISTENER = 0x30, F_COUNT = 0x70, F_CURSOR = 0x74, F_OVERRIDE = 0x78,
    F_CELLS = 0x7c, F_LINES = 0x80, F_CELL_SCROLL = 0x84, F_LINE_SCROLL = 0x24c,
    F_PROVIDER = 0x258, F_ROOT_TYPE = 0xc8, F_ROOT_DATA = 0xd0, F_WRAP = 0x2b0,
    W_TYPE = 0x48, W_IFACE = 0x40, W_DATA = 0x50, W_VALUE = 0x30,
    NATIVE = 0x88, PARENT = 0x38, FLAGS = 0x6b, VISIBLE = 0x40, GET_BOUNDS = 0x1f0
};

static unsigned char *image;
static size_t image_size;
static int ready, passive, logged;
static _Atomic uint32_t press, pointer_x_bits, pointer_y_bits;
static _Atomic int click_armed, direction_held, typing, wheel_left;
static _Atomic int64_t click_ms, focus_ms;
static float load_f(_Atomic uint32_t *bits) {
    uint32_t u = __atomic_load_n(bits, __ATOMIC_RELAXED);
    float f; memcpy(&f, &u, 4); return f;
}
static void store_f(_Atomic uint32_t *bits, float f) {
    uint32_t u; memcpy(&u, &f, 4);
    __atomic_store_n(bits, u, __ATOMIC_RELAXED);
}
static void (ABI *list_original)(uint8_t *comp, uint8_t *input);

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int rd(uint64_t addr, void *out, size_t len) {
    if (addr < 0x10000 || !out) return 0;
    struct iovec local = {out, len}, remote = {(void *)(uintptr_t)addr, len};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)len;
}
static uint32_t r32(const uint8_t *p, size_t off) {
    uint32_t v; memcpy(&v, p + off, 4); return v;
}
static int32_t r32s(const uint8_t *p, size_t off) {
    int32_t v; memcpy(&v, p + off, 4); return v;
}
static uint64_t r64(const uint8_t *p, size_t off) {
    uint64_t v; memcpy(&v, p + off, 8); return v;
}
static int guest_code(uint64_t addr) {
    if (!image || addr < (uint64_t)(uintptr_t)image || addr >= (uint64_t)(uintptr_t)image + OFF_EXEC_END)
        return 0;
    unsigned char p[4];
    return rd(addr, p, 4) && p[0] == 0x55 && p[1] == 0x48 && p[2] == 0x89 && p[3] == 0xe5;
}
static uint32_t confirm_button(void) {
    int32_t region = 0;
    if (image && OFF_REGION + 4 <= image_size) memcpy(&region, image + OFF_REGION, 4);
    return region == 5 ? 0x2000u : 0x4000u;
}
static uint32_t back_button(void) { return confirm_button() == 0x2000u ? 0x4000u : 0x2000u; }
static void queue_button(uint32_t mask) {
    if (mask) __atomic_fetch_or(&press, mask, __ATOMIC_RELAXED);
}

static int flow_ptr(uint64_t *out) {
    if (!image || OFF_FLOW + 8 > image_size) return 0;
    memcpy(out, image + OFF_FLOW, 8);
    return *out != 0;
}
static int front_end(void) {
    uint64_t flow = 0;
    uint8_t started = 1;
    return flow_ptr(&flow) && rd(flow + FLOW_STARTED, &started, 1) && !started;
}
static int menu_open(void) {
    uint64_t flow = 0, menu = 0;
    return flow_ptr(&flow) && rd(flow + FLOW_MENU, &menu, 8) && menu;
}
static int input_blocked(void) {
    uint64_t flow = 0, block = 0, other = 0;
    uint32_t flag = 0;
    if (flow_ptr(&flow) && rd(flow + FLOW_BLOCK, &block, 8) && block) return 1;
    if (image && OFF_BLOCK_B + 8 <= image_size) memcpy(&other, image + OFF_BLOCK_B, 8);
    return other && rd(other + BLOCK_B, &flag, 4) && flag;
}

static void wrapper_release(uint8_t *b) {
    if (!(r32(b, W_TYPE) & 0x40)) return;
    uint64_t iface = r64(b, W_IFACE), vt = 0, rel = 0;
    if (!iface || !rd(iface, &vt, 8) || !rd(vt + 0x18, &rel, 8) || !guest_code(rel)) return;
    typedef void (ABI *release_fn)(void *, uint8_t *, uint64_t);
    ((release_fn)(uintptr_t)rel)((void *)(uintptr_t)iface, b + W_VALUE, r64(b, W_DATA));
}
static uint64_t wrapper_display(const uint8_t *b) {
    if ((r32(b, W_TYPE) & 0x8f) != 10) return 0;
    uint64_t data = r64(b, W_DATA), native = 0;
    if (!data || !rd(data + NATIVE, &native, 8)) return 0;
    return native;
}
static int shown(uint64_t obj) {
    for (int depth = 0; obj && depth < 32; ++depth) {
        uint8_t flags = 0;
        uint64_t parent = 0;
        if (!rd(obj + FLAGS, &flags, 1) || !(flags & VISIBLE)) return 0;
        if (!rd(obj + PARENT, &parent, 8)) return 0;
        obj = parent;
    }
    return 1;
}

struct Rect { float x1, y1, x2, y2; };
static int hit_obj(uint64_t obj, float sx, float sy, struct Rect *world, float grow) {
    uint64_t vt = 0, get_bounds = 0;
    if (!rd(obj, &vt, 8) || !rd(vt + GET_BOUNDS, &get_bounds, 8) || !guest_code(get_bounds)) return 0;
    _Alignas(16) float identity[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    _Alignas(16) float rect[4] = {0};
    typedef void (ABI *bounds_fn)(float *, void *, const float *);
    ((bounds_fn)(uintptr_t)get_bounds)(rect, (void *)(uintptr_t)obj, identity);
    if (!(rect[2] > rect[0]) || !(rect[3] > rect[1])) return 0;
    _Alignas(16) float m[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    typedef void (ABI *world_fn)(void *, float *);
    if (!guest_code((uint64_t)(uintptr_t)image + OFF_WORLD)) return 0;
    ((world_fn)(uintptr_t)(image + OFF_WORLD))((void *)(uintptr_t)obj, m);
    if (world) {
        const float xs[2] = {rect[0], rect[2]}, ys[2] = {rect[1], rect[3]};
        world->x1 = world->y1 = 1e9f;
        world->x2 = world->y2 = -1e9f;
        for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) {
            float wx = (m[0] * xs[i] + m[1] * ys[j] + m[3]) / 20.0f;
            float wy = (m[4] * xs[i] + m[5] * ys[j] + m[7]) / 20.0f;
            if (wx < world->x1) world->x1 = wx;
            if (wx > world->x2) world->x2 = wx;
            if (wy < world->y1) world->y1 = wy;
            if (wy > world->y2) world->y2 = wy;
        }
    }
    const float det = m[0] * m[5] - m[1] * m[4];
    if (fabsf(det) < 1e-12f) return 0;
    const float px = sx * 20.0f - m[3], py = sy * 20.0f - m[7];
    const float lx = (m[5] * px - m[1] * py) / det;
    const float ly = (-m[4] * px + m[0] * py) / det;
    const float g = grow * 20.0f / sqrtf(fabsf(det));
    return lx >= rect[0] - g && lx <= rect[2] + g && ly >= rect[1] - g && ly <= rect[3] + g;
}

static int child(uint8_t *out, uint8_t *parent, const void *name) {
    memset(out, 0, 0x80);
    typedef void (ABI *child_fn)(uint8_t *, uint8_t *, const void *, int);
    if (!guest_code((uint64_t)(uintptr_t)image + OFF_CHILD)) return 0;
    ((child_fn)(uintptr_t)(image + OFF_CHILD))(out, parent, name, 0);
    return 1;
}

static int cursor_of(const uint8_t *c) {
    int32_t over = r32s(c, F_OVERRIDE);
    return over >= 0 ? over : r32s(c, F_CURSOR);
}

/* Absolute index under the pointer, or -1. packed receives the visible cell. */
static int item_at(uint8_t *c, float sx, float sy, uint64_t *packed_out) {
    const int count = r32s(c, F_COUNT), cells = r32s(c, F_CELLS), lines = r32s(c, F_LINES);
    const int cell_scroll = r32s(c, F_CELL_SCROLL), line_scroll = r32s(c, F_LINE_SCROLL);
    const int wraps = c[F_WRAP] != 0;
    if (count <= 0 || cells <= 0 || lines <= 0 || cells > 16 || lines > 16) return -1;
    const int stride = lines != 1 ? cells : (cells > count ? cells : count);
    uint64_t provider = r64(c, F_PROVIDER), pvt = 0, make = 0;
    if (!provider || !rd(provider, &pvt, 8) || !rd(pvt + 0x18, &make, 8) || !guest_code(make)) return -1;
    typedef void (ABI *make_fn)(uint8_t *, void *, uint64_t *);
    int best = -1;
    float best_d = 0;
    uint64_t best_packed = 0;
    for (int line = 0; line < lines; ++line) {
        for (int cell = 0; cell < cells; ++cell) {
            int index = (line_scroll + line) * stride + cell_scroll + cell;
            if (wraps && count) index %= count;
            if (index < 0 || index >= count) continue;
            uint64_t packed = (uint32_t)cell | ((uint64_t)(uint32_t)line << 32);
            _Alignas(16) uint8_t item[0x80], region[0x80];
            memset(item, 0, sizeof item);
            ((make_fn)(uintptr_t)make)(item, (void *)(uintptr_t)provider, &packed);
            uint64_t obj = wrapper_display(item);
            if (!obj || !shown(obj)) { wrapper_release(item); continue; }
            child(region, item, image + OFF_CURSOR_NAME);
            uint64_t target = wrapper_display(region);
            struct Rect w = {0};
            int in = target && hit_obj(target, sx, sy, &w, 0);
            if (!target) {
                target = obj;
                in = hit_obj(obj, sx, sy, &w, 0);
            }
            static const char *parts[] = {"Widgets/ComboBox", "Widgets/Slider"};
            for (int p = 0; p < 2 && !in; ++p) {
                _Alignas(16) uint8_t widget[0x80];
                child(widget, item, parts[p]);
                uint64_t wobj = wrapper_display(widget);
                struct Rect ww = {0};
                if (wobj && shown(wobj) && hit_obj(wobj, sx, sy, &ww, 0)) { in = 1; w = ww; }
                wrapper_release(widget);
            }
            if (in) {
                float dx = (w.x1 + w.x2) * 0.5f - sx, dy = (w.y1 + w.y2) * 0.5f - sy;
                float d = dx * dx + dy * dy;
                if (best < 0 || d < best_d) { best = index; best_d = d; best_packed = packed; }
            }
            wrapper_release(region);
            wrapper_release(item);
        }
    }
    if (packed_out) *packed_out = best_packed;
    return best;
}

static int part_hit(uint8_t *item, const char *path, struct Rect *w, float grow) {
    _Alignas(16) uint8_t part[0x80];
    child(part, item, path);
    uint64_t obj = wrapper_display(part);
    int in = obj && shown(obj) && hit_obj(obj, load_f(&pointer_x_bits), load_f(&pointer_y_bits), w, grow);
    wrapper_release(part);
    return in;
}

static uint32_t click_button(uint8_t *c, uint64_t packed, float sx) {
    uint64_t provider = r64(c, F_PROVIDER), pvt = 0, make = 0;
    if (!provider || !rd(provider, &pvt, 8) || !rd(pvt + 0x18, &make, 8) || !guest_code(make))
        return confirm_button();
    typedef void (ABI *make_fn)(uint8_t *, void *, uint64_t *);
    _Alignas(16) uint8_t item[0x80];
    memset(item, 0, sizeof item);
    ((make_fn)(uintptr_t)make)(item, (void *)(uintptr_t)provider, &packed);
    uint32_t button = confirm_button();
    if (wrapper_display(item)) {
        struct Rect w = {0};
        if (part_hit(item, "Widgets/Slider/LeftArrow", &w, 14)) button = 0x80;
        else if (part_hit(item, "Widgets/Slider/RightArrow", &w, 14)) button = 0x20;
        else if (part_hit(item, "Widgets/ComboBox", &w, 0))
            button = sx < (w.x1 + w.x2) * 0.5f ? 0x80 : 0x20;
    }
    wrapper_release(item);
    return button;
}

static void show_cursor(uint8_t *c, uint64_t packed) {
    uint64_t provider = r64(c, F_PROVIDER), pvt = 0, make = 0;
    if (!provider || !rd(provider, &pvt, 8) || !rd(pvt + 0x18, &make, 8) || !guest_code(make)) return;
    typedef void (ABI *make_fn)(uint8_t *, void *, uint64_t *);
    _Alignas(16) uint8_t item[0x80], cursor[0x80];
    memset(item, 0, sizeof item);
    ((make_fn)(uintptr_t)make)(item, (void *)(uintptr_t)provider, &packed);
    child(cursor, item, image + OFF_CURSOR_NAME);
    uint64_t vt = r64(cursor, 0), set_visible = 0;
    if (wrapper_display(cursor) && vt && rd(vt + 8, &set_visible, 8) && guest_code(set_visible)) {
        typedef void (ABI *vis_fn)(uint8_t *, int);
        ((vis_fn)(uintptr_t)set_visible)(cursor, 1);
    }
    wrapper_release(cursor);
    wrapper_release(item);
}

static void pointer_step(uint8_t *comp, const uint8_t *input) {
    if (!input || !*input || input_blocked()) return;
    __atomic_store_n(&focus_ms, now_ms(), __ATOMIC_RELAXED);
    if (passive || __atomic_load_n(&typing, __ATOMIC_RELAXED)) {
        __atomic_store_n(&wheel_left, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&click_armed, 0, __ATOMIC_RELAXED);
        return;
    }
    const float sx = load_f(&pointer_x_bits);
    const float sy = load_f(&pointer_y_bits);
    const int lines = r32s(comp, F_LINES), cells = r32s(comp, F_CELLS), count = r32s(comp, F_COUNT);
    int wheel = __atomic_exchange_n(&wheel_left, 0, __ATOMIC_RELAXED);
    if (wheel && lines == 1 && cells == 1 && count > 1) {
        queue_button(wheel > 0 ? 0x80u : 0x20u);
        wheel = 0;
    } else if (wheel && guest_code((uint64_t)(uintptr_t)image + OFF_SCROLL)) {
        typedef void (ABI *scroll_fn)(uint8_t *, int, int);
        ((scroll_fn)(uintptr_t)(image + OFF_SCROLL))(comp, 0, -wheel);
    }
    if (lines == 1 && cells == 1 && count > 1) {
        int click = __atomic_load_n(&click_armed, __ATOMIC_RELAXED);
        if (!click) return;
        uint64_t data = r64(comp, F_ROOT_DATA), root = 0;
        if ((r32(comp, F_ROOT_TYPE) & 0x8f) != 10 || !data || !rd(data + NATIVE, &root, 8)) return;
        struct Rect w = {0};
        if (root && shown(root) && hit_obj(root, sx, sy, &w, 0) &&
            __atomic_exchange_n(&click_armed, 0, __ATOMIC_RELAXED))
            queue_button(sx < (w.x1 + w.x2) * 0.5f ? 0x80u : 0x20u);
        return;
    }
    struct Seen { uint8_t *comp; float x, y; int64_t ms; };
    static struct Seen seen[32];
    static int seen_next;
    int64_t now = now_ms();
    int slot = -1;
    for (int i = 0; i < 32; ++i) if (seen[i].comp == comp) slot = i;
    if (slot < 0) {
        slot = seen_next++ % 32;
        seen[slot].comp = comp;
        seen[slot].x = sx;
        seen[slot].y = sy;
        seen[slot].ms = now;
    }
    int fresh = now - seen[slot].ms > 250 && seen[slot].comp == comp && seen[slot].ms != now;
    int moved = !fresh && slot >= 0 && (sx != seen[slot].x || sy != seen[slot].y);
    /* A component seen for the first time this call has ms == now, so it is not a move. */
    if (seen[slot].ms == now) moved = 0;
    seen[slot].x = sx;
    seen[slot].y = sy;
    seen[slot].ms = now;
    int click = __atomic_load_n(&click_armed, __ATOMIC_RELAXED);
    if (click && now - __atomic_load_n(&click_ms, __ATOMIC_RELAXED) > 250) {
        __atomic_store_n(&click_armed, 0, __ATOMIC_RELAXED);
        click = 0;
    }
    if (!moved && !click) return;
    if (__atomic_load_n(&direction_held, __ATOMIC_RELAXED) && !click) return;
    uint64_t packed = 0;
    int want = item_at(comp, sx, sy, &packed);
    if (want < 0) return;
    if (click) __atomic_store_n(&click_armed, 0, __ATOMIC_RELAXED);
    int was = cursor_of(comp), now_i = was;
    if (want != was && guest_code((uint64_t)(uintptr_t)image + OFF_SET_CURSOR)) {
        typedef int (ABI *set_fn)(uint8_t *, int);
        typedef void (ABI *scroll_fn)(uint8_t *, int, int);
        typedef void (ABI *hide_fn)(uint8_t *);
        int s1 = r32s(comp, F_CELL_SCROLL), s2 = r32s(comp, F_LINE_SCROLL);
        int result = ((set_fn)(uintptr_t)(image + OFF_SET_CURSOR))(comp, want);
        int d1 = s1 - r32s(comp, F_CELL_SCROLL), d2 = s2 - r32s(comp, F_LINE_SCROLL);
        if (result == 1 && (d1 || d2) && guest_code((uint64_t)(uintptr_t)image + OFF_SCROLL)) {
            ((scroll_fn)(uintptr_t)(image + OFF_SCROLL))(comp, d1, d2);
            if (guest_code((uint64_t)(uintptr_t)image + OFF_HIDE))
                ((hide_fn)(uintptr_t)(image + OFF_HIDE))(comp);
            show_cursor(comp, packed);
        }
        now_i = cursor_of(comp);
        if (logged) printf("Menu pointer: row %d -> %d\n", was, now_i);
    }
    if (click && now_i == want) queue_button(click_button(comp, packed, sx));
    if (now_i == was) return;
    uint64_t listener = r64(comp, F_LISTENER), lvt = 0, call = 0;
    if (listener && rd(listener, &lvt, 8) && rd(lvt + 0x10, &call, 8) && guest_code(call)) {
        typedef void (ABI *listen_fn)(void *, int *);
        ((listen_fn)(uintptr_t)call)((void *)(uintptr_t)listener, &now_i);
    }
    if (OFF_MENU_SE + 8 <= image_size) {
        int32_t *se = (int32_t *)(image + OFF_MENU_SE);
        if (se[1] <= 0) {
            se[0] = 0;
            se[1] = 0;
            memcpy(image + OFF_MENU_SE_LABEL, &(uint64_t){(uint64_t)(uintptr_t)(image + OFF_MOVE_LABEL)}, 8);
        }
    }
}

static __thread int in_hook;
static void ABI list_update(uint8_t *comp, uint8_t *input) {
    if (in_hook) { list_original(comp, input); return; }
    in_hook = 1;
    pointer_step(comp, input);
    list_original(comp, input);
    in_hook = 0;
}

static int protect_code(void *p, size_t n, int write) {
    long page = sysconf(_SC_PAGESIZE);
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)(page - 1);
    uintptr_t b = ((uintptr_t)p + n + (uintptr_t)page - 1) & ~(uintptr_t)(page - 1);
    int prot = PROT_READ | PROT_EXEC | (write ? PROT_WRITE : 0);
    if (mprotect((void *)a, b - a, prot) != 0) return 0;
    __builtin___clear_cache((char *)a, (char *)b);
    return 1;
}

void runtime_menu_prepare(const void *base, size_t size) {
    const char *env = getenv("BB_MOUSE_MENU");
    passive = env && env[0] == '0';
    logged = 0;
    const char *log = getenv("BB_MOUSE_MENU_LOG");
    if (log && log[0] == '1') logged = 1;
    image = (unsigned char *)base;
    image_size = size;
    ready = 0;
    if (!image || size < OFF_EXEC_END) return;
    unsigned char *update = image + OFF_LIST_UPDATE;
    unsigned char *pad = image + OFF_PAD;
    uint64_t *slot = (uint64_t *)(image + OFF_LIST_VTABLE + OFF_VTABLE_SLOT * 8);
    if (update[0] != 0x55 || memcmp(pad, k_pad_bytes, sizeof k_pad_bytes) != 0 ||
        *slot != (uint64_t)(uintptr_t)update) {
        printf("Menu pointer: list update is not the 1.09 code, left off\n");
        return;
    }
    for (size_t i = 0; i < sizeof k_calls / sizeof k_calls[0]; ++i) {
        unsigned char *site = image + k_calls[i].at;
        int32_t rel = 0;
        memcpy(&rel, site + 1, 4);
        if (site[0] != k_calls[i].op || (uintptr_t)(site + 5 + rel) != (uintptr_t)update) {
            printf("Menu pointer: caller %#x does not reach the list update, left off\n", k_calls[i].at);
            return;
        }
    }
    /* mov r11, hook; jmp r11 in the 14 bytes of padding before the next function. */
    unsigned char stub[13];
    uint64_t hook = (uint64_t)(uintptr_t)&list_update;
    stub[0] = 0x49; stub[1] = 0xbb;
    memcpy(stub + 2, &hook, 8);
    stub[10] = 0x41; stub[11] = 0xff; stub[12] = 0xe3;
    uintptr_t lo = (uintptr_t)pad, hi = (uintptr_t)pad + sizeof k_pad_bytes;
    for (size_t i = 0; i < sizeof k_calls / sizeof k_calls[0]; ++i) {
        uintptr_t s = (uintptr_t)(image + k_calls[i].at);
        if (s < lo) lo = s;
        if (s + 5 > hi) hi = s + 5;
    }
    if (!protect_code((void *)lo, hi - lo, 1)) {
        printf("Menu pointer: cannot write the list update, left off\n");
        return;
    }
    memcpy(pad, stub, sizeof stub);
    for (size_t i = 0; i < sizeof k_calls / sizeof k_calls[0]; ++i) {
        unsigned char *site = image + k_calls[i].at;
        int32_t rel = (int32_t)((intptr_t)pad - (intptr_t)(site + 5));
        memcpy(site + 1, &rel, 4);
    }
    protect_code((void *)lo, hi - lo, 0);
    *slot = hook;
    list_original = (void (ABI *)(uint8_t *, uint8_t *))update;
    ready = 1;
    printf("Menu pointer: on. The cursor shows in menus; click a row, right click goes back.\n");
}

int runtime_menu_ready(void) { return ready; }
int runtime_menu_in_menu(void) {
    if (!ready) return 0;
    if (front_end()) return 1;
    int64_t seen = __atomic_load_n(&focus_ms, __ATOMIC_RELAXED);
    return menu_open() || (seen && now_ms() - seen < 400);
}
uint32_t runtime_menu_take_press(void) {
    return __atomic_exchange_n(&press, 0, __ATOMIC_RELAXED);
}
void runtime_menu_set_direction(int held) {
    __atomic_store_n(&direction_held, held ? 1 : 0, __ATOMIC_RELAXED);
}
void runtime_menu_note(float x, float y, int left, int right, int wheel, int text) {
    store_f(&pointer_x_bits, x);
    store_f(&pointer_y_bits, y);
    __atomic_store_n(&typing, text ? 1 : 0, __ATOMIC_RELAXED);
    if (!runtime_menu_in_menu()) return;
    if (wheel) __atomic_fetch_add(&wheel_left, wheel, __ATOMIC_RELAXED);
    if (right) queue_button(back_button());
    if (!left || text) return;
    /* The title's "press any button" has no list yet: a click is confirm. */
    int64_t seen = __atomic_load_n(&focus_ms, __ATOMIC_RELAXED);
    if (front_end() && !menu_open() && (!seen || now_ms() - seen >= 400)) {
        queue_button(confirm_button());
        return;
    }
    __atomic_store_n(&click_armed, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&click_ms, now_ms(), __ATOMIC_RELAXED);
}
