/* host-test stand-in for sceKernelSendNotificationRequest: prints the toast */
#include <stddef.h>
#include <stdio.h>

typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int dev, notify_request_t *req, size_t len,
                                     int flags) {
    (void)dev; (void)len; (void)flags;
    printf("TOAST %s\n", req->message);
    fflush(stdout);
    return 0;
}
