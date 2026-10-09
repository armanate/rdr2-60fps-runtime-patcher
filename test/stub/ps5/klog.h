/* host-test stand-in for the SDK's <ps5/klog.h> */
#pragma once
#include <stdio.h>
#define klog_printf(...) fprintf(stderr, __VA_ARGS__)
