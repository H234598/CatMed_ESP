"""Run the production boot sampler with independently changing capability heaps.

This verifies fresh per-stage readings and field/capability attribution, not a
claim about device allocation owners. Firmware CI compiles the full call path.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class StartupHeap(unittest.TestCase):
    def test_fresh_stage_samples_keep_internal_and_dma_fields_distinct(self):
        text = (ROOT / "firmware/main/main.c").read_text()
        start = text.index("static void startup_heap_sample(")
        end = text.index("\n}\n", start) + 3
        sampler = text[start:end]
        prefix = r'''
#include <assert.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#define ESP_LOGI(tag, ...) printf(__VA_ARGS__)
static unsigned stage, reads;
static const size_t samples[2][4] = {
    {22820, 14848, 15180, 14848}, {20244, 11776, 12604, 11776}
};
static size_t sample(uint32_t caps, unsigned kind) {
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    assert(caps == internal || caps == (internal | MALLOC_CAP_DMA));
    unsigned field = kind + ((caps & MALLOC_CAP_DMA) ? 2 : 0);
    assert(reads++ == field); /* Exactly one ordered read per field. */
    return samples[stage][field];
}
size_t heap_caps_get_free_size(uint32_t caps) { return sample(caps, 0); }
size_t heap_caps_get_largest_free_block(uint32_t caps) { return sample(caps, 1); }
'''
        suffix = r'''
int main(void) {
    startup_heap_sample("after_ui"); assert(reads == 4);
    puts(""); ++stage; reads = 0;
    startup_heap_sample("after_network"); assert(reads == 4);
    puts("");
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="katmed-startup-heap-") as tmp:
            source = Path(tmp) / "sample.c"
            executable = Path(tmp) / "sample"
            source.write_text(prefix + sampler + suffix)
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-I" + str(ROOT / "tests/host_main/stubs"),
                str(source), "-o", str(executable),
            ], check=True)
            result = subprocess.run([str(executable)], check=True,
                                    capture_output=True, text=True)
        self.assertEqual(result.stdout.splitlines(), [
            "startup stage=after_ui internal_free=22820 internal_largest=14848 "
            "dma_free=15180 dma_largest=14848",
            "startup stage=after_network internal_free=20244 internal_largest=11776 "
            "dma_free=12604 dma_largest=11776",
        ])


if __name__ == "__main__":
    unittest.main()
