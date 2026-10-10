/* rdr2-60fps.c -- one-shot runtime 60fps patch for Red Dead Redemption 2
 * (PS4, v1.32) on a jailbroken PS5.
 *
 * Rewrite of KurohaXR's autopatch60-oneshot.c (MIT). The patch is Jao's 1.32
 * "Unlock FPS" (eboot+0x05853029: 0f 44 f0 -> 31 f6 90), which the
 * PS-Game-Patch database lists for CUSA03041, CUSA08519 and CUSA08568.
 *
 * What this payload does, and nothing else:
 *   - finds the running game (eboot.bin) and reads its title id and version,
 *   - reads its memory with the SDK's mdbg_* debug calls, and writes the 3
 *     bytes either with mdbg or, on firmware that refuses that (>= 8.40),
 *     through the kernel direct map (see ptwalk.c), so no separate debugger
 *     payload is needed,
 *   - changes those 3 bytes in the RDR2 process only, and only after the 61
 *     bytes around them match the clean 1.32 eboot exactly, then reads back,
 *   - shows system notifications and writes its own log file,
 *   - exits. It does not patch the kernel, touch any other process, stay
 *     resident, or change any file besides its own log and settings file.
 *
 * It also sets the render resolution,
 * from LOG_DIR/scale.txt, which it creates with fake4k=1, 2160p=0:
 *   - fake4k=1: on a 1080p TV the game believes the output is 4K and takes
 *     its own PS4 Pro 4K checkerboard path (1920x2160 rebuilt to 3840x2160);
 *     the PS5 scales the 4K frames to its 1080p output. In the apply step
 *     (0x7865e8), before the node's screen size is copied to the video mode,
 *     a cave sets it to 3840x2160 with checkerboard on; the snap to the TV's
 *     own modes (0xa2512e) is skipped. On a 4K TV this changes nothing.
 *   - 2160p=N (N <= 10, ratios up to 1): the PC Resolution Scale on top of a
 *     4K checkerboard (e.g. 2 = 2/3, a 1440p checkerboard), set in the live
 *     settings node where the apply step copies it (0x786846).
 * Those patches have to land before the game applies its settings (about
 * 1 s after it starts), so the game must start after this payload; when it
 * doesn't, or anything doesn't match, only the 60fps patch is applied. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef HOST_TEST
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include "ptwalk.h"
#endif

#include <ps5/kernel.h>
#include <ps5/klog.h>
#include <ps5/mdbg.h>

#define TOOL_VERSION "2.0"

/* process name shown by payload managers (PLDMGR lists processes whose name
 * ends in .elf; the kernel keeps at most 19 characters) */
#define PROC_NAME "rdr260fps.elf"

/* ---------------------------------------------------------------- config */

/* Title ids the 1.32 "Unlock FPS" patch is published for (same address). */
static const char *const TITLE_IDS[] = {"CUSA08519", "CUSA03041", "CUSA08568"};
#define WANT_APP_VER "01.32"

#define PATCH_RVA        0x5453029u  /* 0x05853029 - 0x400000 */
#define IMG_BASE_DEFAULT 0x400000u   /* PS4 eboot base; fast path only */

/* 64 bytes of the clean 1.32 eboot around the target (target at +32):
 *   mov eax,[rip+..]; mov edi,[rip+..]; lea esi,[rax-1]; test eax,eax;
 *   cmove esi,eax  <- target;  call ..; test eax,eax; js .. */
static const uint8_t PATCH_CTX[64] = {
    0x89,0x04,0xe9,0x49,0xff,0xc5,0x4d,0x39,0xf5,0x0f,0x82,0xe8,0xfd,0xff,0xff,0x8b,
    0x05,0xb2,0xcf,0xd5,0x03,0x8b,0x3d,0xcc,0xd9,0xd6,0x03,0x8d,0x70,0xff,0x85,0xc0,
    0x0f,0x44,0xf0,0xe8,0x2f,0xad,0x70,0x01,0x85,0xc0,0x0f,0x88,0xac,0x10,0x00,0x00,
    0x44,0x8b,0x05,0x68,0xcf,0xd5,0x03,0x44,0x8b,0x0d,0x65,0xcf,0xd5,0x03,0x48,0x8d
};
#define CTX_OFF   32
#define PATCH_LEN 3
static const uint8_t PATCH_ORIG[PATCH_LEN] = {0x0f, 0x44, 0xf0}; /* cmove esi,eax */
static const uint8_t PATCH_NEW[PATCH_LEN]  = {0x31, 0xf6, 0x90}; /* xor esi,esi; nop */

/* ------------------------------------------------- resolution (see top) */

/* RVAs: address - 0x400000 */
#define RES_SCALE_RVA   0x386846u  /* mov eax,[r12+0x13c]; mov [index],eax */
#define RES_SCALE_LEN   14u
#define RES_RESUME_RVA  0x386854u
#define RES_FAKE_RVA    0x3865e8u  /* mov rax,[r12+0x1bc]; mov [video],rax */
#define RES_FAKE_LEN    15u
#define RES_FAKE_RESUME 0x3865f7u
#define RES_SNAP_RVA    0x62512eu  /* je: keep the snapped size */
#define RES_SNAP_LEN    2u
#define RES_FAKE_CAVE   0x754bf00u /* zero-filled tail of the code mapping */
#define RES_SCALE_CAVE  0x754bf80u
#define RES_TAIL_RVA    0x7548000u /* 0x7948000..0x794c000 must be all zero */
#define RES_TAIL_LEN    0x4000u
#define RES_CAVE_ROOM   128u
#define RES_INDEX_RVA   0x83ec408u /* the scale index the size function reads */
#define RES_VIDEO_RVA   0x91affa8u /* video mode: W, H (from the node) */
#define RES_SIZES_RVA   0x83ec40cu /* base W,H; scaled W,H; final W,H */

