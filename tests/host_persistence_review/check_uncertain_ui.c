#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main original_host_store_main
#include "../host_store/check_main_submit.c"
#undef main
#pragma GCC diagnostic pop

static StoreResult commit_then_lose_reply(void *context, const uint8_t *data, size_t length)
{
    (void)context;
    assert(length <= sizeof(committed));
    memcpy(committed, data, length);
    committed_length = length;
    ++writes;
    return STORE_COMMIT_UNKNOWN;
}

int main(void)
{
    fake_microseconds = 1000000;
    initialize_runtime();
    runtime.store.backend.write_commit = commit_then_lose_reply;
    UiRequest changed = {.kind = UI_REQUEST_SET_CLOCK_STYLE};
    changed.value.clock_style = ANALOG;
    assert(submit_ui(&runtime, &changed) == UI_SUBMIT_UNAVAILABLE);
    assert(!storage_ready(&runtime));
    assert(runtime.local.clock_style == DIGITAL_LARGE);

    UiRequest unchanged = {.kind = UI_REQUEST_SET_CLOCK_STYLE};
    unchanged.value.clock_style = DIGITAL_LARGE;
    const UiSubmitResult retry = submit_ui(&runtime, &unchanged);
    if (retry != UI_SUBMIT_UNAVAILABLE) {
        fprintf(stderr, "UI claims a saved setting while the committed value is uncertain\n");
        return 1;
    }
    puts("uncertain UI setting was not acknowledged");
}
