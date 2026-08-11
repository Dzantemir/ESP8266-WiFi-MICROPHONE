' ============================================================================
'  discovery.inc — Multi-port UDP discovery
' ============================================================================
#IF NOT %DEF(%EASSP_DISCOVERY_INC)
%EASSP_DISCOVERY_INC = 1

' DiscGetPrimaryFNum - returns first open port's fNum, or 0 if none open.
' Used by SendConfigure/SendStop which only need ONE socket to send through
' (the device will reply to whatever port the request came from, but for
' unicast CONFIGURE/STOP we don't care which socket emits the packet).
FUNCTION DiscGetPrimaryFNum() AS LONG
    LOCAL i AS LONG
    FOR i = 0 TO %MAX_DISC_PORTS - 1
        IF g_DiscPorts(i).isOpen THEN
            FUNCTION = g_DiscPorts(i).fNum
            EXIT FUNCTION
        END IF
    NEXT i
    FUNCTION = 0
END FUNCTION

' DiscAnyOpen - returns 1 if any discovery port is open, 0 otherwise.
FUNCTION DiscAnyOpen() AS LONG
    LOCAL i AS LONG
    FOR i = 0 TO %MAX_DISC_PORTS - 1
        IF g_DiscPorts(i).isOpen THEN
            FUNCTION = 1
            EXIT FUNCTION
        END IF
    NEXT i
    FUNCTION = 0
END FUNCTION

' DiscOpenPort - open a single discovery port, returns 1 on success.
' Finds first free slot, validates not already open, FREEFILE+UDP OPEN under g_csFile.
FUNCTION DiscOpenPort(BYVAL portNum AS LONG) AS LONG
    LOCAL i AS LONG, slot AS LONG

    ' Find free slot
    slot = -1
    FOR i = 0 TO %MAX_DISC_PORTS - 1
        IF g_DiscPorts(i).isOpen = 0 THEN
            slot = i
            EXIT FOR
        END IF
    NEXT i
    IF slot < 0 THEN
        ' No free slots - caller will log "FAILED (in use or no slots)"
        FUNCTION = 0
        EXIT FUNCTION
    END IF

    ' Check for duplicate port (already open in another slot)
    FOR i = 0 TO %MAX_DISC_PORTS - 1
        IF g_DiscPorts(i).isOpen AND g_DiscPorts(i).port = portNum THEN
            FUNCTION = 0   ' already open
            EXIT FUNCTION
        END IF
    NEXT i

    EnterCriticalSection g_csFile
    g_DiscPorts(slot).fNum = FREEFILE
    ERRCLEAR
    ' TIMEOUT 2000: only matters for rare FD_READ/RECV race (data already buffered when FD_READ fires).
    UDP OPEN PORT portNum AS #g_DiscPorts(slot).fNum TIMEOUT 2000
    IF ERR THEN
        LeaveCriticalSection g_csFile
        g_DiscPorts(slot).fNum = 0
        FUNCTION = 0
        EXIT FUNCTION
    END IF
    LeaveCriticalSection g_csFile

    g_DiscPorts(slot).port   = portNum
    g_DiscPorts(slot).isOpen = 1
    INCR g_DiscPortCount
    g_sbDirty = 1   ' trigger statusbar recompute

    ' Each port gets its own WM message so MainDlgProc's CASE range can
    ' dispatch FD_READ to the right socket via DiscoveryProc(slot).
    UDP NOTIFY #g_DiscPorts(slot).fNum, RECV TO g_hDlg AS (%WM_UDP_DISC_BASE + slot)

    FUNCTION = 1
END FUNCTION

' DiscClosePortEx - close a discovery port by slot index, return old port.
' Returns the port number that was closed (0 if slot was empty/invalid).
' SAFE for logging: caller captures the return value BEFORE the port field
' is zeroed. DiscClosePort is now a thin wrapper that discards the result.
FUNCTION DiscClosePortEx(BYVAL slotIdx AS LONG) AS LONG
    LOCAL oldPort AS LONG
    IF slotIdx < 0 OR slotIdx >= %MAX_DISC_PORTS THEN FUNCTION = 0 : EXIT FUNCTION
    IF g_DiscPorts(slotIdx).isOpen = 0 THEN FUNCTION = 0 : EXIT FUNCTION
    oldPort = g_DiscPorts(slotIdx).port
    ' 1.3-fix: serialize CLOSE with FREEFILE (PB file table is process-global)
    EnterCriticalSection g_csFile
    ERRCLEAR
    UDP CLOSE #g_DiscPorts(slotIdx).fNum
    LeaveCriticalSection g_csFile
    g_DiscPorts(slotIdx).isOpen = 0
    g_DiscPorts(slotIdx).fNum   = 0
    g_DiscPorts(slotIdx).port   = 0
    DECR g_DiscPortCount
    g_sbDirty = 1   ' trigger statusbar recompute
    FUNCTION = oldPort
