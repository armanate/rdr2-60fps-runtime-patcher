/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Direct-map (DMAP) physical write for a foreign process.
 *
 * On PS5 firmware >= 8.40 the debug memory-write syscall (mdbg) is refused for
 * a game's executable pages, so a 3-byte code patch cannot land through it.
 * This walks the target process's page tables to translate the virtual address
 * to a physical one, then writes through the kernel's direct map, which
 * bypasses the page protection.
 *
 * This file is a port of ps5debug-NG's proc_ptwalk.c (GPL-3.0,
 * https://github.com/OpenSourcereR-dev/ps5debug-NG) by OpenSourcereR. The page
 * walk, the self-validating DMAP discovery and the write loop are theirs; the
 * only changes are using the ps5-payload-sdk kernel primitives
 * (kernel_copyin/kernel_copyout/kernel_get_proc) in place of ps5debug-NG's own
 * fast variants, and dropping its diagnostics and network plumbing.
 *
 * Because this file is GPL-3.0, the whole rdr2-60fps payload is distributed
 * under GPL-3.0 (see LICENSE).
 */
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "ptwalk.h"

#define PTW_PMAP_OFF_KNOWN    0x300ULL      /* pm_pml4/pm_cr3 pair in vmspace */
#define PTW_VMSPACE_SCAN_END  0x400ULL

#define PTW_PTE_PRESENT  0x1ULL
#define PTW_PTE_PS       0x80ULL
#define PTW_PHYS_MASK    0x000FFFFFFFFFF000ULL
#define PTW_PHYS_BOUND   0x1000000000ULL
#define PTW_CANON_KERNEL 0x1FFFFULL

/* a known value at a known virtual address, used to confirm a candidate dmap */
static volatile uint64_t g_ptw_gt = 0x5E7C0DE5E7C0DE71ULL;

static int      g_ptw_state    = 0;  /* 0 = untried, 1 = ok, -1 = failed */
static uint64_t g_ptw_dmap     = 0;
static uint64_t g_ptw_pmap_off = 0;

static uint64_t ptw_vtophys(uint64_t dmap, uint64_t cr3, uint64_t va) {
    uint64_t table = cr3 & PTW_PHYS_MASK;
    for (int level = 0; level < 4; level++) {
        if (table >= PTW_PHYS_BOUND) return 0;
        int      shift = 39 - level * 9;
        uint64_t idx   = (va >> shift) & 0x1FF;
        uint64_t e     = 0;
        if (kernel_copyout((intptr_t)(dmap + table + idx * 8), &e, 8) != 0)
            return 0;
        if (!(e & PTW_PTE_PRESENT)) return 0;
        if (level == 3)
            return (e & PTW_PHYS_MASK) | (va & 0xFFF);
        if ((level == 1 || level == 2) && (e & PTW_PTE_PS)) {
            uint64_t pgmask = (level == 1) ? ((1ULL << 30) - 1) : ((1ULL << 21) - 1);
            return ((e & PTW_PHYS_MASK) & ~pgmask) | (va & pgmask);
        }
        table = e & PTW_PHYS_MASK;
    }
    return 0;
}

static int ptw_walk_leaf(uint64_t dmap, uint64_t cr3, uint64_t va,
                         uint64_t *out_e, int *out_level) {
    uint64_t table = cr3 & PTW_PHYS_MASK;
    for (int level = 0; level < 4; level++) {
        if (table >= PTW_PHYS_BOUND) { *out_e = 0; *out_level = level; return 1; }
        int      shift = 39 - level * 9;
        uint64_t idx   = (va >> shift) & 0x1FF;
        uint64_t e     = 0;
        if (kernel_copyout((intptr_t)(dmap + table + idx * 8), &e, 8) != 0) {
            *out_e = 0; *out_level = level; return 1;
        }
        if (!(e & PTW_PTE_PRESENT)) { *out_e = e; *out_level = level; return 1; }
        if (level == 3) { *out_e = e; *out_level = 3; return 0; }
        if ((level == 1 || level == 2) && (e & PTW_PTE_PS)) {
            *out_e = e; *out_level = level; return 0;
        }
        table = e & PTW_PHYS_MASK;
    }
    *out_e = 0; *out_level = -1;
    return 1;
}

/* read {pml4_va, cr3} at vmspace+off, derive dmap, confirm it against g_ptw_gt */
static uint64_t ptw_try_offset(uint64_t vmspace, uint64_t off,
                               uint64_t gt_va, uint64_t gt_val) {
    uint64_t pair[2];
    if (kernel_copyout((intptr_t)(vmspace + off), pair, sizeof(pair)) != 0)
        return 0;

    uint64_t pml4_va = pair[0];
    uint64_t cr3     = pair[1];

    if ((pml4_va >> 47) != PTW_CANON_KERNEL) return 0;
    if (pml4_va & 0xFFF)                     return 0;
    if (cr3 == 0 || (cr3 & 0xFFF))           return 0;
    if (cr3 >= PTW_PHYS_BOUND)               return 0;

    uint64_t dmap = pml4_va - cr3;
    if (dmap & 0xFFFFFFFFULL)                return 0;
    if ((dmap >> 47) != PTW_CANON_KERNEL)    return 0;

    uint64_t phys = ptw_vtophys(dmap, cr3, gt_va);
    if (phys == 0) return 0;

    uint64_t probe = 0;
    if (kernel_copyout((intptr_t)(dmap + phys), &probe, 8) != 0) return 0;
    if (probe != gt_val) return 0;

    return dmap;
}

