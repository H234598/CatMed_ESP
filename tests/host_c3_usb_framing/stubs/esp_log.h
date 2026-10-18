#ifndef KATMED_C3_FRAMING_TEST_ESP_LOG_H
#define KATMED_C3_FRAMING_TEST_ESP_LOG_H

void test_esp_log_info(const char *tag, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
#define ESP_LOGI(tag, format, ...) test_esp_log_info(tag, format, ##__VA_ARGS__)

#endif
