#ifndef KATMED_DHCP_DIAG_H
#define KATMED_DHCP_DIAG_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Read-only TCPIP tuple; core_v=0 invalidates the internal fields.
 * dhcp_v=0 means no DHCP object, even if core_v/link_up are valid.
 * Counters are independent saturating observations, not packet delivery proof. */
typedef struct {
    uint32_t core_v, dhcp_v, link_up, state, tries, timeout, fine;
    uint32_t udp_discover, udp_request, udp_other, udp_invalid, udp_ok, udp_fail;
    int32_t udp_error;
    uint32_t tx_discover, tx_request, tx_other, tx_invalid, tx_ok, tx_fail;
    int32_t tx_error;
    uint32_t task_v, resource_v, resource_count, resource_dhcp_type;
    uint32_t resource_calls, resource_ok, resource_nulls, resource_resource_v;
    uint32_t resource_size, resource_caps, resource_dma_free, resource_dma_largest;
    int32_t resource_result;
} KatmedDhcpDiagnostics;
/* Bind the existing STA after Bridge creation, before Mesh starts. */
void katmed_dhcp_diag_bind_sta(void);
void katmed_dhcp_diag_snapshot(KatmedDhcpDiagnostics *out);
#ifdef __cplusplus
}
#endif
#endif
