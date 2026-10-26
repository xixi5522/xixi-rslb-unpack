LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := rslb

LOCAL_SRC_FILES := \
    main.c \
    rslb_pack.c \
    rslb_unpack.c \
    sdk/Alloc.c \
    sdk/CpuArch.c \
    sdk/LzFind.c \
    sdk/LzFindMt.c \
    sdk/LzFindOpt.c \
    sdk/LzmaDec.c \
    sdk/LzmaEnc.c \
    sdk/Threads.c

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH) \
    $(LOCAL_PATH)/sdk

LOCAL_CFLAGS := -O2 -Wall -Werror

# 移除 -lpthread，Android 已内置
LOCAL_LDLIBS :=

include $(BUILD_EXECUTABLE)