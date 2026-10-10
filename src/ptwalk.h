/* SPDX-License-Identifier: GPL-3.0-only
 * DMAP physical write, ported from ps5debug-NG (see ptwalk.c). */
#pragma once
#include <stdint.h>

#define PTWALK_OK              0
#define PTWALK_RESOLVE_FAILED  1
#define PTWALK_COPY_FAILED     2
#define PTWALK_VERIFY_FAILED   3

/* Write len bytes of src into process pid at virtual address va, through the
 * kernel direct map. Returns PTWALK_OK on success (bytes verified in physical
 * memory), or one of the PTWALK_* error codes. */
int ptwalk_write(uint32_t pid, uint64_t va, const void *src, uint64_t len);
