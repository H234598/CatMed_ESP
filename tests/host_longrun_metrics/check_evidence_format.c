#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "runtime_metrics.h"

static unsigned lines;
static char state[1024], health[1024];

void test_log_info(const char *tag, const char *format, ...)
{
    char *out = strcmp(tag, "katmed_state") == 0 ? state : health;
    assert(strcmp(tag, "katmed_state") == 0 || strcmp(tag, "katmed_health") == 0);
    va_list args;
    va_start(args, format);
    int n = vsnprintf(out, 1024, format, args);
    va_end(args);
    assert(n > 0 && n < 1024);
    ++lines;
}

int main(void)
{
    KatmedRuntimeEvidenceState s = {
        .seq=UINT64_MAX,.uptime_ms=UINT64_MAX,.boot_domain=UINT64_MAX,
        .round_device=UINT64_MAX,.round_epoch=UINT64_MAX,
        .round_sequence=UINT64_MAX,.round_ordinal=UINT64_MAX,
        .origin_device=UINT64_MAX,.origin_epoch=UINT64_MAX,
        .origin_sequence=UINT64_MAX,.start_domain=UINT64_MAX,
        .start_ms=UINT64_MAX,.start_utc_ms=INT64_MIN,.clock_domain=UINT64_MAX,
        .regular_ms=UINT64_MAX,.effective_ms=UINT64_MAX,
        .regular_utc_ms=INT64_MIN,.effective_utc_ms=INT64_MIN,
        .now_utc_ms=INT64_MIN,.alarm=1,.reason=4,.start_utc_valid=1,
        .deadline_local=1,.regular_utc_valid=1,.effective_utc_valid=1,
        .now_utc_valid=1,.now_quality=2,.utc_source=2,
        .image_id=UINT32_MAX,.image_remaining_ms=UINT64_MAX,.image_pending=1,
    };
    KatmedRuntimeEvidenceHealth h = {
        .seq=UINT64_MAX,.uptime_ms=UINT64_MAX,.boot_domain=UINT64_MAX,
        .image_valid=1,.image_failures=UINT32_MAX,.store_valid=1,
        .store_read_failures=UINT32_MAX,.store_load_failures=UINT32_MAX,
        .store_commit_failures=UINT32_MAX,.mesh_valid=1,
        .mesh_send_failures=UINT32_MAX,.mesh_queue_drops=UINT32_MAX,
        .mesh_level_changes=UINT32_MAX,.mesh_ip_got=UINT32_MAX,
        .mesh_ip_lost=UINT32_MAX,.mesh_session_up=UINT32_MAX,
        .mesh_session_down=UINT32_MAX,.mesh_connected=1,.mesh_root=1,
        .mesh_root_ip=1,
    };
    katmed_runtime_evidence_log(&s, &h);
    assert(lines == 2);
    assert(strlen("I (4294967295) katmed_state: ") + strlen(state) <= 767);
    assert(strlen("I (4294967295) katmed_health: ") + strlen(health) <= 767);
    assert(strstr(state, "schema=1 sample_seq=18446744073709551615 ") == state);
    assert(strstr(state, "start_utc=-9223372036854775808") != NULL);
    assert(strstr(health, "mesh_queue=4294967295") != NULL);
    printf("state_chars=%zu health_chars=%zu\n",strlen(state),strlen(health));
    h.seq--;
    katmed_runtime_evidence_log(&s, &h);
    assert(lines == 2);
    return 0;
}
