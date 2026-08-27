.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Set DEVKITPRO first, for example: export DEVKITPRO=/opt/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := angrybirdas_nx
APP_TITLE   := Angry Bird Epic All Stars
APP_AUTHOR  := xFlippy
APP_VERSION := 1.0.0
BUILD       := build
SOURCES     := source
INCLUDES    := source
RELEASE_DIR := release/$(TARGET)

ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS   := -g -Wall -Wextra -O2 -ffunction-sections -fdata-sections $(ARCH) $(DEFINES) $(INCLUDE) -D__SWITCH__
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
ASFLAGS  := -g $(ARCH)
LDFLAGS  := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,--gc-sections -Wl,-Map,$(notdir $*.map)

LIBS    := -lSDL2 -lGLESv2 -lEGL -lglapi -ldrm_nouveau -lz -lnx -lm
LIBDIRS := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES := \
    aaudio_bridge.c \
    android_ndk.c \
    bionic.c \
    bionic_jump.c \
    crash_log.c \
    credits_patch.c \
    fakefd.c \
    fatal.c \
    file_bridge.c \
    fmod_audio.c \
    guest_stack.c \
    imports.c \
    input.c \
    installer.c \
    jni_fake.c \
    main.c \
    media_stubs.c \
    runtime.c \
    so_util.c \
    unity_patches.c
CPPFILES :=
SFILES := guest_stack_switch.s
export LD := $(CXX)
export OFILES := $(SFILES:.s=.o) $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(PORTLIBS)/include/SDL2 -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: all clean release
all: $(BUILD) release
$(BUILD):
	@mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
release: $(BUILD)
	@mkdir -p $(RELEASE_DIR)
	@cp $(TARGET).nro $(RELEASE_DIR)/$(TARGET).nro
	@echo "built $(RELEASE_DIR)/$(TARGET).nro"
clean:
	@rm -rf $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf $(TARGET).map $(RELEASE_DIR)/$(TARGET).nro
else
DEPENDS := $(OFILES:.o=.d)
NROFLAGS := --nacp=$(OUTPUT).nacp --icon=$(TOPDIR)/icon.jpg
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)
-include $(DEPENDS)
endif
