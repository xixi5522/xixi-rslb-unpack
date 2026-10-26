NDK_TOOLCHAIN_VERSION := clang
APP_STL := c++_static
APP_PIE := $(APP_PIE_REQUIRED)

# 指定目标 ABI（可根据需要修改）
APP_ABI := arm64-v8a

# 优化选项
APP_CPPFLAGS := -O2
APP_CFLAGS := -O2