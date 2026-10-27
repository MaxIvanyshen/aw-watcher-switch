.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO not set)
endif
TARGET := background-probe
APP_JSON := probe.json
include $(DEVKITPRO)/libnx/switch_rules

ARCH   := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := -g -Wall -Wextra -Werror -std=gnu11 -O2 -ffunction-sections $(ARCH) -D__SWITCH__ -I$(DEVKITPRO)/libnx/include
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(TARGET).map
LIBS   := -L$(DEVKITPRO)/libnx/lib -lnx

$(TARGET).elf: source/main.c
	aarch64-none-elf-gcc $(CFLAGS) $< $(LDFLAGS) $(LIBS) -o $@
$(TARGET).nso: $(TARGET).elf
$(TARGET).nsp: $(TARGET).nso $(TARGET).npdm