/* 64 bytes of the clean 1.32 eboot around each site (site at +32) */
static const uint8_t RES_SCALE_CTX[64] = {
    0xe8,0xf5,0x19,0x06,0x01,0x41,0x83,0xbc,0x24,0xa4,0x01,0x00,0x00,0x00,0x8b,0x15,
    0x4e,0x4e,0x0c,0x08,0xbe,0x01,0x00,0x00,0x00,0x0f,0x94,0x05,0x5a,0xcf,0x3a,0x09,
    0x41,0x8b,0x84,0x24,0x3c,0x01,0x00,0x00,0x89,0x05,0xb4,0x5b,0x06,0x08,0x41,0x8b,
    0x84,0x24,0xd4,0x01,0x00,0x00,0x89,0x05,0x6e,0x97,0xe2,0x08,0xb8,0x03,0x00,0x00
};
static const uint8_t RES_FAKE_CTX[64] = {
    0xab,0xe3,0x08,0xff,0x90,0x18,0x01,0x00,0x00,0x84,0xc0,0x74,0x13,0x48,0x8b,0x05,
    0x04,0xab,0xe3,0x08,0x48,0x8d,0x3d,0xfd,0xaa,0xe3,0x08,0x31,0xf6,0xff,0x50,0x48,
    0x49,0x8b,0x84,0x24,0xbc,0x01,0x00,0x00,0x48,0x89,0x05,0xb1,0x99,0xe2,0x08,0xe8,
    0xd4,0xb2,0xff,0xff,0xc5,0xfa,0x11,0x85,0xc0,0xfe,0xff,0xff,0xe8,0x27,0xb7,0xff
};
static const uint8_t RES_SNAP_CTX[64] = {
    0x98,0x4c,0x01,0x00,0x00,0x48,0x8d,0xb5,0x50,0xff,0xff,0xff,0x48,0x8d,0x84,0x08,
    0x50,0x01,0x00,0x00,0x48,0x0f,0x47,0xf8,0xe8,0x35,0x3f,0xe3,0x04,0x48,0x85,0xc0,
    0x74,0x19,0xc5,0xfa,0x6f,0x00,0xc5,0xf9,0x7e,0xc1,0xc4,0xe3,0x79,0x16,0xc0,0x01,
    0xc4,0xc1,0x7a,0x7f,0x86,0x94,0x00,0x00,0x00,0xeb,0x0e,0x41,0x8b,0x8e,0x94,0x00
};
static const uint8_t RES_SNAP_NEW[2] = {0xeb, 0x19}; /* je -> jmp */

/* the game's ratio tables (0x7220660/0x72206a0), indexed by (index - 1);
 * only ratios up to 1 (1..10) fit the 4K checkerboard buffers */
#define RES_MAX_INDEX 10
static const char *const RATIO_NAME[RES_MAX_INDEX + 1] = {
    "stock", "1/2", "2/3", "3/4", "4/5", "5/6", "7/8", "9/10", "11/12",
    "14/15", "29/30"};
static const uint32_t RATIO_NUM[RES_MAX_INDEX] = {1, 2, 3, 4, 5, 7, 9, 11, 14, 29};
static const uint32_t RATIO_DEN[RES_MAX_INDEX] = {2, 3, 4, 5, 6, 8, 10, 12, 15, 30};

/* overridable only for the host-side tests in test/ */
#ifndef LOG_DIR
#define LOG_DIR  "/data/rdr2-60fps"
#endif
#define LOG_PATH LOG_DIR "/rdr2-60fps.log"
#define CONF_PATH LOG_DIR "/scale.txt"

/* where the system keeps each installed title's param.sfo */
#ifndef APPMETA_DIR
#define APPMETA_DIR "/system_data/priv/appmeta"
#endif
#define LOCK_PORT 47960 /* loopback UDP port held as a single-instance lock */

#define POLL_USEC       20000u  /* fast: the resolution patch must beat startup */
#define WAIT_POLLS      30000   /* x 20 ms = 10 min */
#define LIST_TRIES      5       /* process list failures in a row */
#define RESOLVE_TRIES   10      /* x 0.3 s: game memory not ready yet */
#define RESOLVE_USEC    300000u
#define WRITE_TRIES     3
#define WRITE_USEC      200000u
#define RES_RESOLVE_TRIES 300   /* x 10 ms */
#define RES_RESOLVE_USEC  10000u
#define RES_WRITE_USEC    20000u
#ifndef RES_SIZES_POLLS
#define RES_SIZES_POLLS 120     /* x 0.25 s = 30 s for the game to size its render */
#endif
#define RES_SIZES_USEC  250000u

enum {
    EXIT_OK = 0,
    EXIT_ALREADY_RUNNING = 1,
    /* 2 was "ps5debug-NG not found", no longer possible */
    EXIT_NO_GAME = 3,
    EXIT_BAD_VERSION = 4,
    EXIT_NO_TARGET = 5,
    EXIT_UNEXPECTED = 6,
    EXIT_WRITE_FAIL = 7,
    EXIT_NO_PROC_LIST = 8,
};

/* --------------------------------------------------------- notifications */

typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

/* ------------------------------------------------------------------- log */

static FILE *g_log;
static struct timespec g_t0;

static unsigned long elapsed_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long)((long long)(t.tv_sec - g_t0.tv_sec) * 1000 +
                           (t.tv_nsec - g_t0.tv_nsec) / 1000000);
}

/* fresh log on every launch ("w" truncates) */
static void log_open(void) {
    mkdir(LOG_DIR, 0777);
    g_log = fopen(LOG_PATH, "w");
}

static void logmsg(const char *lvl, const char *fmt, ...) {
    char msg[256];
    unsigned long ms = elapsed_ms();
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (g_log) {
        fprintf(g_log, "[%4lu.%03lu] %-4s %s\n", ms / 1000, ms % 1000, lvl, msg);
        fflush(g_log);
        fsync(fileno(g_log));
    }
    klog_printf("[rdr2-60fps] %s %s\n", lvl, msg);
}

