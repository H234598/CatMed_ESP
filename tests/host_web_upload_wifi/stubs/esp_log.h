#ifndef KATMED_HOST_WIFI_ESP_LOG_H
#define KATMED_HOST_WIFI_ESP_LOG_H
#ifdef KATMED_WIFI_DIAGNOSTIC_CAPTURE
void katmed_wifi_test_log(const char *tag, const char *format, ...);
#define ESP_LOGW(tag, ...) katmed_wifi_test_log(tag, __VA_ARGS__)
#else
#define ESP_LOGW(...) ((void)0)
#endif
#endif
