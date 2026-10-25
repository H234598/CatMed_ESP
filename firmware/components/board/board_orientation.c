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
    /* C6 QMI8658A mounting frame: the complete four-pose acceptance against
     * manual0 established a 180-degree offset from the former sensor basis.
     * Classify directly into display angles BEFORE all state comparisons:
     * candidate and accepted must never mix sensor and display frames.
     * Manual panel/touch angles and C3 do not use this sensor policy. */
    const Orientation pose=ay>ax ? (y>0 ? ORIENTATION_180 : ORIENTATION_0) :
                                  (x>0 ? ORIENTATION_270 : ORIENTATION_90);
    if (s->valid && pose==s->accepted) { s->pending=false; return false; }
    if (!s->pending || pose!=s->candidate || now<s->since_ms) {
        s->candidate=pose; s->since_ms=now; s->pending=true; return false;
    }
    if (now-s->since_ms<400) return false;
    s->accepted=pose; s->valid=true; s->pending=false;
    return true;
}