static void send_notification(const char *prefix, const char *fmt, va_list ap) {
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    strcpy(req.message, prefix);
    vsnprintf(req.message + strlen(req.message),
              sizeof(req.message) - strlen(req.message), fmt, ap);
    logmsg("NOTE", "%s", req.message);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/* "RDR2 60fps: ..." */
static void notify(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    send_notification("RDR2 60fps: ", fmt, ap);
    va_end(ap);
}

/* as is, no prefix */
static void notify_plain(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    send_notification("", fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------- single instance */

static int g_lock = -1;

static int lock_instance(void) {
    struct sockaddr_in sa;
    g_lock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_lock < 0)
        return 0; /* cannot check; carry on */
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(LOCK_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_lock, (struct sockaddr *)&sa, sizeof(sa))) {
        close(g_lock);
        g_lock = -1;
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------- game access */

struct fg_info {
    uint32_t pid;       /* 0 when no game is running */
    char titleid[16];
    char app_ver[16];   /* "" when param.sfo couldn't be read */
};

static int proc_read(uint32_t pid, uint64_t addr, uint8_t *out, uint32_t len) {
    return mdbg_copyout((pid_t)pid, (intptr_t)addr, out, len) ? -1 : 0;
}

static int proc_write(uint32_t pid, uint64_t addr, const uint8_t *data,
                      uint32_t len) {
#ifndef HOST_TEST
    /* firmware >= 8.40 refuses the mdbg write for game code, so the DMAP path
     * is tried first there (the same gate ps5debug-NG applies) */
    if ((kernel_get_fw_version() & 0xffff0000u) >= 0x08400000u) {
        int rc = ptwalk_write(pid, addr, data, len);
        if (rc == PTWALK_OK)
            return 0;
        logmsg("WARN", "DMAP write failed (rc %d), trying mdbg", rc);
    }
#endif
    return mdbg_copyin((pid_t)pid, data, (intptr_t)addr, len) ? -1 : 0;
}

/* load address of the game's eboot.bin, from the kernel's module list */
static int eboot_base(uint32_t pid, uint64_t *base) {
    uint32_t handle;
    intptr_t b;
    if (kernel_dynlib_handle((pid_t)pid, "eboot.bin", &handle))
        return -1;
    b = kernel_dynlib_mapbase_addr((pid_t)pid, handle);
    if (b <= 0)
        return -1;
    *base = (uint64_t)b;
    return 0;
}

static uint32_t get32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* sceKernelGetAppInfo's layout differs between firmwares (on 13.60 the title
 * id is 4 bytes earlier than in the SDK sample), so it's read as raw bytes
 * and searched. Generous size: the call writes about 0x60 bytes. */
#define APP_INFO_LEN 0x100

int sceKernelGetAppInfo(pid_t pid, void *info);

#ifndef HOST_TEST
/* the running game is the process named eboot.bin (one at a time) */
static int find_game(uint32_t *pid, uint8_t info[APP_INFO_LEN]) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    uint8_t *buf, *p;
    if (sysctl(mib, 4, NULL, &size, NULL, 0) || !(buf = malloc(size)))
        return -1;
    if (sysctl(mib, 4, buf, &size, NULL, 0)) {
        free(buf);
        return -1;
    }
    *pid = 0;
    for (p = buf; p < buf + size;) {
        struct kinfo_proc *ki = (struct kinfo_proc *)p;
        if (ki->ki_structsize <= 0)
            break;
        p += ki->ki_structsize;
        if (strncmp(ki->ki_comm, "eboot.bin", 9) || ki->ki_pid <= 0)
            continue;
        memset(info, 0, APP_INFO_LEN);
        if (sceKernelGetAppInfo(ki->ki_pid, info))
            continue;
        *pid = (uint32_t)ki->ki_pid;
        break;
    }
    free(buf);
    return 0;
}
#else
int find_game(uint32_t *pid, uint8_t info[APP_INFO_LEN]); /* test/stub */
#endif

/* first "ABCD12345" (4 capitals + 5 digits) in the app info, or -1 */
static int find_titleid(const uint8_t *info, size_t len, char out[16]) {
    size_t i, k;
    for (i = 0; i + 9 <= len; i++) {
        for (k = 0; k < 9; k++) {
            uint8_t c = info[i + k];
            if (k < 4 ? (c < 'A' || c > 'Z') : (c < '0' || c > '9'))
                break;
        }
        if (k == 9 && (i + 9 == len || info[i + 9] < '0' || info[i + 9] > '9')) {
            memcpy(out, info + i, 9);
            out[9] = 0;
            return 0;
        }
    }
    return -1;
}

/* string value of key in a param.sfo image, or -1 */
static int sfo_get(const uint8_t *sfo, size_t len, const char *key, char *out,
                   size_t outlen) {
    uint32_t keys, data, n, i;
    if (len < 20 || memcmp(sfo, "\0PSF", 4))
        return -1;
    keys = get32le(sfo + 8);
    data = get32le(sfo + 12);
    n = get32le(sfo + 16);
    if (keys > len || data > len || n > (len - 20) / 16)
        return -1;
    for (i = 0; i < n; i++) {
        const uint8_t *e = sfo + 20 + i * 16;
        uint32_t koff = keys + (uint32_t)(e[0] | (e[1] << 8));
        uint32_t dlen = get32le(e + 4), doff = data + get32le(e + 12);
        if (koff >= len || strnlen((const char *)sfo + koff, len - koff) ==
                               len - koff)
            return -1;
        if (strcmp((const char *)sfo + koff, key))
            continue;
        if (doff > len || dlen > len - doff || !dlen || dlen > outlen)
            return -1;
        memcpy(out, sfo + doff, dlen);
        out[dlen - 1] = 0;
        return 0;
    }
    return -1;
}

/* installed version: the higher of APP_VER and VERSION; "" if unknown */
static void read_app_ver(const char *titleid, char out[16]) {
    static uint8_t sfo[16384];
    char path[128], a[16] = "", v[16] = "";
    size_t len;
    FILE *f;
    out[0] = 0;
    snprintf(path, sizeof(path), APPMETA_DIR "/%s/param.sfo", titleid);
    if (!(f = fopen(path, "rb"))) {
        logmsg("WARN", "can't open %s", path);
        return;
    }
    len = fread(sfo, 1, sizeof(sfo), f);
    fclose(f);
    sfo_get(sfo, len, "APP_VER", a, sizeof(a));
    sfo_get(sfo, len, "VERSION", v, sizeof(v));
    strcpy(out, strcmp(a, v) >= 0 ? a : v);
}

static int fg_app(struct fg_info *fg) {
    uint8_t info[APP_INFO_LEN];
    memset(fg, 0, sizeof(*fg));
    if (find_game(&fg->pid, info))
        return -1;
    if (fg->pid && find_titleid(info, sizeof(info), fg->titleid))
        strcpy(fg->titleid, "?");
    return 0;
}

/* ------------------------------------------------------------ the patch */

static int is_wanted_title(const char *titleid) {
    size_t i;
    for (i = 0; i < sizeof(TITLE_IDS) / sizeof(TITLE_IDS[0]); i++)
        if (!strcmp(titleid, TITLE_IDS[i]))
            return 1;
    return 0;
}

/* context check that skips the 3 target bytes, so an already patched game
 * is still recognised (the original compared all 64 and missed that case) */
static int ctx_matches(const uint8_t ctx[64]) {
    return !memcmp(ctx, PATCH_CTX, CTX_OFF) &&
           !memcmp(ctx + CTX_OFF + PATCH_LEN, PATCH_CTX + CTX_OFF + PATCH_LEN,
                   64 - CTX_OFF - PATCH_LEN);
}

enum site { SITE_OK, SITE_UNREADABLE, SITE_MISMATCH };

static enum site check_site(uint32_t pid, uint64_t target, uint8_t ctx[64]) {
    if (proc_read(pid, target - CTX_OFF, ctx, 64))
        return SITE_UNREADABLE;
    return ctx_matches(ctx) ? SITE_OK : SITE_MISMATCH;
}

/* Find the patch site. Fast path: eboot at 0x400000; fallback: base from the
 * memory map. Retries while the game is still being loaded. Returns 0, or
 * -1 with *mismatch set if the bytes were readable but different. */
static int resolve_target(uint32_t pid, uint64_t *target, uint8_t ctx[64],
                          unsigned *tries, int *mismatch) {
    uint64_t base;
    enum site s;
    *mismatch = 0;
    for (*tries = 1; *tries <= RESOLVE_TRIES; (*tries)++) {
        *target = IMG_BASE_DEFAULT + PATCH_RVA;
        s = check_site(pid, *target, ctx);
        if (s == SITE_OK) {
            logmsg("OK", "patch site verified at 0x%llx (default base, try %u)",
                   (unsigned long long)*target, *tries);
            return 0;
        }
        if (s == SITE_MISMATCH)
            *mismatch = 1;
        if (eboot_base(pid, &base)) {
            logmsg("WARN", "try %u/%u: memory map not readable yet", *tries,
                   RESOLVE_TRIES);
        } else if (base != IMG_BASE_DEFAULT) {
            *target = base + PATCH_RVA;
            s = check_site(pid, *target, ctx);
            if (s == SITE_OK) {
                logmsg("OK", "patch site verified at 0x%llx (base 0x%llx, try %u)",
                       (unsigned long long)*target, (unsigned long long)base,
                       *tries);
                return 0;
            }
            if (s == SITE_MISMATCH)
                *mismatch = 1;
            logmsg("WARN", "try %u/%u: base 0x%llx, bytes %s", *tries,
                   RESOLVE_TRIES, (unsigned long long)base,
                   s == SITE_MISMATCH ? "do not match" : "not readable");
        } else {
            logmsg("WARN", "try %u/%u: base 0x400000, bytes %s", *tries,
                   RESOLVE_TRIES,
                   s == SITE_MISMATCH ? "do not match" : "not readable");
        }
        usleep(RESOLVE_USEC);
    }
    *tries = RESOLVE_TRIES;
    return -1;
}

/* verify -> write -> read back; never writes unless the bytes are original */
static int apply_patch(uint32_t pid, uint64_t target, unsigned *tries) {
    uint8_t cur[PATCH_LEN];
    for (*tries = 1; *tries <= WRITE_TRIES; (*tries)++) {
        if (proc_read(pid, target, cur, PATCH_LEN)) {
            logmsg("WARN", "write try %u/%u: re-read failed", *tries, WRITE_TRIES);
        } else if (!memcmp(cur, PATCH_NEW, PATCH_LEN)) {
            logmsg("OK", "bytes read back as 31 f6 90");
            return 0;
        } else if (memcmp(cur, PATCH_ORIG, PATCH_LEN)) {
            logmsg("FAIL", "bytes changed to %02x %02x %02x, not writing",
                   cur[0], cur[1], cur[2]);
            return -2;
        } else if (proc_write(pid, target, PATCH_NEW, PATCH_LEN)) {
            logmsg("WARN", "write try %u/%u: write rejected", *tries, WRITE_TRIES);
        } else if (proc_read(pid, target, cur, PATCH_LEN)) {
            logmsg("WARN", "write try %u/%u: read-back failed", *tries, WRITE_TRIES);
        } else if (!memcmp(cur, PATCH_NEW, PATCH_LEN)) {
            logmsg("OK", "wrote 31 f6 90 and read it back (try %u)", *tries);
            return 0;
        } else {
            logmsg("WARN", "write try %u/%u: read back %02x %02x %02x", *tries,
                   WRITE_TRIES, cur[0], cur[1], cur[2]);
        }
        usleep(WRITE_USEC);
    }
    *tries = WRITE_TRIES;
    return -1;
}

/* Poll the running game until a supported RDR2 title shows up.
 * Returns 0 (found), 1 (timed out) or -1 (process list unavailable). */
static int wait_for_game(struct fg_info *fg, int *already_open) {
    unsigned poll, fails = 0;
    uint32_t last_pid = 0;
    for (poll = 0; poll < WAIT_POLLS; poll++) {
        if (fg_app(fg)) {
            if (++fails > LIST_TRIES)
                return -1;
            logmsg("WARN", "process list failed (%u/%u)", fails, LIST_TRIES);
            sleep(1);
            continue;
        }
        fails = 0;
        if (fg->pid && is_wanted_title(fg->titleid)) {
            *already_open = (poll == 0);
            read_app_ver(fg->titleid, fg->app_ver);
            return 0;
        }
        if (fg->pid && fg->pid != last_pid) {
            read_app_ver(fg->titleid, fg->app_ver);
            logmsg("INFO", "other app in front: %s v%s (pid %u)", fg->titleid,
                   fg->app_ver, fg->pid);
            notify("%s is not RDR2, still waiting", fg->titleid);
        }
        last_pid = fg->pid;
        if (poll && poll % 1500 == 0)
            logmsg("INFO", "still waiting for RDR2 (%u s)", poll / 50);
        usleep(POLL_USEC);
    }
    return 1;
}

/* ------------------------------------------------------------ resolution */

static int g_fake4k = 1;     /* game believes the TV is 4K (1080p TVs) */
static int g_scale_2160 = 0; /* PC Resolution Scale index for 4K */

/* Reads "fake4k=" and "2160p=" lines; writes the defaults if the file
 * doesn't exist yet, so there's something to edit over FTP. */
static void load_settings(void) {
    char line[128];
    FILE *f = fopen(CONF_PATH, "r");
    if (!f) {
        f = fopen(CONF_PATH, "w");
        if (f) {
            fprintf(f, "# RDR2 resolution. fake4k=1: on a 1080p TV, use the game's own\n"
                       "# 4K checkerboard (1920x2160 -> 3840x2160, shown at 1080p).\n"
                       "# 2160p: 0 = stock, or a lighter render for more fps headroom:\n"
                       "#  3 = 3/4 (2880x1620), 2 = 2/3 (2560x1440), 1 = 1/2 (1920x1080)\n"
                       "fake4k=1\n2160p=0\n");
            fclose(f);
            logmsg("INFO", "wrote default settings to %s", CONF_PATH);
        }
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        int v;
        if (line[0] == '#' || !eq)
            continue;
        v = atoi(eq + 1);
        if (!strncmp(line, "fake4k", 6)) {
            g_fake4k = v != 0;
        } else if (!strncmp(line, "2160p", 5)) {
            if (v < 0 || v > RES_MAX_INDEX)
                logmsg("WARN", "ignoring 2160p=%d (must be 0..%d: ratios above 1 "
                       "corrupt the image)", v, RES_MAX_INDEX);
            else
                g_scale_2160 = v;
        }
    }
    fclose(f);
}

static void put32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int all_zero(const uint8_t *p, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++)
        if (p[i])
            return 0;
    return 1;
}

