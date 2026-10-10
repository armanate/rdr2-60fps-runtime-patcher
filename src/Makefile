# Build the PS5 payload (ps5-payload-sdk)
#   export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
#   make             -> "RedDead2 60FPS_v<version>.elf"
#   make send PS5_HOST=192.168.x.x   -> push it to elfldr (port 9021)
PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifeq ($(PS5_PAYLOAD_SDK),)
    ifneq ($(MAKECMDGOALS),clean)
        $(error PS5_PAYLOAD_SDK is undefined)
    endif
else
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

# PLDMGR shows the file name with "_v1.0" as a version badge and "_"/"-" as
# spaces, so the name itself needs a real space ("_60FPS" would read as a
# version). Make can't use names with spaces as targets, so the recipes
# below are phony and always rebuild (it takes a second).
TOOL_VERSION := $(shell sed -n 's/^\#define TOOL_VERSION "\(.*\)"/\1/p' rdr2-60fps.c)
ELF := RedDead2 60FPS_v$(TOOL_VERSION).elf

CFLAGS := -Wall -Wextra -Werror -Os
SRCS := rdr2-60fps.c ptwalk.c

all:
	$(CC) $(CFLAGS) -o "$(ELF)" $(SRCS)

send: all
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) "$(ELF)"

clean:
	rm -f "$(ELF)" rdr2-60fps.elf

.PHONY: all send clean
