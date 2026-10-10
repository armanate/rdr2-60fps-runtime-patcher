/* host-test stand-in for the SDK's <ps5/mdbg.h> */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

int mdbg_copyout(pid_t pid, intptr_t addr, void *buf, size_t len);
int mdbg_copyin(pid_t pid, const void *buf, intptr_t addr, size_t len);
