# Build rdr2-60fps.elf (PS5 payload, ps5-payload-sdk)
#   export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
#   make             -> rdr2-60fps.elf
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

ELF := rdr2-60fps.elf

CFLAGS := -Wall -Wextra -Werror -Os

all: $(ELF)

$(ELF): rdr2-60fps.c
	$(CC) $(CFLAGS) -o $@ $^

send: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

clean:
	rm -f $(ELF)

.PHONY: all send clean