static void hexstr(const uint8_t *p, uint32_t n, char *out, size_t outlen) {
    uint32_t i;
    out[0] = 0;
    for (i = 0; i < n && (i + 1) * 3 < outlen; i++)
        snprintf(out + i * 3, outlen - i * 3, "%02x%s", p[i], i + 1 < n ? " " : "");
}

/* mov dword [r12 + disp32], imm32 */
static uint32_t emit_r12_set(uint8_t *out, uint32_t n, uint32_t disp, uint32_t v) {
    out[n++] = 0x41;
    out[n++] = 0xc7;
    out[n++] = 0x84;
    out[n++] = 0x24;
    put32le(out + n, disp);
    put32le(out + n + 4, v);
    return n + 8;
}

/* The fake-4K cave, from the apply step's copy of the node's screen size
 * (r12 = the live settings node):
 *   cmp dword [r12+0x1c0], 1080; jne copy
 *   mov dword [r12+0x1bc], 3840; mov dword [r12+0x1c0], 2160
 *   mov byte [r12+0xe9], 1                         ; checkerboard on
 *   copy: mov rax, [r12+0x1bc]; mov [rip -> video mode], rax   ; the original
 *   jmp resume
 * Returns its length (67). */
static uint32_t build_fake_cave(uint64_t base, uint8_t out[RES_CAVE_ROOM]) {
    static const uint8_t cmp[8] = {0x41, 0x81, 0xbc, 0x24, 0xc0, 0x01, 0x00, 0x00};
    static const uint8_t cb[9] = {0x41, 0xc6, 0x84, 0x24, 0xe9, 0x00, 0x00, 0x00, 0x01};
    static const uint8_t ld[8] = {0x49, 0x8b, 0x84, 0x24, 0xbc, 0x01, 0x00, 0x00};
    uint64_t cave = base + RES_FAKE_CAVE;
    uint32_t n, jne_at;
    memcpy(out, cmp, sizeof(cmp)); /* cmp dword [r12 + 0x1c0], 1080 */
    put32le(out + 8, 1080);
    n = 12;
    out[n++] = 0x75; /* jne copy */
    jne_at = n++;
    n = emit_r12_set(out, n, 0x1bc, 3840);
    n = emit_r12_set(out, n, 0x1c0, 2160);
    memcpy(out + n, cb, sizeof(cb));
    n += sizeof(cb);
    out[jne_at] = (uint8_t)(n - (jne_at + 1));
    memcpy(out + n, ld, sizeof(ld)); /* mov rax, [r12 + 0x1bc] */
    n += 8;
    out[n++] = 0x48; /* mov [rip + d], rax */
    out[n++] = 0x89;
    out[n++] = 0x05;
    put32le(out + n, (uint32_t)(int32_t)((base + RES_VIDEO_RVA) - (cave + n + 4)));
    n += 4;
    out[n++] = 0xe9; /* jmp resume */
    put32le(out + n, (uint32_t)(int32_t)((base + RES_FAKE_RESUME) - (cave + n + 4)));
    n += 4;
    return n;
}

