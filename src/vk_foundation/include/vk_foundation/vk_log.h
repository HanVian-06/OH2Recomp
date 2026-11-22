#pragma once

#include <android/log.h>

#define OH2_M3_TAG "OH2_M3_VK"
#define OH2_VK_VAL_TAG "VulkanValidation"

#define M3_LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO, OH2_M3_TAG, fmt, ##__VA_ARGS__)
#define M3_LOGW(fmt, ...) __android_log_print(ANDROID_LOG_WARN, OH2_M3_TAG, fmt, ##__VA_ARGS__)
#define M3_LOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, OH2_M3_TAG, fmt, ##__VA_ARGS__)
