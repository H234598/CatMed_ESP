#include "esp_netif.h"

/* Mesh-Lite 1.0.2's required private archive still references the IDF 5.x
 * iterator name. IDF 6.1 exposes the same unlocked traversal as
 * esp_netif_next_unsafe(); callers must retain the API's stated safety
 * conditions (for example, TCP/IP context or no concurrent removal). */
esp_netif_t *esp_netif_next(esp_netif_t *netif)
{
    return esp_netif_next_unsafe(netif);
}