/* The scale cave, from the apply step's copy of scalingMode (r12 = the node,
 * whose screen height is 2160 by then on a 4K or faked-4K TV):
 *   cmp dword [r12+0x1c0], 2159; jbe done
 *   mov dword [r12+0x13c], <2160p setting>
 *   done: mov eax, [r12+0x13c]; mov [rip -> index], eax   ; the original
 *   jmp resume
 * Returns its length (43). */
static uint32_t build_scale_cave(uint64_t base, uint8_t out[RES_CAVE_ROOM]) {
    static const uint8_t cmp[8] = {0x41, 0x81, 0xbc, 0x24, 0xc0, 0x01, 0x00, 0x00};
    static const uint8_t ld[8] = {0x41, 0x8b, 0x84, 0x24, 0x3c, 0x01, 0x00, 0x00};
    uint64_t cave = base + RES_SCALE_CAVE;
    uint32_t n, jbe_at;
    memcpy(out, cmp, sizeof(cmp)); /* cmp dword [r12 + 0x1c0], 2159 */
    put32le(out + 8, 2159);
    n = 12;
    out[n++] = 0x76; /* jbe done */
    jbe_at = n++;
    n = emit_r12_set(out, n, 0x13c, (uint32_t)g_scale_2160);
    out[jbe_at] = (uint8_t)(n - (jbe_at + 1));
    memcpy(out + n, ld, sizeof(ld)); /* mov eax, [r12 + 0x13c] */
    n += 8;
    out[n++] = 0x89; /* mov [rip + d], eax */
    out[n++] = 0x05;
    put32le(out + n, (uint32_t)(int32_t)((base + RES_INDEX_RVA) - (cave + n + 4)));
    n += 4;
    out[n++] = 0xe9; /* jmp resume */
    put32le(out + n, (uint32_t)(int32_t)((base + RES_RESUME_RVA) - (cave + n + 4)));
    n += 4;
    return n;
}

