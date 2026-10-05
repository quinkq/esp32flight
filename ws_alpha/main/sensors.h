#pragma once

#include "ws_link.h"

/* Measures with both sensors and fills the sht_* and bme_* fields of rec.
 * A sensor that doesn't answer gets missing values and its WS_REC_*_OK
 * flag cleared; nothing aborts. */
void sensors_read(ws_link_record_t *rec);
