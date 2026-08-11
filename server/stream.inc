' ============================================================================
'  stream.inc — Stream management (Start/Stop/Wait)
' ============================================================================
#IF NOT %DEF(%STREAM_INC)
%STREAM_INC = 1

' ===========================================================================
' StartStream - Start audio stream for ONE device (no ListView interaction).
' transport change called StopCheckedStreams + StartCheckedStreams, which
' stopped and restarted EVERY checked device - dropping audio on all of them
' just because one switched UDP<->TCP. Now UpdateDevice can call
' StopStream(idx) + StartStream(idx) to affect only the changed device.
' Returns 1 on success, 0 on skip/failure.
' ===========================================================================
FUNCTION StartStream(BYVAL devIdx AS LONG) AS LONG
    LOCAL saveIP AS DWORD
    LOCAL savePort AS DWORD
    LOCAL saveMac AS STRING
    LOCAL saveAudioPort AS DWORD
    LOCAL saveSmpRate AS DWORD
    LOCAL saveSlot AS LONG
    LOCAL hThread AS DWORD
    LOCAL cfgRetry AS LONG

    IF devIdx < 0 OR devIdx >= %MAX_DEVICES THEN FUNCTION = 0 : EXIT FUNCTION

    EnterCriticalSection g_csDev

    ' Skip if not active or already streaming
    ' StopAllStreams/StopStream signalled stop but the thread is still
    ' alive in cleanup (we wait up to 30s, but a timeout could leave an
    ' orphan). Refusing to start until the handle is cleared prevents
    ' duplicate AudioThreads on the same idx/port.
    IF g_Devs(devIdx).dwActive = 0 OR g_Devs(devIdx).dwHBActive <> 0 _
       OR g_Devs(devIdx).hAudioThread <> 0 THEN
        LeaveCriticalSection g_csDev
        FUNCTION = 0
        EXIT FUNCTION
    END IF

    g_Devs(devIdx).dwStatus    = %STS_STREAM
    g_Devs(devIdx).dwError     = 0
    g_Devs(devIdx).dwAudioPort = %AUDIO_PORT_BASE + devIdx

    ' Save values needed outside CS
    saveIP        = g_Devs(devIdx).dwIP
    savePort      = g_Devs(devIdx).dwPort
    saveMac       = TRIM$(g_Devs(devIdx).sMac)
    saveAudioPort = g_Devs(devIdx).dwAudioPort
    ' StartCheckedStreams read it OUTSIDE CS at log time, racing with
    ' UpdateDevice which can change it on the next INFO packet.
    saveSmpRate   = g_Devs(devIdx).dwSmpRate
    saveSlot      = g_Devs(devIdx).dwDiscSlot

    g_Devs(devIdx).dwRunning   = 1

    ' AudioThread later overwrites dwStreamStart when it has opened its
    ' transport + WaveOut - that's the real "audio-ready" timestamp -
    ' but we need a sane value NOW in case HB fires before AudioThread
    ' reaches that line.
    g_Devs(devIdx).dwStreamStart = GetTickCount()
    g_Devs(devIdx).dwLastPktTick = g_Devs(devIdx).dwStreamStart

    LeaveCriticalSection g_csDev

    ' Send CONFIGURE first (3x with 200ms gaps), then SLEEP 50 for ESP's
    ' tcp_stream_init_listen, then THREAD CREATE.
    AddLog "[HB] sending CONFIGURE for dev #" & TRIM$(STR$(devIdx)) & _
           " - pre-thread (TCP listen setup)"
    FOR cfgRetry = 1 TO 3
        SendConfigure saveIP, savePort, saveAudioPort, saveSlot
        IF cfgRetry < 3 THEN SLEEP 200
    NEXT cfgRetry

    SLEEP 50

    ' Create thread outside CS
    THREAD CREATE AudioThread(devIdx) TO hThread

    ' CREATE fails (hThread=0) we still set dwHBActive=1 -> UI shows
    '   ' Streaming' but no audio thread exists -> user stranded with no
    ' error. Recover by clearing state and reporting failure.
    IF hThread = 0 THEN
        AddLog "[HB] FATAL: THREAD CREATE AudioThread failed for dev #" & TRIM$(STR$(devIdx))
        EnterCriticalSection g_csDev
        ' 2.5-fix: clear dwRunning too (was left =1, confusing WM_DEVICECHANGE)
        g_Devs(devIdx).dwRunning    = 0
        g_Devs(devIdx).dwHBActive   = 0
        g_Devs(devIdx).dwStatus     = %STS_IDLE
        ' "device is discovered and visible in the list". A THREAD
        ' CREATE failure (low memory, handle exhaustion) is a stream-
        ' level error, not a discovery-level error. Clearing dwActive
        ' would make the device disappear from the UI even though it's
        ' still sending INFO packets and could be started again later.
        ' Only reset stream state (dwHBActive, dwStatus, hAudioThread).
        g_Devs(devIdx).hAudioThread = 0
        LeaveCriticalSection g_csDev
        FUNCTION = 0
        EXIT FUNCTION
    END IF

    ' 2.1: Recheck dwRunning BEFORE setting dwHBActive=1. AudioThread may
    ' have already exited with error (UDP OPEN fail, HeapAlloc fail) and
    ' cleared dwRunning=0 + dwHBActive=0. Setting dwHBActive=1 here would
    ' leave a 'Streaming' status with no live audio thread.
    EnterCriticalSection g_csDev
    IF g_Devs(devIdx).dwRunning = 0 THEN
        ' Thread already died - don't mark as streaming
        g_Devs(devIdx).hAudioThread = 0
        LeaveCriticalSection g_csDev
        ' Reap the dead thread handle
        WaitForSingleObject hThread, 1000
        LOCAL lRes21 AS LONG
        THREAD CLOSE hThread TO lRes21
        AddLog "[HB] dev #" & TRIM$(STR$(devIdx)) & _
               ": AudioThread exited immediately (open error) - NOT marking streaming"
        FUNCTION = 0
        EXIT FUNCTION
    END IF
    g_Devs(devIdx).hAudioThread = hThread
    g_Devs(devIdx).dwHBActive   = 1
    LeaveCriticalSection g_csDev

    AddLog "[HB] dwHBActive=1 for dev #" & TRIM$(STR$(devIdx)) & _
           " - heartbeat will be sent every " & TRIM$(STR$(%HB_INTERVAL)) & "ms"

    AddLog "Stream started: " & saveMac & " -> " & _
           FormatIP(saveIP) & ":" & TRIM$(STR$(savePort)) & _
           " port=" & TRIM$(STR$(saveAudioPort)) & _
           " rate=" & TRIM$(STR$(saveSmpRate)) & " Hz"
    FUNCTION = 1