/* jmp <cave>; nop x (len - 5) */
static void build_jmp(uint64_t site, uint64_t cave, uint8_t *out, uint32_t len) {
    out[0] = 0xe9;
    put32le(out + 1, (uint32_t)(int32_t)(cave - (site + 5)));
    memset(out + 5, 0x90, len - 5);
}

/* One place the resolution patch changes: 64 bytes of clean code around it
 * (site at +32) and what goes there when it's on. */
struct res_site {
    const char *name;
    uint32_t rva, len;
    const uint8_t *ctx;
    int on;
    uint8_t want[16];
    uint8_t cur[64];
};

enum { RS_SNAP, RS_FAKE, RS_SCALE, RS_COUNT }; /* also the write order */

static void res_sites(struct res_site s[RS_COUNT], uint64_t base) {
    memset(s, 0, sizeof(struct res_site) * RS_COUNT);
    s[RS_SNAP] = (struct res_site){"mode snap", RES_SNAP_RVA, RES_SNAP_LEN,
                                   RES_SNAP_CTX, g_fake4k, {0}, {0}};
    memcpy(s[RS_SNAP].want, RES_SNAP_NEW, RES_SNAP_LEN);
    s[RS_FAKE] = (struct res_site){"fake 4K", RES_FAKE_RVA, RES_FAKE_LEN,
                                   RES_FAKE_CTX, g_fake4k, {0}, {0}};
    build_jmp(base + RES_FAKE_RVA, base + RES_FAKE_CAVE, s[RS_FAKE].want,
              RES_FAKE_LEN);
    s[RS_SCALE] = (struct res_site){"scale", RES_SCALE_RVA, RES_SCALE_LEN,
                                    RES_SCALE_CTX, g_scale_2160 != 0, {0}, {0}};
    build_jmp(base + RES_SCALE_RVA, base + RES_SCALE_CAVE, s[RS_SCALE].want,
              RES_SCALE_LEN);
}

/* the 64 bytes match the clean code, ignoring the site */
static int res_ctx_ok(const struct res_site *s) {
    return !memcmp(s->cur, s->ctx, CTX_OFF) &&
           !memcmp(s->cur + CTX_OFF + s->len, s->ctx + CTX_OFF + s->len,
                   64 - CTX_OFF - s->len);
}

/* write -> read back, a few tries */
static int res_write(uint32_t pid, uint64_t addr, const uint8_t *data,
                     uint32_t len, const char *what) {
    uint8_t cur[RES_CAVE_ROOM];
    unsigned t;
    for (t = 1; t <= WRITE_TRIES; t++) {
        if (proc_write(pid, addr, data, len))
            logmsg("WARN", "%s: write try %u/%u rejected", what, t, WRITE_TRIES);
        else if (proc_read(pid, addr, cur, len))
            logmsg("WARN", "%s: read-back try %u/%u failed", what, t, WRITE_TRIES);
        else if (!memcmp(cur, data, len))
            return 0;
        else
            logmsg("WARN", "%s: read back different bytes (try %u)", what, t);
        usleep(RES_WRITE_USEC);
    }
    return -1;
}

static int read_sizes(uint32_t pid, uint64_t base, uint32_t sz[6]) {
    uint8_t b[24];
    unsigned i;
    if (proc_read(pid, base + RES_SIZES_RVA, b, sizeof(b)))
        return -1;
    for (i = 0; i < 6; i++)
        sz[i] = get32le(b + i * 4);
    return 0;
}

/* the eboot's load address, recognised by the scale site's context; retries
 * briefly while the game is being mapped */
static int res_base(uint32_t pid, uint64_t *base) {
    uint8_t cur[64];
    uint64_t b;
    unsigned t;
    for (t = 0; t < RES_RESOLVE_TRIES; t++) {
        b = IMG_BASE_DEFAULT;
        if (!proc_read(pid, b + RES_SCALE_RVA - CTX_OFF, cur, 64) &&
            !memcmp(cur, RES_SCALE_CTX, CTX_OFF)) {
            *base = b;
            return 0;
        }
        if (!eboot_base(pid, &b) && b != IMG_BASE_DEFAULT &&
            !proc_read(pid, b + RES_SCALE_RVA - CTX_OFF, cur, 64) &&
            !memcmp(cur, RES_SCALE_CTX, CTX_OFF)) {
            *base = b;
            return 0;
        }
        usleep(RES_RESOLVE_USEC);
    }
    return -1;
}

enum res { RES_OFF, RES_PATCHED, RES_ALREADY, RES_SKIPPED };
static enum res g_res = RES_OFF;

