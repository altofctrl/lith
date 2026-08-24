// ESP-IDF's GPIO header. One sketch in the corpus includes it and then only
// uses the Arduino-level pin calls, which the harness already shims; the few
// IDF names below exist so the include resolves rather than to make anything
// behave.
#pragma once
#include "sim_arduino.h"

typedef int gpio_num_t;
typedef int gpio_mode_t;
typedef int gpio_pull_mode_t;

#define GPIO_MODE_INPUT      0
#define GPIO_MODE_OUTPUT     1
#define GPIO_PULLUP_ONLY     2
#define GPIO_PULLDOWN_ONLY   3
#define GPIO_FLOATING        4

static inline esp_err_t gpio_set_direction(gpio_num_t, gpio_mode_t) { return ESP_OK; }
static inline esp_err_t gpio_set_pull_mode(gpio_num_t, gpio_pull_mode_t) { return ESP_OK; }
static inline esp_err_t gpio_set_level(gpio_num_t, uint32_t) { return ESP_OK; }
static inline int       gpio_get_level(gpio_num_t p) { return digitalRead(p); }