END FUNCTION

' ===========================================================================
' StopStream - Stop audio stream for ONE device (no ListView interaction).
'               single device. Returns 1 if a stream was stopped, 0 otherwise.
' ===========================================================================
FUNCTION StopStream(BYVAL devIdx AS LONG) AS LONG
    LOCAL fNum AS LONG
    LOCAL hThread AS DWORD
    LOCAL lRes AS LONG
    LOCAL sMac AS STRING
    LOCAL stopIP AS DWORD
    LOCAL stopPort AS DWORD
    LOCAL stopSlot AS LONG

    IF devIdx < 0 OR devIdx >= %MAX_DEVICES THEN FUNCTION = 0 : EXIT FUNCTION

    ' Returns 1 so a concurrent StartStream right after this will succeed.
    EnterCriticalSection g_csDev
    IF g_Devs(devIdx).dwHBActive = 0 THEN
        IF g_Devs(devIdx).hAudioThread <> 0 THEN
            hThread = g_Devs(devIdx).hAudioThread
            g_Devs(devIdx).hAudioThread = 0
            LeaveCriticalSection g_csDev
            ' 2.5-fix: only close handle if thread actually exited.
            ' On timeout, restore handle so StartStream can still wait on it.
            LOCAL orphanWr AS LONG
            orphanWr = WaitForSingleObject(hThread, 5000)
            IF orphanWr = %WAIT_OBJECT_0 OR orphanWr = %WAIT_FAILED THEN
                THREAD CLOSE hThread TO lRes
                AddLog "[AUD] dev #" & TRIM$(STR$(devIdx)) & _
                       ": StopStream: reaped orphan AudioThread handle (now restartable)"
                FUNCTION = 1
            ELSE
                ' Thread still alive - preserve handle
                EnterCriticalSection g_csDev
                g_Devs(devIdx).hAudioThread = hThread
                LeaveCriticalSection g_csDev
                AddLog "[AUD] dev #" & TRIM$(STR$(devIdx)) & _
                       ": StopStream: orphan did not exit in 5s - handle preserved"
                FUNCTION = 0
            END IF
            EXIT FUNCTION
        END IF
        LeaveCriticalSection g_csDev
        FUNCTION = 0
        EXIT FUNCTION
    END IF
    ' Capture IP/Port/slot BEFORE clearing dwHBActive for SendStop
    stopIP   = g_Devs(devIdx).dwIP
    stopPort = g_Devs(devIdx).dwPort
    stopSlot = g_Devs(devIdx).dwDiscSlot
    g_Devs(devIdx).dwRunning  = 0
    g_Devs(devIdx).dwHBActive = 0
    g_Devs(devIdx).dwStatus   = %STS_IDLE
    fNum = g_Devs(devIdx).fAudioFile
    g_Devs(devIdx).fAudioFile = 0
    sMac = TRIM$(g_Devs(devIdx).sMac)
    hThread = g_Devs(devIdx).hAudioThread
    g_Devs(devIdx).hAudioThread = 0
    LeaveCriticalSection g_csDev

    AddLog "[HB] dwHBActive=0 for dev #" & TRIM$(STR$(devIdx)) & _
           " - heartbeat STOPPED (single stop)"

    ' Send explicit CMD_STOP to device so it stops immediately
    ' (instead of waiting for watchdog timeout)
    SendStop stopIP, stopPort, stopSlot

    ' AudioThread OWNS this socket and may be blocked inside UDP RECV on
    ' it right now. Calling UDP CLOSE #fNum from this (GUI) thread while
    ' RECV is in progress races on PowerBASIC's process-global file-number
    ' table (CLOSE frees the slot while RECV still references it ->
    ' use-after-free -> instant crash). The AudioThread polls dwRunning
    ' every <=500ms (UDP RECV timeout), sees 0, and closes its OWN socket
    ' in its cleanup section. We just wait for the thread below.
    ' (fNum is captured only for diagnostics / bookkeeping; fAudioFile is
    '  cleared above so a restart won't see a stale handle.)

    AddLog "Stream stopping: " & sMac

    ' Wait for the audio thread to finish (it closes its own socket).
    ' 3s timeout. If the thread was stuck in TCP reconnect (up to 2s per
    ' attempt × many retries), the wait timed out, THREAD CLOSE closed the
    ' handle but did NOT kill the thread. A subsequent StartStream would
    ' create a SECOND AudioThread for the same device → both trying to
    ' open the same port → duplicate handles / audio corruption / crash.

    ' Now: poll WaitForSingleObject(hThread, 1000) in a loop up to 5s.
    ' If still alive after 5s, log a warning and return 0 (failure) so
    ' the caller does NOT restart.
    ' The orphan thread will eventually exit on its own (dwRunning=0), but
    ' we don't risk a duplicate by starting a new one.
    IF hThread THEN
        LOCAL stopWaitMs AS LONG
        LOCAL stopWaitRes AS LONG
        LOCAL stopThreadAlive AS LONG
        stopWaitMs = 0
        stopThreadAlive = 0
        DO
            stopWaitRes = WaitForSingleObject(hThread, 1000)
            IF stopWaitRes = %WAIT_OBJECT_0 OR stopWaitRes = %WAIT_FAILED THEN EXIT DO
            stopWaitMs = stopWaitMs + 1000
            IF stopWaitMs >= 5000 THEN
                stopThreadAlive = 1
                EXIT DO
            END IF
        LOOP
        IF stopThreadAlive THEN
            ' StartStream's guard (hAudioThread <> 0) refuses to create a
            ' duplicate AudioThread on the same port. Do NOT THREAD CLOSE
            ' the handle here - the orphan thread self-exits when it next
            ' polls dwRunning=0 (already set).
            ' StopStream call (orphan-reaping block at the top of this
            ' function) or by WaitForAllThreads at program exit. This
            ' avoids the leak where a restored handle was never closed and
            ' the device stayed blocked until program restart.
            EnterCriticalSection g_csDev
            g_Devs(devIdx).hAudioThread = hThread
            LeaveCriticalSection g_csDev
            AddLog "[AUD] dev #" & TRIM$(STR$(devIdx)) & _
                   ": StopStream: AudioThread did not exit in 5s - " & _
                   "NOT restarting (orphan thread will self-exit). " & _
                   "Handle preserved so StartStream blocks until exit."
            FUNCTION = 0   ' failure - caller should not restart
            EXIT FUNCTION
        END IF
        THREAD CLOSE hThread TO lRes
    END IF

    FUNCTION = 1