/* Patches the resolution, or explains in a notification why it didn't.
 * Never touches the game unless every site and the caves check out. */
static enum res res_patch(uint32_t pid, uint64_t *base_out) {
    static uint8_t tail[RES_TAIL_LEN];
    struct res_site st[RS_COUNT];
    uint8_t fcave[RES_CAVE_ROOM], scave[RES_CAVE_ROOM];
    char hex[RES_CAVE_ROOM * 3 + 1];
    uint32_t flen = 0, slen = 0, sz[6], video;
    uint64_t base;
    unsigned i;
    int all_ours = 1, all_clean = 1;

    if (res_base(pid, &base)) {
        notify("resolution: game code not found, 60fps only.");
        return RES_SKIPPED;
    }
    *base_out = base;
    res_sites(st, base);
    if (st[RS_FAKE].on)
        flen = build_fake_cave(base, fcave);
    if (st[RS_SCALE].on)
        slen = build_scale_cave(base, scave);
    if (proc_read(pid, base + RES_TAIL_RVA, tail, RES_TAIL_LEN)) {
        notify("resolution: couldn't read the game code, 60fps only.");
        return RES_SKIPPED;
    }
    for (i = 0; i < RS_COUNT; i++) {
        struct res_site *s = &st[i];
        int clean, ours;
        if (proc_read(pid, base + s->rva - CTX_OFF, s->cur, 64) || !res_ctx_ok(s)) {
            notify("resolution: game code doesn't match v1.32 (%s), 60fps only.",
                   s->name);
            return RES_SKIPPED;
        }
        clean = !memcmp(s->cur + CTX_OFF, s->ctx + CTX_OFF, s->len);
        ours = !memcmp(s->cur + CTX_OFF, s->want, s->len);
        if (!clean && !ours) {
            hexstr(s->cur + CTX_OFF, s->len, hex, sizeof(hex));
            logmsg("WARN", "%s site: %s", s->name, hex);
            notify("resolution: unexpected code (%s), 60fps only.", s->name);
            return RES_SKIPPED;
        }
        all_clean &= clean;
        if (s->on ? !ours : !clean)
            all_ours = 0;
    }
    if (flen && memcmp(tail + (RES_FAKE_CAVE - RES_TAIL_RVA), fcave, flen))
        all_ours = 0;
    if (slen && memcmp(tail + (RES_SCALE_CAVE - RES_TAIL_RVA), scave, slen))
        all_ours = 0;
    if (!all_clean) {
        if (all_ours)
            return RES_ALREADY;
        notify("resolution: patched with other settings. Restart RDR2 for them; "
               "60fps only.");
        return RES_SKIPPED;
    }
    if (!all_zero(tail, RES_TAIL_LEN)) {
        notify("resolution: the end of the game code isn't empty, 60fps only.");
        return RES_SKIPPED;
    }
    /* too late? the apply step writes the video mode, the size function the
     * sizes; after either, the game has its resolution */
    if (read_sizes(pid, base, sz) || proc_read(pid, base + RES_VIDEO_RVA,
                                               (uint8_t *)&video, 4) ||
        !all_zero((const uint8_t *)sz, sizeof(sz)) || video) {
        notify("resolution: RDR2 already set it. Load this before starting "
               "RDR2 for 4K; 60fps only.");
        return RES_SKIPPED;
    }

    /* caves first, then the snap, then the jumps that reach the caves */
    if (flen) {
        hexstr(fcave, flen, hex, sizeof(hex));
        logmsg("INFO", "fake 4K cave %u bytes at 0x%llx: %s", flen,
               (unsigned long long)(base + RES_FAKE_CAVE), hex);
        if (res_write(pid, base + RES_FAKE_CAVE, fcave, flen, "fake 4K cave")) {
            notify("resolution: write failed, 60fps only.");
            return RES_SKIPPED;
        }
    }
    if (slen) {
        hexstr(scave, slen, hex, sizeof(hex));
        logmsg("INFO", "scale cave %u bytes at 0x%llx: %s", slen,
               (unsigned long long)(base + RES_SCALE_CAVE), hex);
        if (res_write(pid, base + RES_SCALE_CAVE, scave, slen, "scale cave")) {
            notify("resolution: write failed, 60fps only.");
            return RES_SKIPPED;
        }
    }
    for (i = 0; i < RS_COUNT; i++) {
        struct res_site *s = &st[i];
        if (!s->on)
            continue;
        if (res_write(pid, base + s->rva, s->want, s->len, s->name)) {
            notify("resolution: write failed (%s). Restart RDR2 before playing.",
                   s->name);
            return RES_SKIPPED;
        }
        hexstr(s->want, s->len, hex, sizeof(hex));
        logmsg("OK", "%s at 0x%llx: %s", s->name,
               (unsigned long long)(base + s->rva), hex);
    }
    return RES_PATCHED;
}

/* Waits for the game to size its render and describes it in `line` (for
 * the final notification); "" if it couldn't be seen or didn't match. */
static void res_report(uint32_t pid, uint64_t base, char *line, size_t len) {
    uint32_t sz[6] = {0}, idx, ew, eh;
    unsigned i;
    line[0] = 0;
    for (i = 0; i < RES_SIZES_POLLS; i++) {
        if (!read_sizes(pid, base, sz) && sz[5])
            break;
        usleep(RES_SIZES_USEC);
    }
    if (i == RES_SIZES_POLLS) {
        logmsg("WARN", "the game didn't size its render within %u s",
               RES_SIZES_POLLS * (RES_SIZES_USEC / 1000u) / 1000u);
        return;
    }
    usleep(RES_SIZES_USEC); /* the checkerboard width doubling comes last */
    read_sizes(pid, base, sz);
    logmsg("OK", "base %ux%u, scaled %ux%u, final %ux%u", sz[0], sz[1], sz[2],
           sz[3], sz[4], sz[5]);
    idx = sz[1] > 2159 ? (uint32_t)g_scale_2160 : 0;
    ew = idx ? sz[0] * RATIO_NUM[idx - 1] / RATIO_DEN[idx - 1] : sz[0];
    eh = idx ? sz[1] * RATIO_NUM[idx - 1] / RATIO_DEN[idx - 1] : sz[1];
    if (sz[2] != ew || sz[3] != eh || (g_fake4k && sz[1] == 1080)) {
        logmsg("WARN", "expected %s on a 4K or faked-4K TV", RATIO_NAME[idx]);
        notify("resolution didn't take effect (rendering %ux%u).", sz[2], sz[3]);
        return;
    }
    snprintf(line, len, "Resolution: %ux%u%s, %ux%u frame", sz[2], sz[3],
             sz[4] != sz[0] ? " checkerboard" : "", sz[4], sz[5]);
}

