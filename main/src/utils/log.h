#pragma once
#include <esp_log.h>

#define STR_IMPL(x) #x
#define STR(x) STR_IMPL(x)
#define LOG_TAG __FILE__ ":" STR(__LINE__)

#define LOG_DEBUG(format, ...) ESP_LOGD(LOG_TAG, format __VA_OPT__(,) __VA_ARGS__)
#define LOG_INFO(format, ...)  ESP_LOGI(LOG_TAG, format __VA_OPT__(,) __VA_ARGS__)
#define LOG_WARN(format, ...)  ESP_LOGW(LOG_TAG, format __VA_OPT__(,) __VA_ARGS__)
#define LOG_ERROR(format, ...) ESP_LOGE(LOG_TAG, format __VA_OPT__(,) __VA_ARGS__)
