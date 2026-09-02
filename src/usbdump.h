/* usbdump.h — full USB descriptor tree dump (composition baseline). */
#pragma once
#include "usbh.h"

/* Print every interface (incl. alternate settings), endpoint and CDC
 * functional descriptor, plus the raw bytes. Diff two of these to see
 * exactly what a composition change did. */
void usbdump_config(const usbh_device_t *d);
