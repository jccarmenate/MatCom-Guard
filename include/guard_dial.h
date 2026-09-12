#ifndef GUARD_DIAL_H
#define GUARD_DIAL_H

/**
 * Fracción de arco (0.0 a 1.0) que representa el progreso current/total,
 * usada para dibujar el dial de guardia.
 *
 * `total <= 0` (progreso indeterminado o inválido) devuelve 0.0.
 * `current` se satura al rango [0, total] antes de calcular la fracción,
 * así que el valor devuelto siempre está en [0.0, 1.0].
 */
double guard_dial_compute_arc_fraction(int current, int total);

#endif // GUARD_DIAL_H
