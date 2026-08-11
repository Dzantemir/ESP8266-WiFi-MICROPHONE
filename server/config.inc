' ============================================================================
'  EASSP Server - Constants and Equates
'  PowerBASIC 10 for Windows
'
'  Win32 system equates are defined in Win32API.INC. %INADDR_NONE and %FD_READ
'  require WINSOCK2.INC, which eassp_server.bas $INCLUDEs explicitly.
' ============================================================================

' ---- EASSP Protocol ----
%EASSP_MAGIC0     = &HEA
%EASSP_MAGIC1     = &H55
%EASSP_VER        = 1
%EASSP_HDR_SZ     = 8
%CMD_DISCOVER     = &H01
%CMD_CONFIGURE    = &H02
%CMD_STOP         = &H03   ' Explicit stop — no payload, device stops immediately
%CMD_INFO         = &H81
%STS_IDLE         = 0
%STS_STREAM       = 1
%STS_ERROR        = 2
%CFG_PAYLOAD_SZ   = 2
%PKT_HDR_SZ       = 16
%DVI4_HDR_SZ      = 4
%CODEC_ID         = 5
%CODEC_ID_PCM     = 6   ' 16-bit signed PCM (diagnostic, AT+RAW=1 on ESP)

' ---- App Constants ----
%MAX_DEVICES      = 16
%NUM_WAVE_BUFS    = 16
%WAVE_BUF_SZ      = 1920
%HB_INTERVAL      = 3000
%DEV_TIMEOUT_MS   = 30000
%REFRESH_MS       = 2000
%AUDIO_PORT_BASE  = 5000

' ---- Stream stall / WiFi-drop recovery ----
' HeartbeatThread resends CONFIGURE if no audio for this long.
%STREAM_STALL_MS  = 10000

' ---- "No audio after start" CONFIGURE resend threshold ----
' 5s avoids spurious resends during slow WiFi / cold boot startup.
%NO_AUDIO_RESEND_MS = 5000

' ---- Control IDs ----
%IDC_LISTVIEW     = 101
%IDC_LOG          = 106
%IDC_BTN_START    = 103
%IDC_BTN_STOP     = 104
%IDC_BTN_STOPALL  = 105
%IDC_BTN_DUMP     = 109
%IDC_STATUSBAR    = 108
%IDC_COMBO_DEVICE = 110   ' WaveOut device selection (VB-Cable, etc.)
%IDC_LBL_OUTPUT   = 111   ' "Output:" label for device combo

' ---- Timer / Menu ----
%IDT_REFRESH      = 1
%IDT_DEVCHANGE    = 2    ' Debounce timer for WM_DEVICECHANGE (wait for all audio devices to settle)
%DEVCHANGE_DEBOUNCE_MS = 2000   ' 2s after last WM_DEVICECHANGE before reopening WaveOut
%IDM_ABOUT        = &H1000   ' Must be multiple of 16 (WM_SYSCOMMAND uses low 4 bits)
%IDM_ADD_DEVICE   = &H1010
%IDM_ADD_PORT     = &H1020   ' Add Discovery Port... (system menu)

' ---- ListView context menu (right-click) ----
%IDM_CTX_START     = &H2001
%IDM_CTX_STOP      = &H2002
%IDM_CTX_SELECTALL = &H2003
%IDM_CTX_CLEARALL  = &H2004
%IDM_CTX_STOPALL   = &H2005

' ---- Add Device Dialog ----
%IDC_AD_IP        = 2001
%IDC_AD_PORT      = 2002

' ---- Discovery Ports Dialog ----
%IDC_DP_LISTVIEW   = 3001
%IDC_DP_ADD        = 3002
%IDC_DP_CLOSE      = 3003
%IDC_DP_DONE       = 3004
%IDC_DP_CHANGE     = 3005

' ---- UDP notification message ----
' Each open discovery port gets its own WM message: port slot i uses %WM_UDP_DISC_BASE + i.
%MAX_DISC_PORTS    = 8       ' max simultaneous discovery ports
%WM_UDP_DISC_BASE  = %WM_USER + 500   ' base; port slot i uses BASE+i

' ---- Async log message (PostMessage from worker threads) ----
%WM_APP_LOG       = %WM_USER + 600

' ---- PCM Ring Buffer / Jitter depth (ms) / Adaptive ----
' Jitter depth is in milliseconds; bytesPerMs computed from WaveOut rate/channels/bits.
' Adaptive is GROW-ONLY (no shrink); ring drops oldest on overflow.
%JITTER_INITIAL_MS = 120   ' Target ring depth (ms) at stream start / reopen
%JITTER_MIN_MS     = 80   ' Soft-refill threshold after first underrun (ms)
%JITTER_MAX_MS     = 200   ' Adaptive grow ceiling (ms) for bad WiFi
%JITTER_ADAPT_MS   = 30000 ' Adaptive grow check interval (ms)
%JITTER_UNDERRUN_THRESHOLD = 1  ' Underruns in interval to trigger grow
%RING_CAP_BYTES    = 262144   ' 256 KB ring capacity (HeapAlloc'd per device)

' ---- BWSOLA PLC (per Yeh et al. 2013) ----
' Bilateral Waveform Similarity Overlap-Add. Reconstructs lost packets using
' both previous and next good packets. Master switch: 1=BWSOLA, 0=simple frame-repeat.
%BWSOLA_PLC_ENABLE    = 1      ' master switch (0 = simple frame-repeat PLC)
%BWSOLA_PITCH_MIN     = 80     ' min pitch period (samples @ 48kHz, per-channel)
%BWSOLA_PITCH_MAX     = 400    ' max pitch period (samples @ 48kHz, per-channel)
%BWSOLA_VOICED_THRESH = 30     ' ACF threshold /100 (0.30) for voiced decision

' ---- Startup click suppression ----
' Skip first N ms to bypass INMP441 startup transient and stale I2S DMA data.
%STARTUP_SKIP_MS  = 1000

' Linear fade-in after skip completes; smooths silence→signal transition.
%FADE_IN_MS       = 1000

' ---- String Constants ----
$APP_TITLE        = "EASSP Server - ESP8266 WiFi Microphone"
$DISC_PORT_STR    = "3950"

' ---- ListView Column Indices ----
%LV_COL_MAC       = 0
%LV_COL_IP        = 1
%LV_COL_STATUS    = 2
%LV_COL_RATE      = 3
%LV_COL_BITS      = 4
%LV_COL_CH        = 5
%LV_COL_CODEC     = 6
%LV_COL_RSSI      = 7
%LV_COL_HEAP      = 8
%LV_COL_FW        = 9
%LV_COL_PKTS      = 10
%LV_COL_LOST      = 11
%LV_COL_DUR       = 12
