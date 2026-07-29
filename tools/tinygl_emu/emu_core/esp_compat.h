#ifndef _ESP_COMPAT_H_
#define _ESP_COMPAT_H_

#include <stdint.h>
#include <unistd.h>
#include <time.h>

// 定时器
static inline int64_t esp_timer_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

// 任务延迟
static inline void vTaskDelay(uint32_t ticks) {
    usleep(ticks * 1000);  // 简化为 ms 级
}

#define pdMS_TO_TICKS(ms) (ms)

// ESP-IDF 日志宏
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) printf("I (%s): " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("W (%s): " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("E (%s): " fmt "\n", tag, ##__VA_ARGS__)

#endif
