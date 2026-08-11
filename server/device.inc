' ============================================================================
'  device.inc — Device management (UpdateDevice)
' ============================================================================
#IF NOT %DEF(%DEVICE_INC)
%DEVICE_INC = 1

' UpdateDevice - Find or create device, update info
' discSlot: discovery port slot the INFO arrived on (-1 = unknown). Stored on
' the device so SendConfigure/SendStop can route the reply via DiscSendOne.
SUB UpdateDevice(BYVAL sMac AS STRING, BYVAL dwIP AS DWORD, BYVAL dwPort AS DWORD, _
                 BYVAL pBuf AS DWORD, BYVAL bufLen AS LONG, BYVAL discSlot AS LONG)
    LOCAL idx AS LONG
    LOCAL bNew AS LONG
    bNew = 0
    LOCAL tick AS DWORD
    LOCAL i AS LONG
    LOCAL b AS BYTE PTR
    LOCAL prevTransport AS LONG
    LOCAL bTransportChanged AS LONG
    LOCAL prevCodec AS LONG, prevCh AS LONG, prevBits AS LONG
    LOCAL prevRate AS DWORD, prevHost AS STRING
    LOCAL prevFw AS STRING
    LOCAL bFmtChanged AS LONG
    LOCAL saveTransport AS LONG
    prevTransport = -1
    prevCodec = -1
    prevCh    = -1
    prevBits  = -1
    prevRate  = 0
    bFmtChanged = 0

    b = pBuf

    ' ---- HARDENING: validate minimum INFO payload length ----
    ' Min 32 bytes (INFO v1) covers fields up to offset 31 (RSSI); reject shorter.
    IF bufLen < %EASSP_HDR_SZ + 32 THEN
        EXIT SUB
    END IF

    ' 2.5-fix: Pre-load INI output device preference (disk I/O) outside g_csDev.
    ' Assigned to g_Devs(idx).dwWaveDevice inside CS only for new devices.
    LOCAL iniWaveDev AS LONG
    iniWaveDev = LoadDeviceOutput(sMac)

    EnterCriticalSection g_csDev

    ' Search for existing device
    idx = -1
    FOR i = 0 TO %MAX_DEVICES - 1
        IF g_Devs(i).dwActive THEN
            IF TRIM$(g_Devs(i).sMac) = TRIM$(sMac) THEN
                idx = i
                EXIT FOR
            END IF
        END IF
    NEXT i

    ' Find empty slot
    IF idx = -1 THEN
        FOR i = 0 TO %MAX_DEVICES - 1
            IF g_Devs(i).dwActive = 0 THEN
                idx = i
                bNew = 1
                EXIT FOR
            END IF
        NEXT i
    END IF

    IF idx = -1 THEN
        LeaveCriticalSection g_csDev
        EXIT SUB
    END IF

    tick = GetTickCount()

    IF bNew THEN
        g_Devs(idx).dwActive = 1
        g_uiDirty = 1   ' trigger full ListView rebuild
        g_Devs(idx).sMac = sMac
        g_Devs(idx).dwDiscovered = tick
        ' 2.5-fix: LoadDeviceOutput does INI I/O - read BEFORE entering CS
        ' (was inside g_csDev, blocking all device access during disk read)
        g_Devs(idx).dwWaveDevice = iniWaveDev   ' 2.5-fix: loaded before CS
        g_Devs(idx).dwDiscSlot   = discSlot
        g_Devs(idx).bFmtDirty    = 1   ' new device: paint all static cols
    ELSE
        prevTransport = g_Devs(idx).dwTransport
        prevCodec     = g_Devs(idx).dwCodec
        prevCh        = g_Devs(idx).dwChannels
        prevBits      = g_Devs(idx).dwBits
        prevRate      = g_Devs(idx).dwSmpRate
        prevHost      = TRIM$(g_Devs(idx).sHostname)
        prevFw        = TRIM$(g_Devs(idx).sFirmware)
    END IF

    ' 3.5: Detect IP change for existing streaming device (TCP needs restart).
    LOCAL prevIP AS DWORD
    prevIP = 0
    IF bNew = 0 THEN prevIP = g_Devs(idx).dwIP
    g_Devs(idx).dwIP         = dwIP
    g_Devs(idx).dwPort       = dwPort
    ' Remember which discovery slot/port the device's INFO arrived on so
    ' SendConfigure/SendStop can reply via the SAME socket (NAT-safe).
    IF discSlot >= 0 AND discSlot < %MAX_DISC_PORTS THEN
        IF g_DiscPorts(discSlot).isOpen THEN
            g_Devs(idx).dwDiscSlot = discSlot
            g_Devs(idx).dwDiscPort = g_DiscPorts(discSlot).port
        END IF
    END IF
    ' 3.4: Don't let INFO downgrade dwStatus to IDLE while we're actively
    ' streaming (dwHBActive=1). ESP may briefly report IDLE during reboot
    ' but our AudioThread is still running. UI would flip to 'Idle'.
    IF g_Devs(idx).dwHBActive = 0 THEN
        g_Devs(idx).dwStatus     = @b[8]
    END IF
    g_Devs(idx).dwError      = @b[10]
    g_Devs(idx).dwChannels   = @b[11]      ' channels from INFO payload
    g_Devs(idx).dwCodec      = @b[9]       ' codec_id from INFO payload (5=ADPCM, 6=PCM)
    g_Devs(idx).dwSmpRate   = PEEK(DWORD, pBuf + 12)   ' offset 4 in payload
    ' Only present in INFO v2 (33-byte payload). Guard against old 32-byte INFO.
    IF bufLen >= %EASSP_HDR_SZ + 33 THEN
        g_Devs(idx).dwBits = PEEK(BYTE, pBuf + 40)   ' bits_per_sample
    END IF
    ' 3.6-fix: Don't clobber transport on short INFO for existing devices.
    IF bufLen >= %EASSP_HDR_SZ + 34 THEN
        g_Devs(idx).dwTransport = PEEK(BYTE, pBuf + 41)   ' transport_mode
    ELSE
        IF bNew THEN g_Devs(idx).dwTransport = 0   ' assume UDP for old firmware
    END IF

    ' ---- HARDENING: clamp out-of-range fields from ESP (parse boundary) ----
    LOCAL vCodec AS LONG, vCh AS LONG, vBits AS LONG, vRate AS DWORD, vTr AS LONG
    vCodec = g_Devs(idx).dwCodec
    vCh    = g_Devs(idx).dwChannels
    vBits  = g_Devs(idx).dwBits
    vRate  = g_Devs(idx).dwSmpRate
    vTr    = g_Devs(idx).dwTransport

    ' Codec: only 5 (ADPCM) and 6 (PCM) supported. Unknown -> default to ADPCM (5)
    IF vCodec <> %CODEC_ID AND vCodec <> %CODEC_ID_PCM THEN
        IF bNew THEN
            AddLog "[INFO] dev #" & TRIM$(STR$(idx)) & _
                   ": unsupported codec=" & TRIM$(STR$(vCodec)) & _
                   " -> defaulting to ADPCM (5)"
        END IF
        vCodec = %CODEC_ID
    END IF

    ' Channels: 1 (mono) or 2 (stereo) only. Clamp, don't reject (graceful).
    IF vCh < 1 THEN vCh = 1
    IF vCh > 2 THEN vCh = 2

    ' Bits: 16 or 24 only. Unknown -> 16 (safe default, always supported).
    IF vBits <> 16 AND vBits <> 24 THEN vBits = 16

    ' Sample rate: must be one of the 7 known EASSP rates. Unknown -> 48000.
    IF vRate <> 8000 AND vRate <> 11025 AND vRate <> 16000 AND _
       vRate <> 22050 AND vRate <> 32000 AND vRate <> 44100 AND _
       vRate <> 48000 THEN
        IF bNew THEN
            AddLog "[INFO] dev #" & TRIM$(STR$(idx)) & _
                   ": unsupported sample_rate=" & TRIM$(STR$(vRate)) & _
                   " -> defaulting to 48000"
        END IF
        vRate = 48000
    END IF

    ' Transport: 0 (UDP) or 1 (TCP) only. 2 (Raw 802.11 TX) cannot be received.
    ' Unknown/Raw -> default UDP; stall-detection will log if no audio arrives.
    IF vTr <> 0 AND vTr <> 1 THEN
        IF bNew AND vTr <> 0 THEN
            AddLog "[INFO] dev #" & TRIM$(STR$(idx)) & _
                   ": unsupported transport=" & TRIM$(STR$(vTr)) & _
                   " -> defaulting to UDP (0). Raw TX (2) cannot be received."
        END IF
        vTr = 0
    END IF

    g_Devs(idx).dwCodec     = vCodec
    g_Devs(idx).dwChannels  = vCh
    g_Devs(idx).dwBits      = vBits
    g_Devs(idx).dwSmpRate   = vRate
    g_Devs(idx).dwTransport = vTr

    IF bNew = 0 AND prevTransport >= 0 AND prevTransport <> vTr AND _
       g_Devs(idx).dwHBActive <> 0 THEN
        bTransportChanged = 1
    END IF

    ' 3.5: IP change during active TCP stream - AudioThread has stale devIP/tcpHost.
    ' Reuse the transport-change restart path to restart the audio thread
    ' so it picks up the new IP. Only matters for TCP (UDP RECV is IP-agnostic).
    IF bNew = 0 AND prevIP <> 0 AND prevIP <> dwIP AND _
       g_Devs(idx).dwHBActive <> 0 AND vTr = 1 THEN
        bTransportChanged = 1
    END IF

    ' Detect format / hostname / transport change for an EXISTING device so
    ' RefreshUI repaints the otherwise-static columns (rate/bits/ch/codec/fw/mac).
    ' New devices already have bFmtDirty=1 set above.
    ' Transport is shown in the CODEC column ("ADPCM/UDP" / "ADPCM/TCP"),
    ' so it MUST be in this check or the UI won't refresh on transport switch.
    IF bNew = 0 THEN
        IF vCodec <> prevCodec THEN bFmtChanged = 1
        IF vCh    <> prevCh    THEN bFmtChanged = 1
        IF vBits  <> prevBits  THEN bFmtChanged = 1
        IF vRate  <> prevRate  THEN bFmtChanged = 1
        IF vTr    <> prevTransport THEN bFmtChanged = 1
    END IF

    ' 3.6-fix: Don't clobber hostname on short INFO for existing devices.
    IF bufLen >= %EASSP_HDR_SZ + 58 THEN
        g_Devs(idx).sHostname = PEEK$(pBuf + 42, 24)
    ELSE
        IF bNew THEN g_Devs(idx).sHostname = ""
    END IF

    ' Hostname is also a "static" ListView cell (shown in the MAC column).
    IF bNew = 0 THEN
        IF TRIM$(g_Devs(idx).sHostname) <> prevHost THEN bFmtChanged = 1
    END IF

    g_Devs(idx).dwPktsSent   = PEEK(DWORD, pBuf + 23)   ' offset 15 in payload
    g_Devs(idx).dwFreeHeap   = PEEK(DWORD, pBuf + 27)   ' offset 19 in payload
    g_Devs(idx).dwLastSeen = tick

    LOCAL bRSSI AS LONG
    bRSSI = PEEK(BYTE, pBuf + 31)
    IF bRSSI > 127 THEN bRSSI = bRSSI - 256
    g_Devs(idx).dwRSSI = bRSSI

    ' Firmware (8 bytes at offset 32 in packet)
    g_Devs(idx).sFirmware = PEEK$(pBuf + 32, 8)
    IF bNew = 0 THEN
        IF TRIM$(g_Devs(idx).sFirmware) <> prevFw THEN bFmtChanged = 1
    END IF
    IF bFmtChanged THEN
        g_Devs(idx).bFmtDirty = 1
    END IF

    LOCAL saveHostname AS STRING
    LOCAL saveSmpRate AS DWORD
    saveHostname = g_Devs(idx).sHostname
    saveSmpRate  = g_Devs(idx).dwSmpRate
    saveTransport = g_Devs(idx).dwTransport

    LeaveCriticalSection g_csDev

    ' AddLog OUTSIDE CS - never hold CS during UI operations
    IF bNew THEN
        LOCAL sDevName AS STRING
        IF LEN(TRIM$(saveHostname)) > 0 THEN
            sDevName = TRIM$(saveHostname)
        ELSE
            sDevName = sMac
        END IF
        AddLog "New device: " & sDevName & " MAC=" & sMac & " IP=" & FormatIP(dwIP) & ":" & TRIM$(STR$(dwPort)) & _
               " rate=" & TRIM$(STR$(saveSmpRate)) & " Hz"
    END IF

    ' Per-device restart on transport switch (avoid dropping audio on unrelated streams).
    ' Also log transport change when NOT streaming so the user knows the new
    ' transport was stored and will take effect on next StartStream.
    IF bNew = 0 AND prevTransport >= 0 AND prevTransport <> vTr AND _
       g_Devs(idx).dwHBActive = 0 THEN
        LOCAL sOldTr2 AS STRING, sNewTr2 AS STRING
        IF prevTransport = 0 THEN sOldTr2 = "UDP" ELSE sOldTr2 = "TCP"
        IF saveTransport   = 0 THEN sNewTr2 = "UDP" ELSE sNewTr2 = "TCP"
        AddLog "[TRANSPORT] dev #" & TRIM$(STR$(idx)) & _
               ": ESP switched " & sOldTr2 & " -> " & sNewTr2 & _
               " (not streaming - will use " & sNewTr2 & " on next Start)"
    END IF
    IF bTransportChanged THEN
        LOCAL sOldTr AS STRING, sNewTr AS STRING
        LOCAL stopResult AS LONG
        IF prevTransport = 0 THEN sOldTr = "UDP" ELSE sOldTr = "TCP"
        IF saveTransport   = 0 THEN sNewTr = "UDP" ELSE sNewTr = "TCP"
        AddLog "[AUTOTRANSPORT] dev #" & TRIM$(STR$(idx)) & _
               ": ESP switched " & sOldTr & " -> " & sNewTr & _
               ", auto-restarting audio thread (per-device)"
        stopResult = StopStream(idx)
        IF stopResult = 0 THEN
            stopResult = StopStream(idx)   ' reap orphan handle, returns 1
        END IF
        IF stopResult = 0 THEN
            AddLog "[AUTOTRANSPORT] dev #" & TRIM$(STR$(idx)) & _
                   ": StopStream timed out twice - NOT restarting (manual restart needed)"
        ELSE
            SLEEP 200   ' brief pause for OS socket recycling
            StartStream idx
        END IF
    END IF
END SUB

#ENDIF
