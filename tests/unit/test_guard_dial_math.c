#include <assert.h>
#include <stdio.h>
#include "guard_dial.h"

int main(void) {
    // total == 0 significa progreso indeterminado: sin fracción que mostrar.
    assert(guard_dial_compute_arc_fraction(5, 0) == 0.0);
    assert(guard_dial_compute_arc_fraction(0, 0) == 0.0);

    // Casos normales.
    assert(guard_dial_compute_arc_fraction(0, 100) == 0.0);
    assert(guard_dial_compute_arc_fraction(50, 100) == 0.5);
    assert(guard_dial_compute_arc_fraction(100, 100) == 1.0);

    // Saturación: nunca se devuelve una fracción fuera de [0,1].
    assert(guard_dial_compute_arc_fraction(150, 100) == 1.0);
    assert(guard_dial_compute_arc_fraction(-10, 100) == 0.0);

    printf("test_guard_dial_math: OK\n");
    return 0;
}
