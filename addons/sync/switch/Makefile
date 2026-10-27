.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO not set)
endif
TARGET := aw-sync
APP_JSON := sync.json
include $(DEVKITPRO)/libnx/switch_rules

# Paths are relative to addons/sync/switch: the watcher's crc32_of lives in ../../../core/src/util.c.
SRCS   := $(sort $(wildcard ../src/*.c)) ../../../core/src/util.c source/platform.c source/main.c
ARCH   := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := -g -Wall -Wextra -Werror -std=gnu11 -O2 -ffunction-sections -fdata-sections $(ARCH) -D__SWITCH__ \
          -I../include -I../src -I../../../core/src -Isource -I$(DEVKITPRO)/libnx/include
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,--gc-sections -Wl,-Map,$(TARGET).map
LIBS   := -L$(DEVKITPRO)/libnx/lib -lnx

$(TARGET).elf: $(SRCS)
	aarch64-none-elf-gcc $(CFLAGS) $(SRCS) $(LDFLAGS) $(LIBS) -o $@
$(TARGET).nso: $(TARGET).elf
$(TARGET).nsp: $(TARGET).nso $(TARGET).npdm