static int finish(int code) {
    if (code != EXIT_OK && g_res == RES_PATCHED)
        notify("the resolution patch is on; restart RDR2 to undo it.");
    logmsg("INFO", "exit code %d", code);
    if (g_lock >= 0)
        close(g_lock);
    if (g_log)
        fclose(g_log);
    return code;
}

int main(void) {
    struct fg_info fg;
    uint8_t ctx[64];
    uint64_t target, res_base_addr = 0;
    enum res res = RES_OFF;
    char res_line[96] = "";
    unsigned tries;
    unsigned long found_ms;
    int already_open = 0, mismatch, rc;
    char when[32] = "?";
    time_t now = time(NULL);
    struct tm tm;

    clock_gettime(CLOCK_MONOTONIC, &g_t0);
#ifndef HOST_TEST
    syscall(SYS_thr_set_name, -1, PROC_NAME); /* else it's "payload.elf" */
#endif

    /* lock before opening the log, so a second copy can't truncate it */
    if (lock_instance()) {
        notify("already running, this copy exits");
        return EXIT_ALREADY_RUNNING;
    }
    log_open();
    if (localtime_r(&now, &tm))
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    logmsg("INFO", "rdr2-60fps v%s started %s", TOOL_VERSION, when);
    logmsg("INFO", "log: %s", g_log ? LOG_PATH : "(could not open)");

    logmsg("INFO", "console firmware %x.%02x", kernel_get_fw_version() >> 24,
           (kernel_get_fw_version() >> 16) & 0xff);
    load_settings();
    logmsg("INFO", "resolution settings: fake 4K %s, 4K scale %s (%d)",
           g_fake4k ? "on" : "off", RATIO_NAME[g_scale_2160], g_scale_2160);

    /* 1) wait for the game */
    notify("ready. Start Red Dead Redemption 2 (waiting up to 10 min)");
    rc = wait_for_game(&fg, &already_open);
    if (rc < 0) {
        notify("couldn't list running apps. Nothing changed.");
        return finish(EXIT_NO_PROC_LIST);
    }
    if (rc > 0) {
        notify("RDR2 was not started within 10 min. Closed, nothing changed.");
        return finish(EXIT_NO_GAME);
    }
    found_ms = elapsed_ms();
    logmsg("OK", "RDR2 found: %s v%s pid %u%s", fg.titleid, fg.app_ver, fg.pid,
           already_open ? " (was already open)" : "");

    /* 2) version gate: the patch is for 1.32 only */
    if (!fg.app_ver[0]) {
        logmsg("WARN", "game version unknown, relying on the byte check");
    } else if (strncmp(fg.app_ver, WANT_APP_VER, strlen(WANT_APP_VER))) {
        notify("game version %s is not supported (needs %s). Nothing changed.",
               fg.app_ver, WANT_APP_VER);
        return finish(EXIT_BAD_VERSION);
    }
    notify("found %s v%s, patching...", fg.titleid, fg.app_ver);

    /* 3) resolution first: it has to land before the game applies its
     * settings; whatever happens here, the 60fps patch still follows */
    if (g_fake4k || g_scale_2160) {
        if (already_open)
            notify("resolution: RDR2 was already open, it's set at startup. "
                   "Load this before starting RDR2 for 4K; 60fps only.");
        else
            g_res = res = res_patch(fg.pid, &res_base_addr);
        if (res == RES_PATCHED)
            logmsg("OK", "resolution patched %lu ms after the game was found",
                   elapsed_ms() - found_ms);
    }

    /* 4) locate and verify the 60fps patch site */
    if (resolve_target(fg.pid, &target, ctx, &tries, &mismatch)) {
        if (mismatch)
            notify("game code doesn't match the 1.32 patch. Nothing changed.");
        else
            notify("couldn't read game memory after %u tries. Nothing changed.",
                   tries);
        return finish(EXIT_NO_TARGET);
    }
    if (!memcmp(ctx + CTX_OFF, PATCH_NEW, PATCH_LEN)) {
        notify("60fps already ON for this game session");
        return finish(EXIT_OK);
    }
    if (memcmp(ctx + CTX_OFF, PATCH_ORIG, PATCH_LEN)) {
        notify("unexpected bytes %02x %02x %02x. Nothing changed.",
               ctx[CTX_OFF], ctx[CTX_OFF + 1], ctx[CTX_OFF + 2]);
        return finish(EXIT_UNEXPECTED);
    }

    /* 5) write + read back */
    rc = apply_patch(fg.pid, target, &tries);
    if (rc == -2) {
        notify("bytes changed under us. Nothing changed.");
        return finish(EXIT_UNEXPECTED);
    }
    if (rc) {
        notify("write failed after %u tries. Game left as it was.", tries);
        return finish(EXIT_WRITE_FAIL);
    }
    logmsg("OK", "patched %lu ms after the game was found",
           elapsed_ms() - found_ms);

    /* 6) what resolution the game picked */
    if (res == RES_PATCHED || res == RES_ALREADY)
        res_report(fg.pid, res_base_addr, res_line, sizeof(res_line));
    if (already_open)
        notify("applied, but RDR2 was already open. If it stays at 30fps: "
               "close the game, load this payload again, then start RDR2.");
    else
        notify_plain("60FPS IS ON, Enjoy.\n%s%sMade by Arman with \u2764\ufe0f",
                     res_line, res_line[0] ? "\n" : "");
    return finish(EXIT_OK);
}
