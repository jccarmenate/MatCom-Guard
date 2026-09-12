#include "guard_dial.h"

double guard_dial_compute_arc_fraction(int current, int total) {
    if (total <= 0) {
        return 0.0;
    }
    if (current <= 0) {
        return 0.0;
    }
    if (current >= total) {
        return 1.0;
    }
    return (double)current / (double)total;
}
