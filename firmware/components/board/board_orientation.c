#include "board_orientation.h"

static int32_t magnitude(int16_t n) { return n < 0 ? -(int32_t)n : n; }

bool board_orientation_update(BoardOrientationPolicy *s, bool ok,
    int16_t x, int16_t y, int16_t z, uint64_t now)
{
    if (!s) return false;
    const int32_t ax=magnitude(x), ay=magnitude(y), az=magnitude(z);
    const int32_t major=ax>ay ? ax : ay, minor=ax>ay ? ay : ax;
    /* Require a near-upright device, a decisive axis and plausible gravity.
     * The 0.25g axis margin is angular hysteresis; 400ms is the dwell. */
    if (!ok || major<4915 || major>12288 || az>5734 ||
        major-minor<2048 || ax+ay+az>18022) {
        s->pending=false;
        return false;
    }
    /* Sensor-to-logical-display basis: +Y => portrait, +X => 90 degrees.
     * Physical four-pose acceptance remains required; these are LVGL rotations,
     * not direct panel MADCTL commands or an observed hardware result. */
    const Orientation pose=ay>ax ? (y>0 ? ORIENTATION_0 : ORIENTATION_180) :
                                  (x>0 ? ORIENTATION_90 : ORIENTATION_270);
    if (s->valid && pose==s->accepted) { s->pending=false; return false; }
    if (!s->pending || pose!=s->candidate || now<s->since_ms) {
        s->candidate=pose; s->since_ms=now; s->pending=true; return false;
    }
    if (now-s->since_ms<400) return false;
    s->accepted=pose; s->valid=true; s->pending=false;
    return true;
}
