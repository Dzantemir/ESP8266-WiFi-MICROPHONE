' ============================================================================
'  heartbeat.inc — Heartbeat thread (discovery + stall detection)
' ============================================================================
#IF NOT %DEF(%HEARTBEAT_INC)
%HEARTBEAT_INC = 1

'  HEARTBEAT THREAD
' ============================================================================
THREAD FUNCTION HeartbeatThread(BYVAL param AS DWORD) AS DWORD
    LOCAL idx AS LONG
    LOCAL tick AS DWORD
    LOCAL elapsed AS DWORD
    LOCAL hbPktRecv AS DWORD
    LOCAL hbIter AS LONG          ' DIAG: iteration counter
    LOCAL hbSentThisIter AS LONG  ' DIAG: how many DISCOVER sent this iteration

    ' The arrays persist between iterations, but the count variables are
    ' explicitly reset to 0 inside the loop, so semantics are unchanged.
    DIM hbTargets(0 TO %MAX_DEVICES - 1) AS LOCAL DWORD   ' IP
    DIM hbPorts(0 TO %MAX_DEVICES - 1) AS LOCAL DWORD    ' Port
    DIM hbCount AS LOCAL LONG
    DIM rsIPs(0 TO %MAX_DEVICES - 1) AS LOCAL DWORD
    DIM rsPorts(0 TO %MAX_DEVICES - 1) AS LOCAL DWORD
    DIM rsAudioPorts(0 TO %MAX_DEVICES - 1) AS LOCAL DWORD
    DIM rsSlots(0 TO %MAX_DEVICES - 1) AS LOCAL LONG
    DIM rsIdxs(0 TO %MAX_DEVICES - 1) AS LOCAL LONG
    DIM rsReasons(0 TO %MAX_DEVICES - 1) AS LOCAL STRING
    DIM rsCount AS LOCAL LONG

    AddLog "[HB] HeartbeatThread started (interval=" & TRIM$(STR$(%HB_INTERVAL)) & "ms)"

    DO WHILE g_bRunning AND ISFALSE g_bShuttingDown
        LOCAL hbSlept AS LONG
        hbSlept = 0
        DO WHILE hbSlept < %HB_INTERVAL
            IF g_bRunning = 0 THEN EXIT DO
            SLEEP 50
            hbSlept = hbSlept + 50
        LOOP
        IF g_bRunning = 0 THEN EXIT DO

        INCR hbIter
        hbSentThisIter = 0

        ' Collect (IP,Port) pairs under CS; send OUTSIDE CS (UDP SEND can block).
        hbCount = 0

        ' Collect ALL devices needing CONFIGURE resend.
        rsCount = 0

        EnterCriticalSection g_csDev
        tick = GetTickCount()

        FOR idx = 0 TO %MAX_DEVICES - 1
            IF g_Devs(idx).dwActive = 0 THEN ITERATE FOR

            IF g_Devs(idx).dwHBActive THEN
                ' 3-watchdog: Check if AudioThread died after dwHBActive=1 was set.
                ' StartStream checks for immediate death, but a thread that dies
                ' 100-500ms later (waveOutOpen fail after UDP OPEN ok) leaves
                ' dwHBActive=1 with a dead handle. UI shows Streaming but no audio.
                ' WaitForSingleObject(h, 0) = WAIT_OBJECT_0 means thread has exited.
                IF g_Devs(idx).hAudioThread THEN
                    IF WaitForSingleObject(g_Devs(idx).hAudioThread, 0) = %WAIT_OBJECT_0 THEN
                        ' Thread is dead - clear streaming status (do NOT close handle;
                        ' StopStream/StopAllStreams may still need to wait on it).
                        g_Devs(idx).dwHBActive = 0
                        g_Devs(idx).dwStatus   = %STS_IDLE
                        g_uiDirty = 1
                        AddLog "[HB] dev #" & TRIM$(STR$(idx)) & _
                               ": AudioThread died (post-start) - clearing streaming status"
                        ITERATE FOR   ' skip DISCOVER/stall check for this dead device
                    END IF
                END IF

                ' Collect target for DISCOVER heartbeat OUTSIDE CS
                hbTargets(hbCount) = g_Devs(idx).dwIP
                hbPorts(hbCount)   = g_Devs(idx).dwPort
                INCR hbCount

                ' ---- Stall detection: decide if this device needs a CONFIGURE resend ----
                hbPktRecv = g_Devs(idx).dwPktRecv
                IF hbPktRecv = 0 THEN
                    ' Case (a): never received any audio since stream start.
                    ' Initial CONFIGURE was probably lost in WiFi.
                    elapsed = tick - g_Devs(idx).dwStreamStart
                    IF elapsed > %NO_AUDIO_RESEND_MS THEN
                        rsIPs(rsCount)        = g_Devs(idx).dwIP
                        rsPorts(rsCount)      = g_Devs(idx).dwPort
                        rsAudioPorts(rsCount) = g_Devs(idx).dwAudioPort
                        rsSlots(rsCount)      = g_Devs(idx).dwDiscSlot
                        rsIdxs(rsCount)       = idx
                        rsReasons(rsCount)    = "no audio " & TRIM$(STR$(elapsed)) & _
                                                "ms after start (initial CONFIGURE lost?)"
                        INCR rsCount
                    END IF
                ELSE
                    ' Case (b): stream was running, then packets stopped.
                    ' dwLastPktTick is updated on every received audio packet
                    ' (AudioThread stats block). If it hasn't moved for
                    ' %STREAM_STALL_MS, the ESP is either unreachable (WiFi
                    ' still down) or has watchdog-timed-out to IDLE. Resend
                    ' CONFIGURE so that whenever the ESP comes back online it
                    ' resumes streaming. Harmless if ESP is still STREAMING
                    ' (same-dest CONFIGURE just resets its watchdog).
                    elapsed = tick - g_Devs(idx).dwLastPktTick
                    IF elapsed > %STREAM_STALL_MS THEN
                        rsIPs(rsCount)        = g_Devs(idx).dwIP
                        rsPorts(rsCount)      = g_Devs(idx).dwPort
                        rsAudioPorts(rsCount) = g_Devs(idx).dwAudioPort
                        rsSlots(rsCount)      = g_Devs(idx).dwDiscSlot
                        rsIdxs(rsCount)       = idx
                        rsReasons(rsCount)    = "stream stalled - no audio for " & _
                                                TRIM$(STR$(elapsed)) & _
                                                "ms (WiFi drop? resending CONFIGURE)"
                        INCR rsCount
                    END IF
                END IF
            ELSE
                elapsed = tick - g_Devs(idx).dwLastSeen
                IF elapsed > %DEV_TIMEOUT_MS THEN
                    g_Devs(idx).dwActive = 0
                    g_uiDirty = 1   ' trigger full ListView rebuild
                END IF
            END IF
        NEXT idx

        LeaveCriticalSection g_csDev

        ' Send DISCOVER heartbeats OUTSIDE CS (UDP SEND can block).
        ' Guard each send: if shutdown started, abort the loop - PBMAIN is
        ' about to close the discovery sockets underneath us.
        FOR idx = 0 TO hbCount - 1
            IF g_bRunning = 0 OR g_bShuttingDown THEN EXIT FOR
            SendDiscover hbTargets(idx), hbPorts(idx)
            INCR hbSentThisIter
        NEXT idx

        ' Log every heartbeat iter when streaming (gated by g_bVerbose), every 5th when idle.
        IF hbSentThisIter > 0 THEN
            IF g_bVerbose THEN
                AddLog "[HB] iter=" & TRIM$(STR$(hbIter)) & _
                       " sent " & TRIM$(STR$(hbSentThisIter)) & " DISCOVER"
            END IF
        ELSEIF (hbIter MOD 5) = 0 THEN
            AddLog "[HB] iter=" & TRIM$(STR$(hbIter)) & " idle (no HBActive devices)"
        END IF

        ' Resend CONFIGURE to ALL stalled devices OUTSIDE CS (UDP SEND can block).
        ' Guard: if shutdown started, abort - PBMAIN is closing sockets.
        FOR idx = 0 TO rsCount - 1
            IF g_bRunning = 0 OR g_bShuttingDown THEN EXIT FOR
            AddLog "[HB] dev #" & TRIM$(STR$(rsIdxs(idx))) & ": " & rsReasons(idx)
            SendConfigure rsIPs(idx), rsPorts(idx), rsAudioPorts(idx), rsSlots(idx)
        NEXT idx
    LOOP

    FUNCTION = 0
END FUNCTION

#ENDIF
