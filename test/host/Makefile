# 宿主机测试：把 ESP-IDF 完全排除在外，只编译纯 C 核心驱动
#
# 常规环境直接：
#   make && ./test_ads1115
#
# 若宿主机 clang 与 CommandLineTools SDK 版本不匹配（报 tapi / tbd malformed
# / unknown architecture 之类链接错误），改用 Xcode 自带工具链：
#   make xcode && ./test_ads1115
CC       ?= cc
SYSROOT  ?=
CFLAGS   ?= -std=gnu99 -Wall -Wextra -Werror -O2 -g
INC      := -I../../components/ads1115/include
SRCS     := test_ads1115.c ../../components/ads1115/ads1115.c
TARGET   := test_ads1115

XCODE_DEV := /Applications/Xcode.app/Contents/Developer
XCODE_CC  := $(XCODE_DEV)/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang
XCODE_SDK := $(XCODE_DEV)/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk

.PHONY: all run xcode clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(if $(SYSROOT),-isysroot $(SYSROOT),) $(INC) -o $(TARGET) $(SRCS) -lm

run: $(TARGET)
	./$(TARGET)

# 绕开 CommandLineTools 链接器问题
xcode:
	$(XCODE_CC) $(CFLAGS) -isysroot $(XCODE_SDK) $(INC) -o $(TARGET) $(SRCS) -lm
	./$(TARGET)

clean:
	rm -f $(TARGET)
