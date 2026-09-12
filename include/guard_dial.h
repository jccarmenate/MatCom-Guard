#ifndef GUARD_DIAL_H
#define GUARD_DIAL_H

#include <gtk/gtk.h>

/**
 * Fracción de arco (0.0 a 1.0) que representa el progreso current/total,
 * usada para dibujar el dial de guardia.
 *
 * `total <= 0` (progreso indeterminado o inválido) devuelve 0.0.
 * `current` se satura al rango [0, total] antes de calcular la fracción,
 * así que el valor devuelto siempre está en [0.0, 1.0].
 */
double guard_dial_compute_arc_fraction(int current, int total);

/** Crea un dial de guardia nuevo, listo para agregar a un contenedor. */
GtkWidget *guard_dial_new(void);

/**
 * Actualiza el progreso mostrado y fuerza un redibujado. `phase` se copia
 * internamente a un buffer propio — el puntero que se pasa no necesita
 * seguir siendo válido después de que esta función retorna. `phase` puede
 * ser NULL (se trata como cadena vacía).
 */
void guard_dial_set_progress(GtkWidget *dial, int current, int total, const char *phase);

#endif // GUARD_DIAL_H
