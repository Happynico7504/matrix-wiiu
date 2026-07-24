.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=/opt/devkitpro")
endif

TOPDIR ?= $(CURDIR)

#-------------------------------------------------------------------------------
# App metadata — must be set BEFORE including wut_rules, which reads these
# variables immediately (Make evaluates its ifneq guards at include time, not
# lazily) to build WUHB_OPTIONS/WUHB_DEPS. Setting them afterward silently
# drops --name/--icon/--tv-image/--drc-image from the wuhbtool invocation.
#-------------------------------------------------------------------------------
TARGET        :=  matrix-wiiu
BUILD         :=  build
SOURCES       :=  src \
                  src/matrix \
                  src/ui
INCLUDES      :=  src
DATA          :=

APP_NAME       := Matrix Wii U (Unofficial)
APP_SHORTNAME  := Matrix
APP_AUTHOR     := WiiU Homebrew — unofficial, not affiliated with Matrix.org
APP_ICON       := $(TOPDIR)/meta/icon.png
APP_TV_SPLASH  := $(TOPDIR)/meta/bootTv.png
APP_DRC_SPLASH := $(TOPDIR)/meta/bootDrc.png
APP_CONTENT    := $(TOPDIR)/content

include $(DEVKITPRO)/wut/share/wut_rules

#-------------------------------------------------------------------------------
# Build flags
#-------------------------------------------------------------------------------
CFLAGS    :=  -g -Wall -Wextra -O2 $(MACHDEP) \
              -Wno-unused-parameter

CXXFLAGS  :=  $(CFLAGS) -std=c++17 -fexceptions -fno-rtti

ASFLAGS   :=  $(ARCH)

LDFLAGS    =  $(ARCH) $(RPXSPECS) -Wl,-Map,$(notdir $*.map)

LIBS      :=  -lSDL2_image \
              -lSDL2_ttf \
              -lharfbuzz \
              -lfreetype \
              -lpng16 \
              -ljpeg \
              -lwebp \
              -lbz2 \
              -lSDL2 \
              -lcurl \
              -lbrotlidec \
              -lbrotlicommon \
              -lmbedtls \
              -lmbedcrypto \
              -lmbedx509 \
              -lz \
              -lwutd \
              -lwut

LIBDIRS   :=  $(WUT_ROOT) \
              $(PORTLIBS)

#-------------------------------------------------------------------------------
# Two-phase build: top-level sets up exports; recursive make builds in BUILD/
#-------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   :=  $(CURDIR)/$(TARGET)
export TOPDIR   :=  $(CURDIR)
export DEPSDIR  :=  $(CURDIR)/$(BUILD)

CFILES    :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(TOPDIR)/$(dir)/*.c)))
CPPFILES  :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(TOPDIR)/$(dir)/*.cpp)))
SFILES    :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(TOPDIR)/$(dir)/*.s)))

export OFILES   :=  $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export INCLUDE  :=  $(foreach dir,$(INCLUDES),-I$(TOPDIR)/$(dir)) \
                    $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                    -I$(CURDIR)/$(BUILD)

export LIBPATHS :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export VPATH    :=  $(foreach dir,$(SOURCES),$(TOPDIR)/$(dir)) \
                    $(foreach dir,$(DATA),$(TOPDIR)/$(dir))

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -rf $(BUILD) $(TARGET).elf $(TARGET).rpx $(TARGET).wuhb

else

#-------------------------------------------------------------------------------
# Build directory context: wire include paths into CPPFLAGS for base_rules;
# use CXX as linker so C++ runtime and -specs flags are handled correctly.
#-------------------------------------------------------------------------------
CPPFLAGS := $(INCLUDE)
LD       := $(CXX)
DEPENDS   := $(OFILES:.o=.d)

$(OUTPUT).wuhb : $(OUTPUT).rpx
$(OUTPUT).rpx  : $(OUTPUT).elf
$(OUTPUT).elf  : $(OFILES)

-include $(DEPENDS)

endif
