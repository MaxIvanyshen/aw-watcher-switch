.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO not set)
endif
TARGET   := sync-probe
SOURCES  := source
include $(DEVKITPRO)/libnx/switch_rules

ARCH   := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := -g -Wall -Wextra -Werror -O2 -ffunction-sections $(ARCH) -D__SWITCH__ -I$(DEVKITPRO)/libnx/include
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(TARGET).map
LIBS   := -L$(DEVKITPRO)/libnx/lib -lnx

$(TARGET).nro: $(TARGET).elf
$(TARGET).elf: source/main.c
	aarch64-none-elf-gcc $(CFLAGS) $< $(LDFLAGS) $(LIBS) -o $@