END FUNCTION

' DiscClosePort - backwards-compatible wrapper around DiscClosePortEx.
' Discards the returned port number. New code should call DiscClosePortEx
' directly so it can log the port BEFORE the slot is zeroed.
SUB DiscClosePort(BYVAL slotIdx AS LONG)
    LOCAL p AS LONG
    p = DiscClosePortEx(slotIdx)
END SUB

' DiscCloseAll - close all open discovery ports.
' Called from WM_CLOSE and PBMAIN cleanup. Safe to call anytime - iterates
' all slots and closes any that are open.
SUB DiscCloseAll()
    LOCAL i AS LONG
    FOR i = 0 TO %MAX_DISC_PORTS - 1
        IF g_DiscPorts(i).isOpen THEN DiscClosePort i
    NEXT i
END SUB

' DiscSendAll - send a packet via ALL open discovery ports.
' Used by SendDiscover for broadcast DISCOVER: the same packet goes out on
' every open socket so devices listening on any of our ports can hear it.
' (SendConfigure/SendStop are unicast and only need one socket - they use
' DiscFNumBySlot / DiscSendOne instead.)
SUB DiscSendAll(BYVAL targetIP AS DWORD, BYVAL targetPort AS DWORD, BYVAL sndBuf AS STRING)
    LOCAL i AS LONG
    ' Defense in depth: if shutdown is in progress, PBMAIN may be closing
    ' the discovery sockets. Skip the send entirely (HB loop also guards).
    IF g_bShuttingDown THEN EXIT SUB
    FOR i = 0 TO %MAX_DISC_PORTS - 1
        IF g_DiscPorts(i).isOpen THEN
            ERRCLEAR
            UDP SEND #g_DiscPorts(i).fNum, AT targetIP, targetPort, sndBuf
        END IF
    NEXT i
END SUB

' DiscFNumBySlot - return the fNum for a known discovery slot.
' Falls back to DiscGetPrimaryFNum if the slot is out of range or closed.
' Used by DiscSendOne so SendConfigure/SendStop can reply out the SAME socket
' the device's INFO arrived on (matters for NAT/guest-WiFi which may block
' packets from a "foreign" source port).
FUNCTION DiscFNumBySlot(BYVAL slot AS LONG) AS LONG
    IF slot >= 0 AND slot < %MAX_DISC_PORTS THEN
        IF g_DiscPorts(slot).isOpen THEN
            FUNCTION = g_DiscPorts(slot).fNum
            EXIT FUNCTION
        END IF
    END IF
    FUNCTION = DiscGetPrimaryFNum()
END FUNCTION

' DiscSendOne - unicast send via ONE socket (prefer the device's discovery slot).
' slot = -1 / out-of-range / closed  -> fall back to DiscGetPrimaryFNum.
SUB DiscSendOne(BYVAL slot AS LONG, BYVAL targetIP AS DWORD, _
                BYVAL targetPort AS DWORD, BYVAL sndBuf AS STRING)
    LOCAL f AS LONG
    ' Defense in depth: if shutdown is in progress, PBMAIN may be closing
    ' the discovery sockets. Skip the send entirely (HB loop also guards).
    IF g_bShuttingDown THEN EXIT SUB
    f = DiscFNumBySlot(slot)
    IF f > 0 THEN
        ERRCLEAR
        UDP SEND #f, AT targetIP, targetPort, sndBuf
    END IF
END SUB

' SaveDiscPortsToIni - persist the current set of open discovery ports to
' INI [discovery] ports=3950,3951,...  Called after Add/Remove (currently
' only Add is wired in the UI). On next startup, InitDiscovery reads this
' key and re-opens the same ports.
SUB SaveDiscPortsToIni()
    LOCAL i AS LONG
    LOCAL sPorts AS STRING
    FOR i = 0 TO %MAX_DISC_PORTS - 1
        IF g_DiscPorts(i).isOpen THEN
            IF LEN(sPorts) > 0 THEN sPorts = sPorts & ","
            sPorts = sPorts & TRIM$(STR$(g_DiscPorts(i).port))
        END IF
    NEXT i
    WritePrivateProfileString "discovery", "ports", BYCOPY sPorts, BYCOPY g_sIniFile