static int ptw_discover(void) {
    if (g_ptw_state != 0) return g_ptw_state;

    int result = -1;
    intptr_t kproc = kernel_get_proc(getpid());
    if (kproc) {
        uint64_t vmspace = 0;
        if (kernel_copyout((intptr_t)(kproc + KERNEL_OFFSET_PROC_P_VMSPACE),
                           &vmspace, 8) == 0 && vmspace) {
            uint64_t gt_va  = (uint64_t)(uintptr_t)&g_ptw_gt;
            uint64_t gt_val = g_ptw_gt;

            uint64_t found_off = PTW_PMAP_OFF_KNOWN;
            uint64_t dmap = ptw_try_offset(vmspace, found_off, gt_va, gt_val);
            if (!dmap) {
                for (uint64_t off = 0x100; off + 16 <= PTW_VMSPACE_SCAN_END;
                     off += 8) {
                    if (off == PTW_PMAP_OFF_KNOWN) continue;
                    dmap = ptw_try_offset(vmspace, off, gt_va, gt_val);
                    if (dmap) { found_off = off; break; }
                }
            }
            if (dmap) {
                g_ptw_dmap     = dmap;
                g_ptw_pmap_off = found_off;
                result = 1;
            }
        }
    }

    g_ptw_state = result;
    return result;
}

static int ptw_verify_phys(uint64_t phys, const uint8_t *expected,
                           uint64_t len) {
    uint8_t actual[0x1000];
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = len - done;
        if (n > sizeof(actual)) n = sizeof(actual);
        if (kernel_copyout((intptr_t)(g_ptw_dmap + phys + done), actual,
                           (size_t)n) != 0)
            return PTWALK_VERIFY_FAILED;
        if (memcmp(actual, expected + done, (size_t)n) != 0)
            return PTWALK_VERIFY_FAILED;
        done += n;
    }
    return PTWALK_OK;
}

int ptwalk_write(uint32_t pid, uint64_t va, const void *src, uint64_t len) {
    if ((int32_t)pid <= 0 || !src || len == 0 || va + len < va)
        return PTWALK_RESOLVE_FAILED;
    if (ptw_discover() != 1) return PTWALK_RESOLVE_FAILED;

    intptr_t kproc = kernel_get_proc((pid_t)pid);
    if (!kproc) return PTWALK_RESOLVE_FAILED;

    uint64_t vmspace = 0;
    if (kernel_copyout((intptr_t)(kproc + KERNEL_OFFSET_PROC_P_VMSPACE),
                       &vmspace, 8) != 0 || !vmspace)
        return PTWALK_RESOLVE_FAILED;

    uint64_t pair[2];
    if (kernel_copyout((intptr_t)(vmspace + g_ptw_pmap_off), pair,
                       sizeof(pair)) != 0)
        return PTWALK_RESOLVE_FAILED;

    uint64_t cr3 = pair[1];
    if (cr3 == 0 || (cr3 & 0xFFF) || cr3 >= PTW_PHYS_BOUND)
        return PTWALK_RESOLVE_FAILED;
    if (pair[0] - cr3 != g_ptw_dmap)
        return PTWALK_RESOLVE_FAILED;

    const uint8_t *in   = (const uint8_t *)src;
    uint64_t       done = 0;
    while (done < len) {
        uint64_t cur_va = va + done;
        uint64_t e   = 0;
        int      lvl = -1;
        if (ptw_walk_leaf(g_ptw_dmap, cr3, cur_va, &e, &lvl) != 0)
            return PTWALK_RESOLVE_FAILED;

        uint64_t page_size;
        if      (lvl == 3) page_size = 0x1000ULL;
        else if (lvl == 2) page_size = 0x200000ULL;
        else if (lvl == 1) page_size = 0x40000000ULL;
        else return PTWALK_RESOLVE_FAILED;

        uint64_t page_mask   = page_size - 1;
        uint64_t off_in_page = cur_va & page_mask;
        uint64_t phys        = ((e & PTW_PHYS_MASK) & ~page_mask) + off_in_page;

        uint64_t n = len - done;
        uint64_t avail = page_size - off_in_page;
        if (n > avail) n = avail;

        if (phys >= PTW_PHYS_BOUND || phys + n < phys
            || phys + n > PTW_PHYS_BOUND)
            return PTWALK_RESOLVE_FAILED;

        if (kernel_copyin(in + done, (intptr_t)(g_ptw_dmap + phys), n) != 0)
            return PTWALK_COPY_FAILED;

        int verify_rc = ptw_verify_phys(phys, in + done, n);
        if (verify_rc != PTWALK_OK) return verify_rc;
        done += n;
    }
    return PTWALK_OK;
}
