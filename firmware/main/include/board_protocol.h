#ifndef BOARD_PROTOCOL_H
#define BOARD_PROTOCOL_H

/* Packet format sizes and firmware version — fixed protocol constants
 * (NOT configurable via Kconfig).
 * Extracted from board_config.h (R3-C). */

#include "sdkconfig.h"
#include <stdint.h>
#include <stdbool.h>

/* ====================================================================
 *  Fixed Protocol Constants (NOT configurable)
 * ==================================================================== */

/*
 * UDP packet header (16 bytes, packed):
 *   seq_num(2) + timestamp_ms(4) + codec(1) +
 *   sample_rate_enum(1) + channels(1) + frame_ms(1) +
 *   bitrate(4) + bits(2)
 */
#define PKT_HDR_SIZE        16
#define DVI4_HEADER_SIZE    4        /* predict(2) + index(1) + reserved(1) */

/* Firmware version — single source of truth. Must match the protocol version:
 *   v2.0 = INFO payload 33 bytes (no transport_mode, no hostname)
 *   v2.1 = +1 byte transport_mode (34 bytes)
 *   v2.2 = +24 bytes hostname (58 bytes)  <-- current */
#define FIRMWARE_VERSION    "v2.2"

#endif /* BOARD_PROTOCOL_H */
