#---------------------------------------------------------------------------------
# NX Downloader —— Nintendo Switch 自制程序（homebrew）
#
# 基于 devkitPro / libnx 标准 Makefile 模板 + borealis UI 库
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# TARGET      输出文件名（最终得到 $(TARGET).nro）
# BUILD       中间文件目录
# SOURCES     源码目录（不递归）
# DATA        二进制数据目录（会被 bin2o 打包）
# INCLUDES    头文件目录
# ROMFS       RomFS 目录（会被打进 .nro，运行时通过 romfs:/ 访问）
#---------------------------------------------------------------------------------
TARGET		:=	nx-downloader
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	source
ROMFS		:=	romfs

APP_TITLE	:=	NX Downloader
APP_AUTHOR	:=	xtgxiso
APP_VERSION	:=	2.7.1

# 图标（jpg）。删掉 icon.jpg 时请同时设置 NO_ICON := 1，否则 elf2nro 会报错
ICON		:=	icon.jpg

#---------------------------------------------------------------------------------
# borealis UI 库位置：$(BOREALIS_PATH)/library/{include,lib}
#   git clone --depth=1 https://github.com/XITRIX/borealis.git borealis
#---------------------------------------------------------------------------------
BOREALIS_PATH		:=	borealis
BOREALIS_RESOURCES	:=	romfs:/

#---------------------------------------------------------------------------------
# libcurl：编译/链接参数直接由 devkitPro 提供的 curl-config 生成，
# 这样 libcurl 的传递依赖（mbedtls / zlib 等）不会漏
#---------------------------------------------------------------------------------
CURL_CONFIG	:=	$(PORTLIBS)/bin/curl-config
CURL_CFLAGS	:=	$(shell $(CURL_CONFIG) --cflags)
CURL_LIBS	:=	$(shell $(CURL_CONFIG) --libs)

#---------------------------------------------------------------------------------
# 编译选项
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS	:=	-g -Wall -O2 -ffunction-sections \
			$(ARCH) $(DEFINES)

CFLAGS	+=	$(INCLUDE) -D__SWITCH__ \
			-DBOREALIS_RESOURCES="\"$(BOREALIS_RESOURCES)\"" \
			$(CURL_CFLAGS)

# borealis 要求 C++17，并且必须保留 RTTI 与异常（不要加 -fno-rtti / -fno-exceptions）
CXXFLAGS	:=	$(CFLAGS) -std=c++1z -Wno-volatile -Wno-unused-parameter

ASFLAGS	:=	-g $(ARCH)

LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# 链接顺序：依赖方在前，被依赖方在后
LIBS	:=	$(CURL_LIBS) -lnx -lm

#---------------------------------------------------------------------------------
# 库搜索路径：portlibs（curl 等）与 libnx
#---------------------------------------------------------------------------------
LIBDIRS	:=	$(PORTLIBS) $(LIBNX)

# 引入 borealis（必须在 LIBDIRS / SOURCES / INCLUDES 定义之后）
include $(TOPDIR)/$(BOREALIS_PATH)/library/borealis.mk

#---------------------------------------------------------------------------------
# 以下内容通常无需修改
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# 使用 CXX 链接 C++ 工程
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(CONFIG_JSON)),)
	jsons := $(wildcard *.json)
	ifneq (,$(findstring $(TARGET).json,$(jsons)))
		export APP_JSON := $(TOPDIR)/$(TARGET).json
	else
		ifneq (,$(findstring config.json,$(jsons)))
			export APP_JSON := $(TOPDIR)/config.json
		endif
	endif
else
	export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)
endif

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg)
	ifneq (,$(findstring $(TARGET).jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).jpg
	else
		ifneq (,$(findstring icon.jpg,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.jpg
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

ifneq ($(ROMFS),)
	export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all sync-resources

# 必须显式指定默认目标：
#   GNU make 默认执行文件里「第一个」目标，而 sync-resources 规则写在 all 之前，
#   不加这行的话直接 `make` 只会同步资源、不编译（表现为 1 秒结束且没有 .nro）。
.DEFAULT_GOAL := all

#---------------------------------------------------------------------------------
# 把 borealis 的运行时资源同步进 romfs/
#   * i18n/     —— 底部按键提示、崩溃界面的文案
#   * material/ —— 图标字体（文件夹等 Material 图标靠它渲染）
#
# 首次编译前执行一次即可（--depth=1 克隆 borealis 之后）：
#   make sync-resources
#---------------------------------------------------------------------------------
sync-resources:
	@if [ ! -d "$(BOREALIS_PATH)" ]; then \
		echo "!! 找不到 $(BOREALIS_PATH)/，请先克隆 borealis："; \
		echo "   git clone --depth=1 https://github.com/XITRIX/borealis.git $(BOREALIS_PATH)"; \
		exit 1; \
	fi
	@mkdir -p $(ROMFS)
	@cp -r $(BOREALIS_PATH)/resources/i18n $(ROMFS)/
	@cp -r $(BOREALIS_PATH)/resources/material $(ROMFS)/
	@echo "-- borealis 资源已同步到 $(ROMFS)/"

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@MSYS2_ARG_CONV_EXCL="-D;$(MSYS2_ARG_CONV_EXCL)" $(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
ifeq ($(strip $(APP_JSON)),)
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
else
	@rm -fr $(BUILD) $(TARGET).nsp $(TARGET).nso $(TARGET).npdm $(TARGET).elf
endif

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
ifeq ($(strip $(APP_JSON)),)

all	:	$(OUTPUT).nro

ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro	:	$(OUTPUT).elf
endif

else

all	:	$(OUTPUT).nsp

$(OUTPUT).nsp	:	$(OUTPUT).nso $(OUTPUT).npdm

$(OUTPUT).nso	:	$(OUTPUT).elf

endif

$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
