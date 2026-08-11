#ifndef AT_CMD_H
#define AT_CMD_H

#include "esp_err.h"

/*
 * AT command parser on UART0.
 *
 * Commands (terminated with \r\n):
 *   AT                 - test connection
 *   AT+RST             - restart
 *   AT+GMR             - show version
 *   AT+HELP            - command list
 *   AT+WIFI?           - show WiFi settings
 *   AT+WIFI=ssid,pwd   - set WiFi (auto-save, immediate)
 *   AT+PORT?           - show service port
 *   AT+PORT=n          - set service port (auto-save, restart required)
 *   AT+TXPWR?          - show TX power
 *   AT+TXPWR=n         - set TX power in dBm (see AT+TXPWR? for range, auto-save)
 *   AT+RATE?           - show sample rate
 *   AT+RATE=n          - set 8000/11025/16000/22050/32000/44100/48000
 *   AT+BITS?           - show I2S bits (16 or 24)
 *   AT+BITS=16|24      - set bits (auto-save, AT+HOTRESTART to apply)
 *   AT+FMT?            - show I2S comm format
 *   AT+FMT=0|1         - 0=Philips 1=LSB (auto-save, AT+HOTRESTART to apply)
 *   AT+CH?             - show channel format
 *   AT+CH=0|1|2        - 0=left 1=right 2=stereo (auto-save, AT+HOTRESTART to apply)
 *   AT+STATUS          - full device status
 *   AT+FACTORY         - factory reset (restart required)
 *   AT+HOST?          - show hostname
 *   AT+HOST=name      - set hostname (1-23 chars, auto-save, AT+RST to apply)
 *   AT+GAIN?          - show digital gain
 *   AT+GAIN=n         - set gain 0-64 (0=bypass, auto-save, AT+HOTRESTART to apply)
 *   AT+AGC?           - show AGC mode
 *   AT+AGC=0..8       - set AGC preset (0=OFF, 1..8=presets, auto-save, AT+HOTRESTART to apply)
 *   AT+CODEC?         - show codec
 *   AT+CODEC=0|1      - 0=ADPCM 1=PCM (auto-save, AT+HOTRESTART to apply)
 *   AT+XPORT?         - show transport mode
 *   AT+XPORT=0|1|2    - 0=UDP 1=TCP 2=RawTX (auto-save, AT+HOTRESTART/RST)
 *   AT+WCH?           - show WiFi channel (RawTX)
 *   AT+WCH=1..14      - set WiFi channel (auto-save, AT+HOTRESTART to apply)
 *   AT+TIMING?        - show I2S timing delays
 *   AT+TIMING=sd,ws,bck - set I2S RX delays 0..3 each (auto-save, AT+HOTRESTART to apply)
 *   AT+BATT?          - show battery status (if BATTERY_ENABLED)
 *   AT+HOTRESTART     - restart stream with current config (no reboot)
 *   AT+LOG=0|1        - mute/restore ESP_LOG output (debug only)
 */

esp_err_t at_cmd_init(void);

#endif /* AT_CMD_H */