END SUB

' InitDiscovery - Open UDP sockets for EASSP discovery.
' Reads port list from INI [discovery] ports=3950,3951,... (default "3950").
FUNCTION InitDiscovery() AS LONG
    LOCAL i AS LONG
    LOCAL sPorts AS STRING, sPort AS STRING
    LOCAL comma AS LONG, nPort AS LONG, opened AS LONG
    LOCAL sPortsBuf AS STRINGZ * 256

    ' Read ports from INI (default: 3950)
    ' 4.3-fix: use $DISC_PORT_STR constant instead of literal
    GetPrivateProfileString "discovery", "ports", $DISC_PORT_STR, _
                             BYREF sPortsBuf, 256, BYCOPY g_sIniFile
    sPorts = TRIM$(sPortsBuf, ANY CHR$(0, 32))

    ' Parse comma-separated ports and open each
    opened = 0
    DO WHILE LEN(sPorts) > 0
        comma = INSTR(sPorts, ",")
        IF comma > 0 THEN
            sPort  = LEFT$(sPorts, comma - 1)
            sPorts = MID$(sPorts, comma + 1)
        ELSE
            sPort  = sPorts
            sPorts = ""
        END IF
        sPort = TRIM$(sPort)
        IF LEN(sPort) > 0 THEN
            nPort = VAL(sPort)
            IF nPort > 0 AND nPort < 65536 THEN
                IF DiscOpenPort(nPort) THEN
                    INCR opened
                    AddLog "[INI] Discovery port " & sPort & " opened"
                ELSE
                    AddLog "[INI] Discovery port " & sPort & " FAILED (in use or no slots)"
                END IF
            ELSE
                AddLog "[INI] Discovery port " & sPort & " INVALID (must be 1..65535)"
            END IF
        END IF
    LOOP

    IF opened > 0 THEN
        AddLog "EASSP Server started. Listening on " & TRIM$(STR$(opened)) & " UDP port(s)"
    END IF
    FUNCTION = opened
END FUNCTION

' DiscoveryProc - Process incoming EASSP INFO packets
SUB DiscoveryProc(BYVAL portIdx AS LONG)
    LOCAL recvBuf AS STRING
    LOCAL fromIP AS DWORD
    LOCAL fromPort AS DWORD
    LOCAL tmpMac AS STRING
    LOCAL i AS LONG

    ' Shutdown guard: WM_UDP_DISC handler already drops late FD_READ, but
    ' guard here too in case a packet was already queued before the flag
    ' was set. Do NOT call UDP RECV during teardown - sockets may be gone.
    IF g_bShuttingDown THEN EXIT SUB
    IF portIdx < 0 OR portIdx >= %MAX_DISC_PORTS THEN EXIT SUB
    IF g_DiscPorts(portIdx).isOpen = 0 THEN EXIT SUB

    ERRCLEAR
    UDP RECV #g_DiscPorts(portIdx).fNum, FROM fromIP, fromPort, recvBuf

    IF ERR THEN EXIT SUB
    ' Accept INFO payloads v2.0 (34B) through v2.2 (58B); each field has bufLen guard.
    IF LEN(recvBuf) < %EASSP_HDR_SZ + 32 THEN EXIT SUB

    ' Check magic bytes
    IF PEEK(BYTE, STRPTR(recvBuf)) <> %EASSP_MAGIC0 THEN EXIT SUB
    IF PEEK(BYTE, STRPTR(recvBuf) + 1) <> %EASSP_MAGIC1 THEN EXIT SUB
    IF PEEK(BYTE, STRPTR(recvBuf) + 2) <> %EASSP_VER THEN EXIT SUB
    IF PEEK(BYTE, STRPTR(recvBuf) + 3) <> %CMD_INFO THEN EXIT SUB

    ' Format MAC from bytes 17..22 (svc_info_payload: offset 9..14)
    tmpMac = ""
    FOR i = 17 TO 22
        tmpMac = tmpMac & RIGHT$("0" & HEX$(PEEK(BYTE, STRPTR(recvBuf) + i)), 2)
        IF i < 22 THEN tmpMac = tmpMac & ":"
    NEXT i

    ' Update device, passing our listen slot so CONFIGURE/STOP route out
    ' the SAME socket the device's INFO arrived on.
    UpdateDevice tmpMac, fromIP, fromPort, STRPTR(recvBuf), LEN(recvBuf), portIdx
END SUB


#ENDIF