END FUNCTION

' StartCheckedStreams - Start audio stream for all checked devices.
' monolithic version mixed per-device logic with ListView iteration,
' making it impossible to restart just one device on transport change.
SUB StartCheckedStreams()
    LOCAL hLV AS DWORD
    LOCAL i AS LONG
    LOCAL nCount AS LONG
    LOCAL devIdx AS LONG
    LOCAL started AS LONG

    CONTROL HANDLE g_hDlg, %IDC_LISTVIEW TO hLV
    IF hLV = 0 THEN EXIT SUB

    ' LISTVIEW GET COUNT, and %LVM_GETITEM (lParam retrieval) with
    ' LISTVIEW GET USER. Loop index   ' i' is 0-based for IsItemChecked
    ' (checkbox state still uses CONTROL SEND); LISTVIEW GET USER
    ' uses i + 1 (1-based).
    LISTVIEW GET COUNT g_hDlg, %IDC_LISTVIEW TO nCount

    FOR i = 0 TO nCount - 1
        IF IsItemChecked(hLV, i) = 0 THEN ITERATE FOR

        LISTVIEW GET USER g_hDlg, %IDC_LISTVIEW, i + 1 TO devIdx

        IF devIdx < 0 OR devIdx >= %MAX_DEVICES THEN ITERATE FOR

        IF StartStream(devIdx) THEN INCR started
    NEXT i

    IF started = 0 THEN
        AddLog "No devices to start (not checked or already streaming)"
    ELSE
        AddLog "Started" & STR$(started) & " stream(s)"
    END IF
