/* rdr2-60fps.c -- one-shot runtime 60fps patch for Red Dead Redemption 2
 * (PS4, v1.32) on a jailbroken PS5.
 *
 * Rewrite of KurohaXR's autopatch60-oneshot.c (MIT). The patch is Jao's 1.32
 * "Unlock FPS" (eboot+0x05853029: 0f 44 f0 -> 31 f6 90), which the
 * PS-Game-Patch database lists for CUSA03041, CUSA08519 and CUSA08568.
 *
 * What this payload does, and nothing else:
 *   - talks to ps5debug-NG on 127.0.0.1:744 (the only way it touches memory),
 *   - asks which app is in front (pid, title id, version),
 *   - writes 3 bytes into the RDR2 process, only after the 61 bytes around
 *     them match the clean 1.32 eboot exactly, then reads them back,
 *   - shows system notifications and writes its own log file,
 *   - exits. No kernel patches, no other process touched, nothing resident,
 *     no files changed besides its own log. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <ps5/klog.h>

#define TOOL_VERSION "1.0"

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

#ifndef DBG_PORT
#define DBG_PORT  744
#endif
#define LOCK_PORT 47960 /* loopback UDP port held as a single-instance lock */

#define CONNECT_TRIES   60      /* x 1 s: wait for ps5debug-NG to come up */
#define POLL_USEC       500000u /* foreground poll, as in the tested original */
#define WAIT_POLLS      1200    /* x 0.5 s = 10 min */
#define RECONNECT_TRIES 5
#define RESOLVE_TRIES   10      /* x 0.3 s: game memory not ready yet */
#define RESOLVE_USEC    300000u
#define WRITE_TRIES     3
#define WRITE_USEC      200000u

/* ps5debug-NG wire protocol (see its PROTOCOL.md) */
#define PACKET_MAGIC       0xFFAABBCCu
#define WIRE_SUCCESS       0x80000000u
#define CMD_VERSION        0xBD000001u
#define CMD_FW_VERSION     0xBD000500u
#define CMD_PROC_READ      0xBDAA0002u
#define CMD_PROC_WRITE     0xBDAA0003u
#define CMD_PROC_MAPS      0xBDAA0004u
#define CMD_FG_APP         0xBDDD0006u
#define FG_APP_RESP_LEN    140
#define MAPS_ENTRY_LEN     58
#define PROT_EXEC          0x4u

