/* host-test stand-ins for the PS5 calls the payload uses to find and patch
 * the game. Each call is forwarded to the fake console in run_tests.py over
 * 127.0.0.1:FAKE_PORT as one request on a fresh connection:
 *   'F'                       -> u32 pid, u8 app_info[256] (raw bytes, as
 *                                sceKernelGetAppInfo fills them)
 *   'R' u32 pid u64 addr u32 n -> u32 status, then n bytes if status == 0
 *   'W' u32 pid u64 addr u32 n, n bytes -> u32 status
 *   'B' u32 pid               -> u64 eboot base (0 = unknown)
 *   'H' u32 pid u64 0 u32 n, name -> u32 status, u32 handle
 *   'S' u32 pid u64 handle u32 n, name -> u64 address (0 = not found)
 *   'M' u32 pid               -> u32 count (0xffffffff = map refused), then
 *                                count x (u64 start, u64 end, u32 prot) */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/mdbg.h>

static int xfer(const void *req, size_t reqlen, const void *extra,
                size_t extralen, void *resp, size_t resplen, int *fd_out) {
    struct sockaddr_in sa;
    size_t got = 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(FAKE_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) ||
        send(fd, req, reqlen, 0) != (ssize_t)reqlen ||
        (extralen && send(fd, extra, extralen, 0) != (ssize_t)extralen))
        goto fail;
    while (got < resplen) {
        ssize_t r = recv(fd, (char *)resp + got, resplen - got, 0);
        if (r <= 0)
            goto fail;
        got += (size_t)r;
    }
    if (fd_out) {
        *fd_out = fd;
        return 0;
    }
    close(fd);
    return 0;
fail:
    close(fd);
    return -1;
}

static void pack(uint8_t *b, char op, uint32_t pid, uint64_t addr, uint32_t n) {
    b[0] = (uint8_t)op;
    memcpy(b + 1, &pid, 4);
    memcpy(b + 5, &addr, 8);
    memcpy(b + 13, &n, 4);
}

int find_game(uint32_t *pid, uint8_t info[0x100]) {
    uint8_t r[4 + 0x100];
    if (xfer("F", 1, NULL, 0, r, sizeof(r), NULL))
        return -1;
    memcpy(pid, r, 4);
    memcpy(info, r + 4, 0x100);
    return 0;
}

int mdbg_copyout(pid_t pid, intptr_t addr, void *buf, size_t len) {
    uint8_t b[17];
    uint32_t st;
    size_t got = 0;
    int fd;
    pack(b, 'R', (uint32_t)pid, (uint64_t)addr, (uint32_t)len);
    if (xfer(b, sizeof(b), NULL, 0, &st, 4, &fd))
        return -1;
    while (!st && got < len) {
        ssize_t r = recv(fd, (char *)buf + got, len - got, 0);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    return !st && got == len ? 0 : -1;
}

int mdbg_copyin(pid_t pid, const void *buf, intptr_t addr, size_t len) {
    uint8_t b[17];
    uint32_t st;
    pack(b, 'W', (uint32_t)pid, (uint64_t)addr, (uint32_t)len);
    if (xfer(b, sizeof(b), buf, len, &st, 4, NULL))
        return -1;
    return st ? -1 : 0;
}

int kernel_dynlib_handle(pid_t pid, const char *basename, uint32_t *handle) {
    uint8_t b[17];
    uint32_t r[2];
    *handle = 0;
    if (!strcmp(basename, "eboot.bin"))
        return 0;
    pack(b, 'H', (uint32_t)pid, 0, (uint32_t)strlen(basename));
    if (xfer(b, sizeof(b), basename, strlen(basename), r, sizeof(r), NULL) || r[0])
        return -1;
    *handle = r[1];
    return 0;
}

intptr_t kernel_dynlib_dlsym(pid_t pid, uint32_t handle, const char *sym) {
    uint8_t b[17];
    uint64_t addr;
    pack(b, 'S', (uint32_t)pid, handle, (uint32_t)strlen(sym));
    if (xfer(b, sizeof(b), sym, strlen(sym), &addr, 8, NULL))
        return 0;
    return (intptr_t)addr;
}

/* a memory map entry, as the test console reports it */
struct region {
    uint64_t start, end;
    int prot;
};

int get_vmmap(uint32_t pid, struct region *r, int max) {
    uint8_t b[17], e[20];
    uint32_t n, i;
    size_t got;
    int fd;
    pack(b, 'M', pid, 0, 0);
    if (xfer(b, sizeof(b), NULL, 0, &n, 4, &fd))
        return -1;
    if (n == 0xffffffffu) {
        close(fd);
        return -1;
    }
    for (i = 0; i < n; i++) {
        for (got = 0; got < sizeof(e);) {
            ssize_t k = recv(fd, e + got, sizeof(e) - got, 0);
            if (k <= 0) {
                close(fd);
                return -1;
            }
            got += (size_t)k;
        }
        if ((int)i < max) {
            memcpy(&r[i].start, e, 8);
            memcpy(&r[i].end, e + 8, 8);
            memcpy(&r[i].prot, e + 16, 4);
        }
    }
    close(fd);
    return (int)n < max ? (int)n : max;
}

intptr_t kernel_dynlib_mapbase_addr(pid_t pid, uint32_t handle) {
    uint8_t b[17];
    uint64_t base;
    (void)handle;
    pack(b, 'B', (uint32_t)pid, 0, 0);
    if (xfer(b, sizeof(b), NULL, 0, &base, 8, NULL))
        return -1;
    return base ? (intptr_t)base : -1;
}

uint32_t kernel_get_fw_version(void) {
    return 0x13600007; /* as reported by a 13.60 console */
}