END SUB

' StopCheckedStreams - Stop audio stream for all checked streaming devices.
' two-phase monolith (signal-all-then-wait-all) is preserved by calling
' StopStream per device, which signals AND waits before returning. This
' serializes stop operations (slower for many devices) but is far simpler
' and avoids the duplicated ListView-iteration code. The two-phase approach
' was originally needed so all sockets got CMD_STOP quickly; in practice
' the 50ms between stops is negligible vs the 3s thread-wait timeout.
SUB StopCheckedStreams()
    LOCAL hLV AS DWORD
    LOCAL i AS LONG
    LOCAL nCount AS LONG
    LOCAL devIdx AS LONG
    LOCAL stopped AS LONG

    CONTROL HANDLE g_hDlg, %IDC_LISTVIEW TO hLV
    IF hLV = 0 THEN EXIT SUB

    ' LISTVIEW GET COUNT, and %LVM_GETITEM (lParam retrieval) with
    ' LISTVIEW GET USER. Loop index   ' i' is 0-based for IsItemChecked
    ' (checkbox state still uses CONTROL SEND); LISTVIEW GET USER
    ' uses i + 1 (1-based).
    LISTVIEW GET COUNT g_hDlg, %IDC_LISTVIEW TO nCount

    FOR i = 0 TO nCount - 1
        IF IsItemChecked(hLV, i) = 0 THEN ITERATE FOR

        LISTVIEW GET USER g_hDlg, %IDC_LISTVIEW, i + 1 TO devIdx

        IF devIdx < 0 OR devIdx >= %MAX_DEVICES THEN ITERATE FOR

        IF StopStream(devIdx) THEN INCR stopped
    NEXT i
END SUB

