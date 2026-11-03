#ifndef KATMED_C3_RESOURCE_POLICY_H
#define KATMED_C3_RESOURCE_POLICY_H

/* Included by the full-stack main on both boards. Each HTTPS/HTTP server
 * owns a listen socket plus control receive/send sockets (six total).
 * Preserve a four-socket background allowance (not a measured owner census),
 * one HTTP client and one C3 / two C6 HTTPS clients: twelve / thirteen total.
 * Reject retained sdkconfig values that override either board default. */
#if defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32C6) && CONFIG_IDF_TARGET_ESP32C6
#if CONFIG_LWIP_MAX_SOCKETS < 13
#error "C6_FULLSTACK_SOCKET_POLICY: require at least 13 lwIP sockets"
#endif
#elif defined(ESP_PLATFORM) && CONFIG_LWIP_MAX_SOCKETS < 12
#error "FULLSTACK_SOCKET_POLICY: require at least 12 lwIP sockets"
#endif

/* The full-stack C3 shares SRAM between code, UI and network/Web. These
 * resolved SDK choices are part of its resource profile, including private
 * builds: retained sdkconfig values must not silently defeat board defaults.
 * Official flash placement and a matched two-buffer/two-frame RX window keep
 * AMPDU and dynamic RX/TX enabled. Throughput/latency still need device checks.
 * The four Web admission floors remain unchanged. */
#if defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32C3) && CONFIG_IDF_TARGET_ESP32C3
#if (defined(CONFIG_ESP_WIFI_IRAM_OPT) && CONFIG_ESP_WIFI_IRAM_OPT) || \
    (defined(CONFIG_ESP_WIFI_RX_IRAM_OPT) && CONFIG_ESP_WIFI_RX_IRAM_OPT)
#error "C3_RESOURCE_POLICY_PLACEMENT: WiFi speed-optimized code must use flash"
#endif
#if CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM != 2 || CONFIG_ESP_WIFI_RX_BA_WIN != 2
#error "C3_RESOURCE_POLICY_RX: require two static RX buffers and BA window two"
#endif
#if !CONFIG_ESP_WIFI_AMPDU_RX_ENABLED || !CONFIG_ESP_WIFI_AMPDU_TX_ENABLED
#error "C3_RESOURCE_POLICY_FEATURES: AMPDU RX and TX must remain enabled"
#endif
#if !CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER || CONFIG_ESP_WIFI_TX_BUFFER_TYPE != 1 || \
    CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM != 32 || CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM != 32
#error "C3_RESOURCE_POLICY_DYNAMIC: retain dynamic TX and bounded RX/TX limits 32"
#endif
#endif

#endif