enum {
    EXIT_OK = 0,
    EXIT_ALREADY_RUNNING = 1,
    EXIT_NO_DEBUGGER = 2,
    EXIT_NO_GAME = 3,
    EXIT_BAD_VERSION = 4,
    EXIT_NO_TARGET = 5,
    EXIT_UNEXPECTED = 6,
    EXIT_WRITE_FAIL = 7,
    EXIT_LOST_DEBUGGER = 8,
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

static void notify(const char *fmt, ...) {
    notify_request_t req;
    va_list ap;
    memset(&req, 0, sizeof(req));
    strcpy(req.message, "RDR2 60fps: ");
    va_start(ap, fmt);
    vsnprintf(req.message + strlen(req.message),
              sizeof(req.message) - strlen(req.message), fmt, ap);
    va_end(ap);
    logmsg("NOTE", "%s", req.message);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
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

/* -------------------------------------------------------- debugger link */

static int g_sock = -1;

static void put32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put64le(uint8_t *p, uint64_t v) {
    put32le(p, (uint32_t)v); put32le(p + 4, (uint32_t)(v >> 32));
}

static uint32_t get32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get64le(const uint8_t *p) {
    return get32le(p) | ((uint64_t)get32le(p + 4) << 32);
}

static void dbg_close(void) {
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
}

/* any protocol error leaves the stream in an unknown state: drop it so the
 * next command starts on a fresh connection */
static int dbg_fail(void) {
    dbg_close();
    return -1;
}

static int dbg_connect(void) {
    struct sockaddr_in sa;
    struct timeval tv = {10, 0};
    dbg_close();
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0)
        return -1;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(g_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(DBG_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(g_sock, (struct sockaddr *)&sa, sizeof(sa)))
        return dbg_fail();
    return 0;
}

static int dbg_ensure(void) {
    return g_sock >= 0 ? 0 : dbg_connect();
}

/* read exactly n bytes or fail */
static int recvn(uint8_t *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(g_sock, (char *)buf + got, n - got, 0);
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

static int sendn(const uint8_t *buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = send(g_sock, (const char *)buf + sent, n - sent, 0);
        if (r <= 0)
            return -1;
        sent += (size_t)r;
    }
    return 0;
}

static int cmd(uint32_t id, const uint8_t *body, uint32_t len) {
    uint8_t h[12];
    if (dbg_ensure())
        return -1;
    put32le(h, PACKET_MAGIC); put32le(h + 4, id); put32le(h + 8, len);
    if (sendn(h, 12) || (len && sendn(body, len)))
        return dbg_fail();
    return 0;
}

static int expect_success(void) {
    uint8_t b[4];
    if (recvn(b, 4) || get32le(b) != WIRE_SUCCESS)
        return dbg_fail();
    return 0;
}

/* protocol version string, e.g. "1.3" (no status word) */
static int dbg_version(char out[32]) {
    uint8_t b[4];
    uint32_t len;
    if (cmd(CMD_VERSION, NULL, 0) || recvn(b, 4))
        return dbg_fail();
    len = get32le(b);
    if (len >= 32 || recvn((uint8_t *)out, len))
        return dbg_fail();
    out[len] = 0;
    return 0;
}

/* firmware as decimal, e.g. 1360 for 13.60 (no status word) */
static int dbg_fw(unsigned *fw) {
    uint8_t b[2];
    if (cmd(CMD_FW_VERSION, NULL, 0) || recvn(b, 2))
        return dbg_fail();
    *fw = (unsigned)b[0] | ((unsigned)b[1] << 8);
    return 0;
}

struct fg_info {
    uint32_t pid;       /* 0 when no game is in front */
    char titleid[16];
    char app_ver[16];
};

static int fg_app(struct fg_info *fg) {
    uint8_t resp[FG_APP_RESP_LEN];
    if (cmd(CMD_FG_APP, NULL, 0) || expect_success() ||
        recvn(resp, sizeof(resp)))
        return dbg_fail();
    fg->pid = get32le(resp);
    memcpy(fg->titleid, resp + 4, 16);
    fg->titleid[15] = 0;
    memcpy(fg->app_ver, resp + 124, 16);
    fg->app_ver[15] = 0;
    return 0;
}

static int proc_read(uint32_t pid, uint64_t addr, uint8_t *out, uint32_t len) {
    uint8_t b[16];
    put32le(b, pid); put64le(b + 4, addr); put32le(b + 12, len);
    if (cmd(CMD_PROC_READ, b, 16) || expect_success() || recvn(out, len))
        return dbg_fail();
    return 0;
}

static int proc_write(uint32_t pid, uint64_t addr, const uint8_t *data,
                      uint32_t len) {
    uint8_t b[16];
    put32le(b, pid); put64le(b + 4, addr); put32le(b + 12, len);
    if (cmd(CMD_PROC_WRITE, b, 16) || expect_success() || sendn(data, len) ||
        expect_success())
        return dbg_fail();
    return 0;
}

/* local memmem in case the payload libc lacks it */
static const void *local_memmem(const void *h, size_t hlen, const void *n,
                                size_t nlen) {
    const uint8_t *hh = h;
    size_t i;
    if (!nlen || nlen > hlen)
        return NULL;
    for (i = 0; i + nlen <= hlen; i++)
        if (!memcmp(hh + i, n, nlen))
            return hh + i;
    return NULL;
}

/* base = start of the lowest large executable mapping (home of .text) */
static int eboot_base(uint32_t pid, uint64_t *base) {
    uint8_t b[4], e[MAPS_ENTRY_LEN];
    uint64_t best = 0;
    uint32_t num, i;
    put32le(b, pid);
    if (cmd(CMD_PROC_MAPS, b, 4) || expect_success() || recvn(b, 4))
        return dbg_fail();
    num = get32le(b);
    if (num > 100000)
        return dbg_fail(); /* garbage: stream out of sync */
    for (i = 0; i < num; i++) {
        uint64_t start, end;
        uint16_t prot;
        if (recvn(e, sizeof(e)))
            return dbg_fail();
        start = get64le(e + 32);
        end = get64le(e + 40);
        prot = (uint16_t)(e[56] | (e[57] << 8));
        if (!(prot & PROT_EXEC) || end - start < 0x1000000) /* .text is tens of MB */
            continue;
        if (!local_memmem(e, 32, "executable", 10))
            continue;
        if (!best || start < best)
            best = start;
    }
    if (!best)
        return -1;
    *base = best;
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

/* Poll the foreground app until a supported RDR2 title shows up.
 * Returns 0 (found), 1 (timed out) or -1 (lost ps5debug-NG). */
static int wait_for_game(struct fg_info *fg, int *already_open) {
    unsigned poll, fails = 0;
    uint32_t last_pid = 0;
    for (poll = 0; poll < WAIT_POLLS; poll++) {
        if (fg_app(fg)) {
            if (++fails > RECONNECT_TRIES)
                return -1;
            logmsg("WARN", "foreground query failed (%u/%u), reconnecting", fails,
                   RECONNECT_TRIES);
            sleep(1);
            continue;
        }
        fails = 0;
        if (fg->pid && is_wanted_title(fg->titleid)) {
            *already_open = (poll == 0);
            return 0;
        }
        if (fg->pid && fg->pid != last_pid) {
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
    dbg_close();
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
    unsigned fw, tries, i;
    unsigned long found_ms;
    int already_open = 0, mismatch, rc;
    char ver[32];
    char when[32] = "?";
    time_t now = time(NULL);
    struct tm tm;

    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    signal(SIGPIPE, SIG_IGN); /* a dropped connection must not kill us */

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

    /* 1) ps5debug-NG */
    for (i = 1; dbg_connect(); i++) {
        if (i == 1)
            notify("waiting for ps5debug-NG (port %d)...", DBG_PORT);
        if (i >= CONNECT_TRIES) {
            notify("ps5debug-NG not found. Load it first, then this payload. "
                   "Nothing changed.");
            return finish(EXIT_NO_DEBUGGER);
        }
        logmsg("WARN", "connect try %u/%u failed", i, CONNECT_TRIES);
        sleep(1);
    }
    logmsg("OK", "connected to ps5debug-NG (try %u)", i);
    if (!dbg_version(ver))
        logmsg("INFO", "ps5debug-NG protocol %s", ver);
    if (!dbg_fw(&fw))
        logmsg("INFO", "console firmware %u.%02u", fw / 100, fw % 100);

    /* 2) wait for the game */
    notify("ready. Start Red Dead Redemption 2 (waiting up to 10 min)");
    rc = wait_for_game(&fg, &already_open);
    if (rc < 0) {
        notify("lost connection to ps5debug-NG. Nothing changed.");
        return finish(EXIT_LOST_DEBUGGER);
    }
    if (rc > 0) {
        notify("RDR2 was not started within 10 min. Closed, nothing changed.");
        return finish(EXIT_NO_GAME);
    }
    found_ms = elapsed_ms();
    logmsg("OK", "RDR2 found: %s v%s pid %u%s", fg.titleid, fg.app_ver, fg.pid,
           already_open ? " (was already open)" : "");

    /* 3) version gate: the patch is for 1.32 only */
    if (!fg.app_ver[0]) {
        logmsg("WARN", "game version unknown, relying on the byte check");
    } else if (strncmp(fg.app_ver, WANT_APP_VER, strlen(WANT_APP_VER))) {
        notify("game version %s is not supported (needs %s). Nothing changed.",
               fg.app_ver, WANT_APP_VER);
        return finish(EXIT_BAD_VERSION);
    }
    notify("found %s v%s, patching...", fg.titleid, fg.app_ver);

    /* 4) locate and verify the patch site */
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
    if (already_open)
        notify("applied, but RDR2 was already open. If it stays at 30fps: "
               "close the game, load this payload again, then start RDR2.");
    else
        notify("60fps ON (try %u). Patch by Jao, original tool by KurohaXR",
               tries);
    return finish(EXIT_OK);
}
