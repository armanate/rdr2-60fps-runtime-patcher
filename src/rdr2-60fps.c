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
 *     resident, or change any file besides its own log. */
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

#define TOOL_VERSION "1.0"

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

/* overridable only for the host-side tests in test/ */
#ifndef LOG_DIR
#define LOG_DIR  "/data/rdr2-60fps"
#endif
#define LOG_PATH LOG_DIR "/rdr2-60fps.log"

/* where the system keeps each installed title's param.sfo */
#ifndef APPMETA_DIR
#define APPMETA_DIR "/system_data/priv/appmeta"
#endif
#define LOCK_PORT 47960 /* loopback UDP port held as a single-instance lock */

#define POLL_USEC       500000u /* foreground poll, as in the tested original */
#define WAIT_POLLS      1200    /* x 0.5 s = 10 min */
#define LIST_TRIES      5       /* process list failures in a row */
#define RESOLVE_TRIES   10      /* x 0.3 s: game memory not ready yet */
#define RESOLVE_USEC    300000u
#define WRITE_TRIES     3
#define WRITE_USEC      200000u

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
        if (poll && poll % 60 == 0)
            logmsg("INFO", "still waiting for RDR2 (%u s)", poll / 2);
        usleep(POLL_USEC);
    }
    return 1;
}

static int finish(int code) {
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
    uint64_t target;
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

    /* 3) locate and verify the patch site */
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

    /* 4) write + read back */
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
    if (already_open)
        notify("applied, but RDR2 was already open. If it stays at 30fps: "
               "close the game, load this payload again, then start RDR2.");
    else
        notify_plain("60FPS IS ON, Enjoy.\nMade by Arman with \u2764\ufe0f");
    return finish(EXIT_OK);
}
