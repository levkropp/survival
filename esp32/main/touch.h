/*
 * touch.h — XPT2046 resistive touchscreen driver (bit-banged SPI)
 */
#ifndef TOUCH_H
#define TOUCH_H

#include <stdbool.h>
#include <stdint.h>

/* Initialize XPT2046 GPIO pins. */
void touch_init(void);

/* Read current touch state. Returns true if touched.
 * x, y are screen coordinates (0..319, 0..239) after calibration. */
bool touch_read(int *x, int *y);

/* Block until a touch-down + release. Returns coordinates of the tap. */
void touch_wait_tap(int *x, int *y);

#endif /* TOUCH_H */
