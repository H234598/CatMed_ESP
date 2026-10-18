#ifndef C3_INDEPENDENT_ESP_LOG_H
#define C3_INDEPENDENT_ESP_LOG_H
void independent_sdk_log_info(const char *tag, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
#define ESP_LOGI(tag, format, ...) independent_sdk_log_info(tag, format, ##__VA_ARGS__)
#endif
