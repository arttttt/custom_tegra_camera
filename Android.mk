LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := camera.tegra
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_MODULE_TAGS := optional

LOCAL_SRC_FILES := camera_hal3.cpp

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

LOCAL_CFLAGS := -Wall -Wextra -Wno-unused-parameter -DMIUI_CAMERA_SERVICE
LOCAL_LDFLAGS := -Wl,--hash-style=sysv
LOCAL_NDK_STL_VARIANT := none
LOCAL_SDK_VERSION := 19

include $(BUILD_SHARED_LIBRARY)
