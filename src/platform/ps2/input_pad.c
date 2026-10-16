/*
 * Input backend for the Fallout PS2 port (F1: stub).
 *
 * F3 implementa aquí la lectura del DualShock 2 con libpad y la traducción a
 * eventos SDL sintéticos (ratón + teclado) usando los helpers de
 * sdl/ps2_sdl_internal.h. El mapeo aprobado es:
 *
 *   - Stick izquierdo: cursor (curva de aceleración suave)
 *   - Stick derecho:   scroll
 *   - Cruz: clic izquierdo; Círculo: clic derecho / modo cursor
 *   - L1/R1/L2/R2: arma activa / Pip-Boy / inventario / fin de turno
 */

#include "ps2_input.h"

void ps2_input_init(void)
{
}

void ps2_input_poll(void)
{
}
