LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := camera.tegra
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_MODULE_TAGS := optional

# Set to false for AOSP/LineageOS CameraService
MIUI_CAMERA_SERVICE ?= true

LOCAL_SRC_FILES := camera_hal3.cpp

ifeq ($(MIUI_CAMERA_SERVICE),true)
  LOCAL_SRC_FILES += vendor_miui.c
else
  LOCAL_SRC_FILES += vendor_noop.c
endif

LOCAL_SHARED_LIBRARIES := \
    liblog \
    libcutils \
    libdl \
    libhardware \
    libui \
    libutils

LOCAL_C_INCLUDES := \
    system/core/include \
    system/media/camera/include \
    hardware/libhardware/include \
    frameworks/native/include \
    frameworks/av/include

LOCAL_CFLAGS := -Wall -Wextra -Wno-unused-parameter
LOCAL_LDFLAGS := -Wl,--hash-style=sysv
LOCAL_NDK_STL_VARIANT := none
LOCAL_SDK_VERSION := 19

include $(BUILD_SHARED_LIBRARY)
