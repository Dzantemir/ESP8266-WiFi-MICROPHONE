' ============================================================================
'  net_cmd.inc — EASSP protocol commands (DISCOVER/CONFIGURE/STOP)
' ============================================================================
#IF NOT %DEF(%EASSP_NET_CMD_INC)
%EASSP_NET_CMD_INC = 1

' SendDiscover - Send DISCOVER command (heartbeat)
SUB SendDiscover(BYVAL targetIP AS DWORD, BYVAL targetPort AS DWORD)
    LOCAL sndBuf AS STRING
    LOCAL seq AS LONG
    LOCAL sndErr AS LONG

    sndBuf = CHR$(%EASSP_MAGIC0, %EASSP_MAGIC1, %EASSP_VER, %CMD_DISCOVER)
    seq = InterlockedIncrement(g_seqCnt)   ' capture return atomically
    sndBuf = sndBuf & CHR$(LO(BYTE, seq), HI(BYTE, seq), 0, 0)

    ' Broadcasts via ALL open discovery sockets.
    IF DiscAnyOpen() THEN
        ERRCLEAR
        DiscSendAll targetIP, targetPort, sndBuf
        sndErr = ERR
        IF sndErr THEN
            AddLog "[HB] SendDiscover FAILED err=" & TRIM$(STR$(sndErr)) & _
                   " to " & FormatIP(targetIP) & ":" & TRIM$(STR$(targetPort))
        END IF
    ELSE
        AddLog "[HB] SendDiscover SKIP - no discovery ports open"
    END IF
END SUB

' SendConfigure - Send CONFIGURE command to device
'
' PROTOCOL PRINCIPLE: Receiver NEVER dictates audio params. CONFIGURE only
' tells the device WHERE to send audio (stream_port); device is the audio
' authority. Server learns format from INFO and adapts WaveOut.
'
' discSlot (optional, default -1): route the unicast CONFIGURE out the SAME
' discovery socket the device's last INFO arrived on. NAT/guest WiFi may
' drop packets from a "foreign" source port. -1 / out-of-range falls back
' to the primary socket via DiscFNumBySlot.
SUB SendConfigure(BYVAL targetIP AS DWORD, BYVAL targetPort AS DWORD, _
                  BYVAL streamPort AS DWORD, BYVAL discSlot AS LONG)
    LOCAL sndBuf AS STRING
    LOCAL nPort AS WORD
    LOCAL seq AS LONG
    LOCAL slot AS LONG

    slot = discSlot

    sndBuf = CHR$(%EASSP_MAGIC0, %EASSP_MAGIC1, %EASSP_VER, %CMD_CONFIGURE)
    seq = InterlockedIncrement(g_seqCnt)   ' capture return atomically
    sndBuf = sndBuf & CHR$(LO(BYTE, seq), HI(BYTE, seq))
    sndBuf = sndBuf & CHR$(LO(BYTE, %CFG_PAYLOAD_SZ), HI(BYTE, %CFG_PAYLOAD_SZ))

    ' Stream port (2 bytes, network byte order) - the ONLY field in payload.
    ' NOTE: Avoid CINT() - it overflows for ports >= 32768
    nPort = htons(streamPort AND &HFFFF&)
    sndBuf = sndBuf & MKI$(nPort)

    ' 3.10-fix: log send errors (SendDiscover/SendStop already do)
    ERRCLEAR
    DiscSendOne slot, targetIP, targetPort, sndBuf
    IF ERR THEN
        AddLog "[CFG] SendConfigure FAILED err=" & TRIM$(STR$(ERR)) & _
               " to " & FormatIP(targetIP) & ":" & TRIM$(STR$(targetPort))
    END IF
END SUB

' SendStop - Send STOP command (explicit immediate stop)
'
' Packet format: [EASSP header 8 bytes, payload_len=0]
'   magic[2]=EA 55, version=01, cmd=03, seq[2], payload_len[2]=00 00
'
' discSlot (optional, default -1): see SendConfigure for rationale.
SUB SendStop(BYVAL targetIP AS DWORD, BYVAL targetPort AS DWORD, _
             BYVAL discSlot AS LONG)
    LOCAL sndBuf AS STRING
    LOCAL seq AS LONG
    LOCAL slot AS LONG

    slot = discSlot

    sndBuf = CHR$(%EASSP_MAGIC0, %EASSP_MAGIC1, %EASSP_VER, %CMD_STOP)
    seq = InterlockedIncrement(g_seqCnt)   ' capture return atomically
    sndBuf = sndBuf & CHR$(LO(BYTE, seq), HI(BYTE, seq), 0, 0)

    IF DiscAnyOpen() = 0 THEN
        AddLog "[STOP] SendStop SKIP - no discovery ports open"
        EXIT SUB
    END IF
    ERRCLEAR
    DiscSendOne slot, targetIP, targetPort, sndBuf
    IF ERR THEN
        AddLog "[STOP] SendStop FAILED err=" & TRIM$(STR$(ERR)) & _
               " to " & FormatIP(targetIP) & ":" & TRIM$(STR$(targetPort))
    END IF
END SUB

' SendStopForced - Send STOP command during shutdown.
'
' Identical to SendStop but BYPASSES the g_bShuttingDown guard in DiscSendOne.
' During shutdown WM_CLOSE sets g_bShuttingDown=1 first (to block HB from sending
' stray CONFIGURE that would restart the stream after our STOP). But that guard
' also blocks our own SendStop. This function calls DiscFNumBySlot + UDP SEND
' directly, so CMD_STOP reaches the device even during shutdown.
'
' SAFE because:
'  - Called BEFORE DiscCloseAll (sockets still open)
'  - DiscFNumBySlot checks isOpen (returns 0 if slot closed)
'  - f>0 check before UDP SEND
'  - No AddLog on error (g_bShuttingDown=1 makes it a no-op anyway)
SUB SendStopForced(BYVAL targetIP AS DWORD, BYVAL targetPort AS DWORD, _
                   BYVAL discSlot AS LONG)
    LOCAL sndBuf AS STRING
    LOCAL seq AS LONG
    LOCAL slot AS LONG
    LOCAL f AS LONG

    slot = discSlot

    sndBuf = CHR$(%EASSP_MAGIC0, %EASSP_MAGIC1, %EASSP_VER, %CMD_STOP)
    seq = InterlockedIncrement(g_seqCnt)
    sndBuf = sndBuf & CHR$(LO(BYTE, seq), HI(BYTE, seq), 0, 0)

    ' Get socket fNum directly (bypasses DiscSendOne's g_bShuttingDown guard)
    f = DiscFNumBySlot(slot)
    IF f > 0 THEN
        ERRCLEAR
        UDP SEND #f, AT targetIP, targetPort, sndBuf
        ERRCLEAR
    END IF
END SUB

' ============================================================================

#ENDIF