' StopAllStreams - Stop all active streams, unblock audio UDP recv
SUB StopAllStreams()
    LOCAL idx AS LONG
    DIM fNums(%MAX_DEVICES - 1) AS LONG
    DIM stopIPs(%MAX_DEVICES - 1) AS DWORD
    DIM stopPorts(%MAX_DEVICES - 1) AS DWORD
    DIM stopSlots(%MAX_DEVICES - 1) AS LONG
    ' HeapAlloc is unsafe, and if AddLog is ever changed to SendMessage
    ' this becomes an instant deadlock.
    DIM logLines(%MAX_DEVICES - 1) AS STRING
    DIM logCount AS LONG
    ' StartStream's precondition only checked dwHBActive - so the user
    ' could click Start and a SECOND AudioThread would be created on the
    ' same idx/port while the old one was still alive -> duplicate socket
    ' / port conflict / crash. Now we wait per-device (same pattern as
    ' StopStream) so StopAllStreams doesn't return until all threads are
    ' actually dead (or 30s timeout).
    DIM waitThreads(%MAX_DEVICES - 1) AS DWORD
    DIM waitDevs(%MAX_DEVICES - 1) AS LONG
    DIM waitCount AS LONG
    waitCount = 0

    EnterCriticalSection g_csDev

    FOR idx = 0 TO %MAX_DEVICES - 1
        IF g_Devs(idx).dwActive AND g_Devs(idx).dwHBActive THEN
            ' Capture IP/Port/slot for SendStop BEFORE clearing dwHBActive
            stopIPs(idx)   = g_Devs(idx).dwIP
            stopPorts(idx) = g_Devs(idx).dwPort
            stopSlots(idx) = g_Devs(idx).dwDiscSlot
            g_Devs(idx).dwRunning  = 0
            g_Devs(idx).dwHBActive = 0
            g_Devs(idx).dwStatus   = %STS_IDLE
            logLines(logCount) = "[HB] dwHBActive=0 for dev #" & _
                                  TRIM$(STR$(idx)) & _
                                  " - heartbeat STOPPED (stop all)"
            INCR logCount
            waitThreads(waitCount) = g_Devs(idx).hAudioThread
            waitDevs(waitCount)    = idx
            INCR waitCount
            g_Devs(idx).hAudioThread = 0
        ELSE
            stopIPs(idx) = 0
            stopSlots(idx) = -1
        END IF
        ' Read fAudioFile under CS and zero it
        fNums(idx) = g_Devs(idx).fAudioFile
        g_Devs(idx).fAudioFile = 0
    NEXT idx

    LeaveCriticalSection g_csDev

    ' Emit deferred log lines OUTSIDE the critical section.
    FOR idx = 0 TO logCount - 1
        AddLog logLines(idx)
    NEXT idx

    ' Send explicit CMD_STOP to each device OUTSIDE CS (UDP SEND can block)
    FOR idx = 0 TO %MAX_DEVICES - 1
        IF stopIPs(idx) THEN
            SendStop stopIPs(idx), stopPorts(idx), stopSlots(idx)
        END IF
    NEXT idx

    ' OWNS its socket and closes it in its own cleanup section. Closing from
    ' this thread while an AudioThread is blocked in UDP RECV races on PB's
    ' process-global file-number table (close frees the slot while RECV still
    ' references it -> use-after-free -> instant crash, seen in UDP mode).
    ' Threads see dwRunning=0 within their RECV timeout (<=500ms UDP, <=2s TCP)
    ' and exit cleanly. (fNums() captured above only to clear fAudioFile slots.)

    ' StartStream's precondition (dwHBActive=0 + hAudioThread=0) is safe.
    FOR idx = 0 TO waitCount - 1
        LOCAL waitH AS DWORD
        LOCAL waitDev AS LONG
        LOCAL waitMs AS LONG
        LOCAL waitRes AS LONG
        LOCAL lRes AS LONG
        waitH   = waitThreads(idx)
        waitDev = waitDevs(idx)
        IF waitH = 0 THEN ITERATE FOR
        waitMs = 0
        DO
            waitRes = WaitForSingleObject(waitH, 1000)
            IF waitRes = %WAIT_OBJECT_0 OR waitRes = %WAIT_FAILED THEN EXIT DO
            waitMs = waitMs + 1000
            IF waitMs >= 5000 THEN
                ' Do NOT THREAD CLOSE the handle - keep it open so a later
                ' StopStream / WaitForAllThreads can still wait on it.
                EnterCriticalSection g_csDev
                g_Devs(waitDev).hAudioThread = waitH
                LeaveCriticalSection g_csDev
                AddLog "[AUD] dev #" & TRIM$(STR$(waitDev)) & _
                       ": StopAllStreams: AudioThread did not exit in 5s - " & _
                       "orphan thread will self-exit (handle preserved, StartStream blocked)"
                ' Skip THREAD CLOSE below - handle kept open as orphan.
                ITERATE FOR
            END IF
        LOOP
        THREAD CLOSE waitH TO lRes
    NEXT idx

    FOR idx = 0 TO %MAX_DEVICES - 1
        LOCAL reapH AS DWORD
        EnterCriticalSection g_csDev
        IF g_Devs(idx).dwHBActive = 0 AND g_Devs(idx).hAudioThread <> 0 THEN
            reapH = g_Devs(idx).hAudioThread
            g_Devs(idx).hAudioThread = 0
        ELSE
            reapH = 0
        END IF
        LeaveCriticalSection g_csDev
        IF reapH THEN
            ' 2.4-fix: only close handle if thread actually exited
            LOCAL reapWr AS LONG
            reapWr = WaitForSingleObject(reapH, 5000)
            IF reapWr = %WAIT_OBJECT_0 OR reapWr = %WAIT_FAILED THEN
                THREAD CLOSE reapH TO lRes
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": StopAllStreams: reaped orphan AudioThread handle"
            ELSE
                ' Thread still alive - preserve handle
                EnterCriticalSection g_csDev
                g_Devs(idx).hAudioThread = reapH
                LeaveCriticalSection g_csDev
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": StopAllStreams: orphan still alive, handle preserved"
            END IF
        END IF
    NEXT idx
END SUB

#ENDIF
