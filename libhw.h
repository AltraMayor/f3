#ifndef HEADER_LIBHW_H
#define HEADER_LIBHW_H

#include <stdbool.h>

int hw_open_com(char *device);
int hw_close_com();
int hw_disconnect(bool state);
void hw_toggle();

#endif  /* HEADER_LIBHW_H */
