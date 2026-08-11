' ============================================================================
'  audio_thread.inc — Audio thread (AudioThread + ResetPipelineState macro)
' ============================================================================
#IF NOT %DEF(%AUDIO_THREAD_INC)
%AUDIO_THREAD_INC = 1

' ResetPipelineState macro - must be called from inside AudioThread.
MACRO ResetPipelineState()
    ' ADPCM decoder state (per-channel predictors)
    predictor  = 0
    stepIndex  = 0
    predictor2 = 0
    stepIndex2 = 0
    ' Ring buffer reset. ringEverStarted NOT reset (stays 1 after first start
    ' for faster ringMinBytes refill on reconnect instead of full prebuffer).
    ringW = 0
    ringR = 0
    ringUsed = 0
    ringPlaying = 0
    frameBytes = 0
    underrunCount = 0
    overflowCount = 0
    lastAdaptTick = GetTickCount()
    ' Startup skip + fade-in (1s skip + 1s fade, same as cold start)
    skipFramesRemaining = 999
    fadeSamplesRemaining = 0
    ' PLC state
    lastPcmPtr = 0
    lastPcmLen = 0
    ' BWSOLA PLC pending state (avoid stale plcLPtr firing on next good packet).
    plcPending = 0
    plcLPtr = 0
    plcLLen = 0
    pktRecv = 0
    lastSeq = 0
    lostPkt = 0
    oooPkt  = 0
    ' Reset per-device stats + stall-detection baseline (fresh start).
    EnterCriticalSection g_csDev
    g_Devs(idx).dwPktRecv     = 0
    g_Devs(idx).dwPktOOO      = 0
    g_Devs(idx).dwPktLost     = 0
    g_Devs(idx).wLastSeq      = 0
    g_Devs(idx).dwStreamStart = GetTickCount()
    g_Devs(idx).dwLastPktTick = g_Devs(idx).dwStreamStart
    LeaveCriticalSection g_csDev
END MACRO

' AudioThread - Per-device audio receive/decode/play thread
'   Supports mono (1 DVI4 block) and stereo (2 independent DVI4 blocks).
'   Stereo format: [pkt_header][DVI4_L][DVI4_R]
THREAD FUNCTION AudioThread(BYVAL param AS DWORD) AS DWORD
    LOCAL idx AS LONG
    LOCAL fNum AS LONG
    LOCAL waveOut AS DWORD
    LOCAL wIdx AS LONG
    LOCAL wIdx2 AS LONG
    LOCAL fromIP AS DWORD
    LOCAL fromPort AS DWORD
    LOCAL recvBuf AS STRING
    LOCAL adpcmLen AS LONG
    LOCAL pcmLen AS LONG
    LOCAL seqNum AS WORD
    LOCAL lastSeq AS WORD
    LOCAL pktRecv AS DWORD
    LOCAL predictor AS INTEGER
    LOCAL stepIndex AS LONG
    LOCAL predictor2 AS INTEGER
    LOCAL stepIndex2 AS LONG
    LOCAL bindPort AS LONG
    LOCAL lostPkt AS LONG
    LOCAL oooPkt AS LONG
    LOCAL nCh AS LONG
    LOCAL adpcmLenR AS LONG
    LOCAL adpcmLenL AS LONG
    LOCAL pcmLenL AS LONG
    LOCAL pcmLenR AS LONG
    LOCAL wfFmt AS WAVEFORMATEX
    LOCAL stillRun AS LONG

    idx = param

    ' Read audio port, channels, sample rate, bits, codec under CS
    EnterCriticalSection g_csDev
    bindPort = g_Devs(idx).dwAudioPort
    nCh = g_Devs(idx).dwChannels
    LOCAL devSmpRate AS DWORD
    devSmpRate = g_Devs(idx).dwSmpRate
    LOCAL devBits AS LONG
    devBits = g_Devs(idx).dwBits
    LOCAL devCodec AS LONG
    devCodec = g_Devs(idx).dwCodec
    LOCAL devTransport AS LONG
    devTransport = g_Devs(idx).dwTransport
    LOCAL devIP AS DWORD
    devIP = g_Devs(idx).dwIP
    LOCAL devWaveDevice AS LONG
    devWaveDevice = g_Devs(idx).dwWaveDevice
    ' Initialized from devSmpRate via Hz-to-enum conversion; updated on format change.
    LOCAL curRateEnum AS LONG
    curRateEnum = HzToRateEnum(devSmpRate)
    LeaveCriticalSection g_csDev

    IF nCh < 1 THEN nCh = 1
    IF nCh > 2 THEN nCh = 2
    IF devSmpRate < 8000 OR devSmpRate > 48000 THEN devSmpRate = 48000
    IF devBits <> 16 AND devBits <> 24 THEN devBits = 16
    ' HARDENING: re-validate transport (ProcessInfo already corrects, but state may have been corrupted).
    IF devTransport <> 0 AND devTransport <> 1 THEN
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
               ": invalid transport=" & TRIM$(STR$(devTransport)) & _
               " -> forcing UDP (0)"
        devTransport = 0
    END IF

    ' Open transport: TCP connect to ESP (if transport=1) or UDP listen (default).
    ' FREEFILE is called inside each branch under g_csFile (A1: file-number
    ' allocation is not thread-safe across AudioThread / dump / discovery).
    ERRCLEAR
    IF devTransport = 1 THEN
        ' TCP mode: ESP = listener, we connect to ESP_IP:bindPort.
        ' Retry connect every 1s until success or stream stops.
        LOCAL tcpHost AS STRING
        tcpHost = FormatIP(devIP)
        LOCAL connectAttempts AS LONG
        connectAttempts = 0
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
               ": TCP connecting to " & tcpHost & ":" & TRIM$(STR$(bindPort))
        ' Retry connect until success or stream stops.
        ' stillRun is read under g_csDev (A1: dwRunning is not atomically safe).
        DO
            EnterCriticalSection g_csFile
            fNum = FREEFILE
            ERRCLEAR
            TCP OPEN PORT bindPort AT tcpHost AS #fNum TIMEOUT 2000
            LeaveCriticalSection g_csFile
            IF ERR = 0 THEN EXIT DO
            INCR connectAttempts
            IF connectAttempts = 1 OR (connectAttempts MOD 10) = 0 THEN
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": TCP connect attempt " & TRIM$(STR$(connectAttempts)) & _
                       " failed err=" & TRIM$(STR$(ERR)) & ", retrying..."
            END IF
            ' Check if we should stop (parent cleared dwRunning externally)
            EnterCriticalSection g_csDev
            stillRun = g_Devs(idx).dwRunning
            LeaveCriticalSection g_csDev
            IF stillRun = 0 AND connectAttempts > 1 THEN
                ' Stream was stopped while we were trying to connect
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & ": stream stopped during TCP connect"
                FUNCTION = 0
                EXIT FUNCTION
            END IF
            SLEEP 1000
        LOOP
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
               ": TCP connected to " & tcpHost & ":" & TRIM$(STR$(bindPort)) & _
               " (file #" & TRIM$(STR$(fNum)) & ")"
    ELSE
        ' UDP mode (default): open listening UDP socket on bindPort.
        ' TIMEOUT 500 so thread notices dwRunning=0 within 500ms and closes its
        ' OWN socket (closing #fNum from main thread while RECV is in progress races).
        EnterCriticalSection g_csFile
        fNum = FREEFILE
        ERRCLEAR
        UDP OPEN PORT bindPort AS #fNum TIMEOUT 500
        IF ERR THEN
            LeaveCriticalSection g_csFile
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": UDP OPEN PORT " & TRIM$(STR$(bindPort)) & " FAILED err=" & TRIM$(STR$(ERR))
            EnterCriticalSection g_csDev
            g_Devs(idx).fAudioFile = 0
            g_Devs(idx).dwRunning  = 0
            LeaveCriticalSection g_csDev
            FUNCTION = 0
            EXIT FUNCTION
        END IF
        LeaveCriticalSection g_csFile
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
               ": UDP OPEN PORT " & TRIM$(STR$(bindPort)) & " OK (file #" & TRIM$(STR$(fNum)) & ")"
    END IF

    EnterCriticalSection g_csDev
    g_Devs(idx).fAudioFile = fNum
    LeaveCriticalSection g_csDev

    ' Allocate WAVEHDR array
    DIM whdrs(0 TO %NUM_WAVE_BUFS - 1) AS LOCAL WAVEHDR
    DIM pcmPtrs(0 TO %NUM_WAVE_BUFS - 1) AS LOCAL DWORD

    ' Allocate PCM buffers. Max size (WAVE_BUF_SZ * 4) so format changes
    ' (mono <-> stereo) on the fly don't require reallocation.
    LOCAL bufSz AS LONG
    bufSz = %WAVE_BUF_SZ * 4
    FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
        pcmPtrs(wIdx2) = HeapAlloc(g_hHeap, 0, bufSz)
        IF pcmPtrs(wIdx2) = 0 THEN
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": HeapAlloc failed at buf " & TRIM$(STR$(wIdx2))
            LOCAL j AS LONG
            FOR j = 0 TO wIdx2 - 1
                IF pcmPtrs(j) THEN HeapFree g_hHeap, 0, pcmPtrs(j)
            NEXT j
            ERRCLEAR
            IF devTransport = 1 THEN TCP CLOSE #fNum ELSE UDP CLOSE #fNum
            EnterCriticalSection g_csDev
            ' UI does not stay in "Streaming" after a failed start.
            g_Devs(idx).fAudioFile = 0
            g_Devs(idx).dwRunning  = 0
            g_Devs(idx).dwHBActive = 0
            g_Devs(idx).dwStatus   = %STS_IDLE
            LeaveCriticalSection g_csDev
            FUNCTION = 0
            EXIT FUNCTION
        END IF
    NEXT wIdx2

    ' Init WAVEHDRs
    FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
        whdrs(wIdx2).lpData         = pcmPtrs(wIdx2)
        whdrs(wIdx2).dwBufferLength = bufSz
        whdrs(wIdx2).dwBytesRecorded = 0
        whdrs(wIdx2).dwUser         = 0
        whdrs(wIdx2).dwFlags        = 0
        whdrs(wIdx2).dwLoops        = 0
        whdrs(wIdx2).lpNext         = 0
        whdrs(wIdx2).Reserved       = 0
    NEXT wIdx2

    ' Set up WAVEFORMATEX - bits per sample depends on CODEC:
    '   ADPCM (codec=5): ALWAYS 16-bit (ESP dithers 24→16 before encoding).
    '   PCM (codec=6): uses devBits (16 or 24). Falls back to 16 if card rejects 24.
    LOCAL outBits AS LONG
    IF devCodec = %CODEC_ID_PCM THEN
        outBits = devBits          ' PCM: use native bit depth
    ELSE
        outBits = 16               ' ADPCM: always 16-bit (dithered on ESP)
    END IF

    ' BWSOLA PLC forces 16-bit WaveOut (operates on INTEGER PCM).
    IF %BWSOLA_PLC_ENABLE AND outBits = 24 THEN
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
               ": BWSOLA forces 16-bit WaveOut (was 24-bit PCM)"
        outBits = 16
    END IF

    ' Open WaveOut at ACTUAL ESP I2S rate (drift fix; see EspActualRate).
    ' Pass devBits (real I2S depth), NOT outBits.
    LOCAL actualSmpRate AS DWORD
    actualSmpRate = EspActualRate(devSmpRate, devBits)
    IF actualSmpRate <> devSmpRate THEN
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
               ": rate trim " & TRIM$(STR$(devSmpRate)) & "Hz -> " & _
               TRIM$(STR$(actualSmpRate)) & "Hz (ESP I2S clock, drift fix)"
    END IF

    wfFmt.wFormatTag      = %WAVE_FORMAT_PCM
    wfFmt.nChannels       = nCh
    wfFmt.nSamplesPerSec  = actualSmpRate
    wfFmt.wBitsPerSample  = outBits
    wfFmt.nBlockAlign     = nCh * (outBits \ 8)
    wfFmt.nAvgBytesPerSec = wfFmt.nSamplesPerSec * wfFmt.nBlockAlign
    wfFmt.cbSize          = 0

    ' Open WaveOut. Try 24-bit first if ESP is 24-bit; retry with 16-bit if rejected.
    LOCAL waveResult AS LONG
    waveResult = waveOutOpen(waveOut, devWaveDevice, wfFmt, 0, 0, %CALLBACK_NULL)
    IF waveResult <> 0 AND outBits = 24 THEN
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
               ": 24-bit WaveOut failed (err=" & TRIM$(STR$(waveResult)) & _
               "), falling back to 16-bit"
        outBits = 16
        wfFmt.wBitsPerSample  = 16
        wfFmt.nBlockAlign     = nCh * 2
        wfFmt.nAvgBytesPerSec = wfFmt.nSamplesPerSec * wfFmt.nBlockAlign
        waveResult = waveOutOpen(waveOut, devWaveDevice, wfFmt, 0, 0, %CALLBACK_NULL)
    END IF
    IF waveResult = 0 THEN
        LOCAL sCodecName AS STRING
        IF devCodec = %CODEC_ID_PCM THEN
            sCodecName = "PCM"
        ELSE
            sCodecName = "ADPCM"
        END IF
        IF outBits <> devBits AND devCodec = %CODEC_ID_PCM THEN
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": WaveOut " & TRIM$(STR$(outBits)) & "-bit " & sCodecName & _
                   " (converting from " & TRIM$(STR$(devBits)) & "-bit)"
        ELSE
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": WaveOut " & TRIM$(STR$(outBits)) & "-bit " & sCodecName & " (native)"
        END IF
    END IF
    IF waveResult <> 0 THEN
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
               ": waveOutOpen FAILED err=" & TRIM$(STR$(waveResult))
        FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
            IF pcmPtrs(wIdx2) THEN HeapFree g_hHeap, 0, pcmPtrs(wIdx2)
        NEXT wIdx2
        ERRCLEAR
        IF devTransport = 1 THEN TCP CLOSE #fNum ELSE UDP CLOSE #fNum
        EnterCriticalSection g_csDev
        g_Devs(idx).dwRunning  = 0
        g_Devs(idx).fAudioFile = 0
        g_Devs(idx).dwHBActive = 0
        g_Devs(idx).dwStatus   = %STS_IDLE
        LeaveCriticalSection g_csDev
        FUNCTION = 0
        EXIT FUNCTION
    END IF

    EnterCriticalSection g_csDev
    g_Devs(idx).hWaveOut = waveOut
    LeaveCriticalSection g_csDev

    ' Prepare headers. waveOutPrepareHeader failure disables the buffer (mark INQUEUE).
    LOCAL prepRes AS LONG
    FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
        prepRes = waveOutPrepareHeader(waveOut, whdrs(wIdx2), SIZEOF(WAVEHDR))
        IF prepRes <> 0 THEN
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": waveOutPrepareHeader FAILED buf=" & TRIM$(STR$(wIdx2)) & _
                   " err=" & TRIM$(STR$(prepRes)) & " - buffer disabled"
            ' Mark as INQUEUE so submit scan skips it; otherwise we'd send
            ' an unprepared header to waveOutWrite -> UB.
            whdrs(wIdx2).dwFlags = whdrs(wIdx2).dwFlags OR %WHDR_INQUEUE
        END IF
    NEXT wIdx2

    ' Init stats under CS (idempotent re-affirmation of StartStream's dwRunning=1).
    EnterCriticalSection g_csDev
    g_Devs(idx).dwRunning     = 1
    g_Devs(idx).dwPktRecv     = 0
    g_Devs(idx).dwPktOOO      = 0
    g_Devs(idx).dwPktLost     = 0
    g_Devs(idx).dwBytesRecv   = 0
    g_Devs(idx).wLastSeq      = 0
    g_Devs(idx).dwStreamStart = GetTickCount()
    g_Devs(idx).dwLastPktTick = g_Devs(idx).dwStreamStart   ' stall-detection baseline
    LeaveCriticalSection g_csDev

    predictor  = 0
    stepIndex  = 0
    predictor2 = 0
    stepIndex2 = 0
    lastSeq = 0
    pktRecv = 0
    wIdx = 0

    ' ---- PCM Ring Buffer / PLC / Adaptive state ----
    LOCAL ringCap AS LONG          ' Ring capacity in bytes (= %RING_CAP_BYTES)
    LOCAL ringPtr AS DWORD         ' HeapAlloc'd ring buffer (ringCap bytes)
    LOCAL ringW AS LONG            ' Write index (next free byte)
    LOCAL ringR AS LONG            ' Read index (next data byte)
    LOCAL ringUsed AS LONG         ' Bytes currently in ring
    LOCAL frameBytes AS LONG       ' Bytes per audio frame (locked from first played frame)
    LOCAL ringTargetBytes AS LONG  ' Target ring depth (bytes) at start/refill
    LOCAL ringMinBytes AS LONG     ' Soft-refill threshold after first underrun
    LOCAL ringMaxBytes AS LONG     ' Adaptive grow ceiling
    LOCAL ringPlaying AS LONG      ' 1 = consumer (FeedWaveOut) is active
    LOCAL ringEverStarted AS LONG  ' 1 after first ring play ON (faster refill on reconnect)
    LOCAL underrunCount AS LONG    ' Underruns in current adaptation interval
    LOCAL overflowCount AS LONG    ' Overflows in current adaptation interval
    LOCAL lastAdaptTick AS DWORD   ' Last adaptive adjustment time
    LOCAL bytesPerMs AS LONG       ' actualSmpRate * nCh * (outBits/8) / 1000
    LOCAL feedNeed AS LONG         ' Temp: bytes needed to (re)start consumer
    LOCAL decodePtr AS DWORD       ' Decode always here, not into WaveOut slot
    LOCAL interleaveBuf AS DWORD   ' Separate buffer for stereo interleave (heap overflow fix)

    LOCAL lastPcmPtr AS DWORD      ' Pointer to last decoded PCM (for PLC)
    LOCAL lastPcmLen AS LONG       ' Length of last decoded PCM (for PLC)

    ' ---- BWSOLA PLC state (per Yeh et al. 2013) ----
    ' Bilateral PLC: on loss detection, copy lastPcm into plcLBuf (stable storage
    ' since decodePtr gets overwritten) and set plcPending. When the next good
    ' packet arrives and is decoded, run BWSOLA_PLC_Frame using plcLBuf (L) +
    ' decodePtr (R), write concealed frame, then write the real packet.
    LOCAL plcPending AS LONG     ' >0 = loss detected, waiting for R_WSOLA
    LOCAL plcLPtr AS DWORD       ' pointer to L_WSOLA (stable copy in plcLBuf)
    LOCAL plcLLen AS LONG        ' length of L_WSOLA in BYTES
    LOCAL plcLBuf AS DWORD       ' HeapAlloc'd stable buffer for L_WSOLA copy
    LOCAL plcOutBuf AS DWORD     ' HeapAlloc'd buffer for BWSOLA_PLC_Frame output

    ' Startup skip: skip first N ms of audio. Frame count computed from frame_ms
    ' (parsed from first packet header, offset 9).
    LOCAL skipFramesRemaining AS LONG
    LOCAL skipFrameMs AS LONG
    skipFramesRemaining = 999   ' large sentinel; replaced after first packet
    skipFrameMs = 20            ' default; updated from packet header

    LOCAL fadeSamplesRemaining AS LONG
    LOCAL fadeSamplesTotal AS LONG
    fadeSamplesRemaining = 0   ' armed when skip completes
    fadeSamplesTotal = 0

    ' ---- Allocate ring + decode buffers ----
    ' Ring allocated AFTER waveOutOpen (need actualSmpRate/outBits for bytesPerMs).
    ringCap = %RING_CAP_BYTES
    ringPtr = HeapAlloc(g_hHeap, %HEAP_ZERO_MEMORY, ringCap)
    IF ringPtr = 0 THEN
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & ": RingAlloc FAILED"
        IF waveOut THEN
            waveOutReset waveOut
            FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
                waveOutUnprepareHeader waveOut, whdrs(wIdx2), SIZEOF(WAVEHDR)
            NEXT wIdx2
            waveOutClose waveOut
            waveOut = 0
            EnterCriticalSection g_csDev
            g_Devs(idx).hWaveOut = 0
            LeaveCriticalSection g_csDev
        END IF
        FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
            IF pcmPtrs(wIdx2) THEN HeapFree g_hHeap, 0, pcmPtrs(wIdx2)
        NEXT wIdx2
        ERRCLEAR
        IF devTransport = 1 THEN TCP CLOSE #fNum ELSE UDP CLOSE #fNum
        EnterCriticalSection g_csDev
        g_Devs(idx).fAudioFile = 0
        g_Devs(idx).dwRunning  = 0
        g_Devs(idx).dwHBActive = 0
        g_Devs(idx).dwStatus   = %STS_IDLE
        LeaveCriticalSection g_csDev
        FUNCTION = 0 : EXIT FUNCTION
    END IF

    decodePtr = HeapAlloc(g_hHeap, 0, bufSz)
    interleaveBuf = HeapAlloc(g_hHeap, 0, bufSz)
    IF decodePtr = 0 OR interleaveBuf = 0 THEN
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & ": decodeBuf/interleaveBuf alloc FAILED"
        IF waveOut THEN
            waveOutReset waveOut
            FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
                waveOutUnprepareHeader waveOut, whdrs(wIdx2), SIZEOF(WAVEHDR)
            NEXT wIdx2
            waveOutClose waveOut
            waveOut = 0
            EnterCriticalSection g_csDev
            g_Devs(idx).hWaveOut = 0
            LeaveCriticalSection g_csDev
        END IF
        HeapFree g_hHeap, 0, interleaveBuf
        HeapFree g_hHeap, 0, ringPtr
        FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
            IF pcmPtrs(wIdx2) THEN HeapFree g_hHeap, 0, pcmPtrs(wIdx2)
        NEXT wIdx2
        ERRCLEAR
        IF devTransport = 1 THEN TCP CLOSE #fNum ELSE UDP CLOSE #fNum
        EnterCriticalSection g_csDev
        g_Devs(idx).fAudioFile = 0
        g_Devs(idx).dwRunning  = 0
        g_Devs(idx).dwHBActive = 0
        g_Devs(idx).dwStatus   = %STS_IDLE
        LeaveCriticalSection g_csDev
        FUNCTION = 0 : EXIT FUNCTION
    END IF

    ' ---- BWSOLA PLC buffers ----
    ' plcLBuf: stable storage for L_WSOLA copy. plcOutBuf: BWSOLA_PLC_Frame output.
    ' Both bufSz bytes (same as decodePtr).
    IF %BWSOLA_PLC_ENABLE THEN
        plcLBuf = HeapAlloc(g_hHeap, %HEAP_ZERO_MEMORY, bufSz)
        IF plcLBuf = 0 THEN
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & ": plcLBuf alloc FAILED"
            IF waveOut THEN
                waveOutReset waveOut
                FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
                    waveOutUnprepareHeader waveOut, whdrs(wIdx2), SIZEOF(WAVEHDR)
                NEXT wIdx2
                waveOutClose waveOut
                waveOut = 0
                EnterCriticalSection g_csDev
                g_Devs(idx).hWaveOut = 0
                LeaveCriticalSection g_csDev
            END IF
            HeapFree g_hHeap, 0, interleaveBuf
            HeapFree g_hHeap, 0, decodePtr
            HeapFree g_hHeap, 0, ringPtr
            FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
                IF pcmPtrs(wIdx2) THEN HeapFree g_hHeap, 0, pcmPtrs(wIdx2)
            NEXT wIdx2
            ERRCLEAR
            IF devTransport = 1 THEN TCP CLOSE #fNum ELSE UDP CLOSE #fNum
            EnterCriticalSection g_csDev
            g_Devs(idx).fAudioFile = 0
            g_Devs(idx).dwRunning  = 0
            g_Devs(idx).dwHBActive = 0
            g_Devs(idx).dwStatus   = %STS_IDLE
            LeaveCriticalSection g_csDev
            FUNCTION = 0 : EXIT FUNCTION
        END IF
        plcOutBuf = HeapAlloc(g_hHeap, %HEAP_ZERO_MEMORY, bufSz)
        IF plcOutBuf = 0 THEN
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & ": plcOutBuf alloc FAILED"
            IF waveOut THEN
                waveOutReset waveOut
                FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
                    waveOutUnprepareHeader waveOut, whdrs(wIdx2), SIZEOF(WAVEHDR)
                NEXT wIdx2
                waveOutClose waveOut
                waveOut = 0
                EnterCriticalSection g_csDev
                g_Devs(idx).hWaveOut = 0
                LeaveCriticalSection g_csDev
            END IF
            HeapFree g_hHeap, 0, interleaveBuf
            HeapFree g_hHeap, 0, plcLBuf : plcLBuf = 0
            HeapFree g_hHeap, 0, decodePtr
            HeapFree g_hHeap, 0, ringPtr
            FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
                IF pcmPtrs(wIdx2) THEN HeapFree g_hHeap, 0, pcmPtrs(wIdx2)
            NEXT wIdx2
            ERRCLEAR
            IF devTransport = 1 THEN TCP CLOSE #fNum ELSE UDP CLOSE #fNum
            EnterCriticalSection g_csDev
            g_Devs(idx).fAudioFile = 0
            g_Devs(idx).dwRunning  = 0
            g_Devs(idx).dwHBActive = 0
            g_Devs(idx).dwStatus   = %STS_IDLE
            LeaveCriticalSection g_csDev
            FUNCTION = 0 : EXIT FUNCTION
        END IF
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & ": BWSOLA PLC enabled (16-bit)"
    ELSE
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & ": BWSOLA PLC disabled (frame-repeat fallback)"
    END IF

    ringW = 0 : ringR = 0 : ringUsed = 0
    frameBytes = 0
    ringPlaying = 0
    ringEverStarted = 0
    underrunCount = 0
    overflowCount = 0
    lastAdaptTick = GetTickCount()
    lastPcmPtr = 0
    lastPcmLen = 0

    ' BWSOLA PLC pending state (init)
    plcPending = 0
    plcLPtr = 0
    plcLLen = 0
    ' Depth from ACTUAL WaveOut rate (EspActualRate) + channels + outBits.
    ' bytesPerMs is recomputed on every reopen (rate/ch/bits may change).
    bytesPerMs = (CLNG(actualSmpRate) * nCh * (outBits \ 8) + 999) \ 1000
    IF bytesPerMs < 1 THEN bytesPerMs = 1
    ringTargetBytes = bytesPerMs * %JITTER_INITIAL_MS
    ringMinBytes    = bytesPerMs * %JITTER_MIN_MS
    ringMaxBytes    = bytesPerMs * %JITTER_MAX_MS
    IF ringMaxBytes > (ringCap \ 2) THEN ringMaxBytes = ringCap \ 2
    IF ringTargetBytes > ringMaxBytes THEN ringTargetBytes = ringMaxBytes
    IF ringMinBytes > ringTargetBytes THEN ringMinBytes = ringTargetBytes

    AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
           ": ring target=" & TRIM$(STR$(ringTargetBytes)) & "B (~" & _
           TRIM$(STR$(%JITTER_INITIAL_MS)) & "ms) min=" & _
           TRIM$(STR$(ringMinBytes)) & " max=" & TRIM$(STR$(ringMaxBytes)) & _
           " cap=" & TRIM$(STR$(ringCap))

    ' ---- Main audio loop ----
    ' TCP framing: [u16 length BE][16-byte pkt_header][payload].
    ' UDP: each datagram = [16-byte pkt_header][payload].
    LOCAL audRecvCount AS LONG    ' count of successful RECV
    LOCAL audErrCount  AS LONG    ' count of ERR on RECV
    audRecvCount = 0
    audErrCount  = 0

    ' TCP framing state
    LOCAL tcpLenBuf AS STRING        ' 2-byte length prefix buffer
    LOCAL tcpPktLen AS LONG          ' decoded packet length (16..16384)
    LOCAL tcpGot AS LONG             ' bytes received so far in current frame
    LOCAL tcpNeed AS LONG            ' bytes still needed
    LOCAL tcpTmp AS STRING           ' temp buffer for partial reads
    LOCAL tcpReconnectCount AS LONG
    tcpReconnectCount = 0

    DO
        ' A1: read dwRunning under CS (not atomic, races with StopStream).
        EnterCriticalSection g_csDev
        stillRun = g_Devs(idx).dwRunning
        LeaveCriticalSection g_csDev
        IF stillRun = 0 THEN EXIT DO

        ERRCLEAR

        IF devTransport = 1 THEN
            ' ---- TCP framing read ----
            ' 1) Read 2-byte length prefix (big-endian).
            ' PowerBASIC TCP RECV may return fewer bytes than requested,
            ' so loop until we have exactly 2 bytes (or error/timeout).
            tcpLenBuf = ""
            DO WHILE LEN(tcpLenBuf) < 2
                ERRCLEAR
                LOCAL tmpLen AS STRING
                TCP RECV #fNum, 2 - LEN(tcpLenBuf), tmpLen
                ' 2.7: Distinguish timeout (ERR=24) from real disconnect.
                ' On timeout, keep the connection - just retry the read.
                ' On real disconnect (ERR<>24 or LEN=0), exit to reconnect.
                IF ERR THEN
                    IF ERR = 24 THEN
                        ' Timeout - connection still alive, just no data yet
                        ERRCLEAR
                        ' 3.2-fix: Feed consumer during TCP idle (like UDP path)
                        GOSUB FeedWaveOut
                        ' 2.2-fix: If FeedWaveOut set bReopenWaveOut, exit inner loop
                        ' to reach reopen_check in main loop.
                        EnterCriticalSection g_csDev
                        IF g_Devs(idx).dwRunning = 0 THEN stillRun = 0 ELSE stillRun = 1
                        IF g_Devs(idx).bReopenWaveOut THEN stillRun = 2
                        LeaveCriticalSection g_csDev
                        IF stillRun = 0 THEN EXIT DO
                        IF stillRun = 2 THEN EXIT DO   ' bReopenWaveOut -> reopen_check
                        ITERATE DO
                    END IF
                    EXIT DO   ' real error - reconnect
                END IF
                IF LEN(tmpLen) = 0 THEN EXIT DO
                ' 5.1-fix: defensive clip (match payload read)
                IF LEN(tmpLen) > (2 - LEN(tcpLenBuf)) THEN tmpLen = LEFT$(tmpLen, 2 - LEN(tcpLenBuf))
                tcpLenBuf = tcpLenBuf & tmpLen
            LOOP
            ' 2.2d: If we exited inner loop due to bReopenWaveOut (stillRun=2),
            ' skip reconnect and go straight to reopen_check.
            IF stillRun = 2 THEN GOTO reopen_check

            IF ERR OR LEN(tcpLenBuf) < 2 THEN
                ' Connection broken / closed by ESP. Reconnect.
                INCR audErrCount
                IF audErrCount = 1 OR (audErrCount MOD 30) = 0 THEN
                    AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                           ": TCP RECV len-prefix err=" & TRIM$(STR$(ERR)) & _
                           " got=" & TRIM$(STR$(LEN(tcpLenBuf))) & _
                           " (count=" & TRIM$(STR$(audErrCount)) & ")"
                END IF
                ' Close + retry connect (ESP may have restarted stream).
                TCP CLOSE #fNum
                ERRCLEAR
                INCR tcpReconnectCount
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & ": TCP reconnecting to " & tcpHost & ":" & TRIM$(STR$(bindPort)) & " (attempt " & TRIM$(STR$(tcpReconnectCount)) & ")"
                LOCAL reconOk AS LONG
                LOCAL reconTries AS LONG
                reconOk = 0
                reconTries = 0
                DO
                    EnterCriticalSection g_csDev
                    stillRun = g_Devs(idx).dwRunning
                    LeaveCriticalSection g_csDev
                    IF stillRun = 0 THEN EXIT DO
                    EnterCriticalSection g_csFile
                    fNum = FREEFILE
                    ERRCLEAR
                    TCP OPEN PORT bindPort AT tcpHost AS #fNum TIMEOUT 2000
                    LeaveCriticalSection g_csFile
                    IF ERR = 0 THEN reconOk = 1 : EXIT DO
                    INCR reconTries
                    ' Log every 5th failed attempt to show we're still trying
                    IF (reconTries MOD 5) = 1 THEN
                        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                               ": TCP connect failed (try " & TRIM$(STR$(reconTries)) & _
                               ", err=" & TRIM$(STR$(ERR)) & ") - retrying"
                    END IF
                    SLEEP 1000
                LOOP
                IF reconOk = 0 THEN EXIT DO   ' stream stopped
                ' A1: update fAudioFile so StopStream sees the new file number
                EnterCriticalSection g_csDev
                g_Devs(idx).fAudioFile = fNum
                LeaveCriticalSection g_csDev
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": TCP reconnected (attempt " & TRIM$(STR$(tcpReconnectCount)) & ")"
                ResetPipelineState
                ITERATE DO
            END IF
            ' Decode big-endian u16 length.
            tcpPktLen = (ASC(tcpLenBuf, 1) * 256&) + ASC(tcpLenBuf, 2)
            ' 2.1-fix: 1416 was UDP-MTU-based; TCP has no MTU limit.
            ' PCM stereo 48kHz/20ms = 3856B, 24-bit = 5776B. Use 16384 ceiling.
            IF tcpPktLen < %PKT_HDR_SZ OR tcpPktLen > 16384 THEN
                ' Invalid frame — resync by closing+reconnecting (rare).
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": TCP invalid frame len=" & TRIM$(STR$(tcpPktLen)) & ", resyncing"
                TCP CLOSE #fNum
                ERRCLEAR
                SLEEP 500
                DO
                    EnterCriticalSection g_csDev
                    stillRun = g_Devs(idx).dwRunning
                    LeaveCriticalSection g_csDev
                    IF stillRun = 0 THEN EXIT DO
                    EnterCriticalSection g_csFile
                    fNum = FREEFILE
                    ERRCLEAR
                    TCP OPEN PORT bindPort AT tcpHost AS #fNum TIMEOUT 2000
                    LeaveCriticalSection g_csFile
                    IF ERR = 0 THEN EXIT DO
                    SLEEP 1000
                LOOP
                ' A1: update fAudioFile with new fNum from reconnect
                EnterCriticalSection g_csDev
                g_Devs(idx).fAudioFile = fNum
                LeaveCriticalSection g_csDev
                ResetPipelineState
                ITERATE DO
            END IF
            ' 2) Read tcpPktLen bytes (payload = pkt_header + data).
            ' into it. The previous code used string concatenation in the
            ' loop (recvBuf = recvBuf & tcpTmp), which reallocates + copies
            ' the entire string on every iteration. With 1-byte reads (high
            ' fragmentation on WiFi), reading a 1400-byte frame is O(n^2).
            ' The previous order (alloc then update) allocated the buffer
            ' with the STALE tcpNeed (0 on the first frame, previous-frame
            ' size on subsequent frames). The POKE$ below then wrote
            ' tcpPktLen bytes into a smaller buffer -> heap corruption.
            tcpNeed = tcpPktLen
            recvBuf = SPACE$(tcpNeed)
            tcpGot = 0
            DO WHILE tcpGot < tcpNeed
                ERRCLEAR
                TCP RECV #fNum, (tcpNeed - tcpGot), tcpTmp
                IF ERR OR LEN(tcpTmp) = 0 THEN
                    ' Partial frame - connection broke mid-read. Reconnect.
                    EXIT DO
                END IF
                ' 2-defensive: clip to requested size (PB should never return more,
                ' but guard against POKE$ overflow if it ever does)
                IF LEN(tcpTmp) > (tcpNeed - tcpGot) THEN tcpTmp = LEFT$(tcpTmp, tcpNeed - tcpGot)
                POKE$ STRPTR(recvBuf) + tcpGot, tcpTmp
                tcpGot = tcpGot + LEN(tcpTmp)
            LOOP
            IF tcpGot < tcpNeed THEN recvBuf = LEFT$(recvBuf, tcpGot)
            IF tcpGot < tcpNeed THEN
                ' Reconnect on partial read.
                INCR audErrCount
                TCP CLOSE #fNum
                ERRCLEAR
                DO
                    EnterCriticalSection g_csDev
                    stillRun = g_Devs(idx).dwRunning
                    LeaveCriticalSection g_csDev
                    IF stillRun = 0 THEN EXIT DO
                    EnterCriticalSection g_csFile
                    fNum = FREEFILE
                    ERRCLEAR
                    TCP OPEN PORT bindPort AT tcpHost AS #fNum TIMEOUT 2000
                    LeaveCriticalSection g_csFile
                    IF ERR = 0 THEN EXIT DO
                    SLEEP 1000
                LOOP
                ' A1: update fAudioFile with new fNum from reconnect
                EnterCriticalSection g_csDev
                g_Devs(idx).fAudioFile = fNum
                LeaveCriticalSection g_csDev
                ResetPipelineState
                ITERATE DO
            END IF
            fromIP = devIP
            fromPort = bindPort
        ELSE
            ' ---- UDP datagram receive (original path) ----
            ' 2.3-fix: Refresh devIP from g_Devs in case ESP changed IP (DHCP).
            ' Old code used stale devIP captured at thread start -> audio ignored.
            EnterCriticalSection g_csDev
            devIP = g_Devs(idx).dwIP
            LeaveCriticalSection g_csDev
            UDP RECV #fNum, FROM fromIP, fromPort, recvBuf
            IF ERR THEN
                recvBuf = ""   ' 3.1-fix: clear to prevent stale data on fallthrough
                INCR audErrCount
                IF audErrCount = 1 OR (audErrCount MOD 5000) = 0 THEN
                    AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                           ": UDP RECV err=" & TRIM$(STR$(ERR)) & _
                           " (count=" & TRIM$(STR$(audErrCount)) & ") - no data?"
                END IF
                GOSUB FeedWaveOut
                ' 2.2-fix: If FeedWaveOut set bReopenWaveOut, GOTO reopen_check
                ' (not fallthrough - recvBuf='' would ITERATE before reaching reopen)
                EnterCriticalSection g_csDev
                IF g_Devs(idx).bReopenWaveOut THEN
                    LeaveCriticalSection g_csDev
                    GOTO reopen_check
                END IF
                LeaveCriticalSection g_csDev
                SLEEP 1
                ITERATE DO
            END IF
        END IF

        IF LEN(recvBuf) < %PKT_HDR_SZ THEN ITERATE DO

        ' 5.1: Verify UDP source IP matches devIP. Audio port (5000+idx) is
        ' easily discovered; without this check any host can inject audio.
        ' TCP path is safe (fromIP=devIP by connect). Only UDP needs this.
        IF devTransport = 0 AND fromIP <> devIP THEN
            INCR audErrCount
            IF audErrCount = 1 OR (audErrCount MOD 1000) = 0 THEN
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": UDP packet from wrong IP " & FormatIP(fromIP) & _
                       " (expected " & FormatIP(devIP) & ") - ignored"
            END IF
            ITERATE DO
        END IF

        ' DIAG: log first received packet
        INCR audRecvCount
        IF audRecvCount = 1 THEN
            LOCAL b0 AS LONG, b1 AS LONG, b2 AS LONG, b3 AS LONG
            LOCAL b4 AS LONG, b5 AS LONG, b6 AS LONG, b7 AS LONG, b8 AS LONG
            b0 = PEEK(BYTE, STRPTR(recvBuf) + 0)
            b1 = PEEK(BYTE, STRPTR(recvBuf) + 1)
            b2 = PEEK(BYTE, STRPTR(recvBuf) + 2)
            b3 = PEEK(BYTE, STRPTR(recvBuf) + 3)
            b4 = PEEK(BYTE, STRPTR(recvBuf) + 4)
            b5 = PEEK(BYTE, STRPTR(recvBuf) + 5)
            b6 = PEEK(BYTE, STRPTR(recvBuf) + 6)
            b7 = PEEK(BYTE, STRPTR(recvBuf) + 7)
            b8 = PEEK(BYTE, STRPTR(recvBuf) + 8)
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": FIRST pkt len=" & TRIM$(STR$(LEN(recvBuf))) & _
                   " from " & FormatIP(fromIP) & ":" & TRIM$(STR$(fromPort))
            AddLog "[AUD]   bytes[0..8]= " & TRIM$(STR$(b0)) & " " & TRIM$(STR$(b1)) & _
                   " " & TRIM$(STR$(b2)) & " " & TRIM$(STR$(b3)) & _
                   " " & TRIM$(STR$(b4)) & " " & TRIM$(STR$(b5)) & _
                   " " & TRIM$(STR$(b6)) & " " & TRIM$(STR$(b7)) & _
                   " " & TRIM$(STR$(b8))
            AddLog "[AUD]   byte[6]=codec=" & TRIM$(STR$(b6)) & _
                   " (expect " & TRIM$(STR$(%CODEC_ID)) & " or " & _
                   TRIM$(STR$(%CODEC_ID_PCM)) & ")  byte[8]=ch=" & _
                   TRIM$(STR$(b8)) & " (expect nCh=" & TRIM$(STR$(nCh)) & ")"
        END IF

        ' Read format fields from packet header (offsets 6,7,8,14).
        '   offset 6:  codec (5=ADPCM, 6=PCM)
        '   offset 7:  sample_rate_enum (0..6 → 8k..48k)
        '   offset 8:  channels (1 or 2)
        '   offset 14: bits (uint16 LE, 16 or 24) - I2S bit depth
        LOCAL pktCodec AS LONG
        LOCAL pktRateEnum AS LONG
        LOCAL pktCh AS LONG
        LOCAL pktBits AS LONG
        pktCodec = PEEK(BYTE, STRPTR(recvBuf) + 6)
        pktRateEnum = PEEK(BYTE, STRPTR(recvBuf) + 7)
        pktCh = PEEK(BYTE, STRPTR(recvBuf) + 8)
        pktBits = PEEK(WORD, STRPTR(recvBuf) + 14)

        ' ---- HARDENING (zero-day defense): validate all packet header fields ----
        ' A malformed/malicious packet could carry out-of-range values. Reject
        ' the packet (ITERATE DO) rather than risk downstream corruption.

        ' Codec: only 5 (ADPCM) and 6 (PCM) supported. Reject garbage.
        IF pktCodec <> %CODEC_ID AND pktCodec <> %CODEC_ID_PCM THEN
            IF audRecvCount <= 3 THEN
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": REJECT pkt#" & TRIM$(STR$(audRecvCount)) & _
                       " codec=" & TRIM$(STR$(pktCodec)) & " (expected 5 or 6)"
            END IF
            ITERATE DO
        END IF

        ' Sample-rate enum: must be 0..6 (7 known EASSP rates). Reject garbage.
        ' A bad enum would silently fall back to 16000 in RateEnumToHz, causing
        ' a format-mismatch with WaveOut -> audio corruption or crash.
        IF pktRateEnum < 0 OR pktRateEnum > 6 THEN
            IF audRecvCount <= 3 THEN
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": REJECT pkt#" & TRIM$(STR$(audRecvCount)) & _
                       " rate_enum=" & TRIM$(STR$(pktRateEnum)) & " (expected 0..6)"
            END IF
            ITERATE DO
        END IF

        ' Bits: 16 or 24 only. Clamp to 16 for any other value (graceful).
        IF pktBits <> 16 AND pktBits <> 24 THEN pktBits = 16
        ' Channels: 1 or 2 only. Clamp (graceful).
        IF pktCh < 1 THEN pktCh = 1
        IF pktCh > 2 THEN pktCh = 2

        ' ---- Startup click suppression: skip first ~1 second of audio ----
        ' On the first packet, read frame_ms (offset 9) and compute how many
        ' frames to skip (STARTUP_SKIP_MS / frame_ms). Then for each of those
        ' frames, consume the packet (keeps the socket drained, sends ACK-ish
        ' heartbeat via normal stats update) but do NOT decode or play it.
        ' This discards the mic's startup transient + stale DMA data.
        ' When the skip completes, arm a 1-second fade-in to smooth the
        ' silence→signal transition (see fadeSamplesRemaining below).
        IF skipFramesRemaining = 999 THEN
            ' First valid packet — compute skip count from frame_ms
            skipFrameMs = PEEK(BYTE, STRPTR(recvBuf) + 9)
            IF skipFrameMs < 1 THEN skipFrameMs = 20  ' guard against 0/garbage
            skipFramesRemaining = %STARTUP_SKIP_MS \ skipFrameMs
            ' Compute fade-in sample count from the actual sample rate.
            ' 3.3-fix: multiply by nCh (stereo interleaved = 2x samples per frame)
            ' 3.2-fix: use actualSmpRate (after EspActualRate drift correction)
            fadeSamplesTotal = (CLNG(actualSmpRate) * nCh * %FADE_IN_MS) \ 1000
            IF fadeSamplesTotal < 1 THEN fadeSamplesTotal = 16000  ' guard
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": startup skip - discarding first " & _
                   TRIM$(STR$(skipFramesRemaining)) & " frames (" & _
                   TRIM$(STR$(%STARTUP_SKIP_MS)) & "ms / " & _
                   TRIM$(STR$(skipFrameMs)) & "ms), then " & _
                   TRIM$(STR$(%FADE_IN_MS)) & "ms fade-in (" & _
                   TRIM$(STR$(fadeSamplesTotal)) & " samples) to suppress mic transient"
        END IF
        IF skipFramesRemaining > 0 THEN
            DECR skipFramesRemaining
            ' Track seq during skip so loss detection works on first play packet.
            LOCAL skipSeqNum AS WORD
            skipSeqNum = CVWRD(PEEK$(STRPTR(recvBuf), 2))
            lastSeq = skipSeqNum
            INCR pktRecv
            EnterCriticalSection g_csDev
            g_Devs(idx).dwPktRecv   = pktRecv
            g_Devs(idx).wLastSeq    = lastSeq
            g_Devs(idx).dwLastPktTick = GetTickCount()
            LeaveCriticalSection g_csDev
            ' When the LAST skipped frame is consumed, arm the fade-in so
            ' the NEXT frame (first played) starts the ramp.
            IF skipFramesRemaining = 0 THEN
                fadeSamplesRemaining = fadeSamplesTotal
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": skip complete, fade-in armed (" & _
                       TRIM$(STR$(fadeSamplesRemaining)) & " samples / " & _
                       TRIM$(STR$(%FADE_IN_MS)) & "ms)"
            END IF
            ITERATE DO
        END IF

        ' ---- WAV dump mode ----
        ' Write decoded PCM audio to a WAV file.
        ' For PCM codec: raw payload is already PCM, write directly.
        ' For ADPCM codec: decoded PCM is in pcmPtrs after decode (handled below).
        ' First packet: determine format and open WAV file with header.
        ' 2.2-fix: Detect format change BEFORE dump. If format changed, skip dump
        ' write this iteration (the format-change block below will close the old
        ' WAV and reopen with new format on the next packet).
        LOCAL bFmtChangedThisPkt AS LONG
        bFmtChangedThisPkt = 0
        IF pktCodec <> devCodec OR pktCh <> nCh OR _
           pktRateEnum <> curRateEnum OR pktBits <> devBits THEN
            bFmtChangedThisPkt = 1
        END IF

        IF g_bDumping AND bFmtChangedThisPkt = 0 THEN
            ' ToggleDump to the currently-selected device's IP (or 0 if
            ' none selected, meaning "dump all" - legacy behavior). Without
            ' this filter, 2+ concurrent AudioThreads would all write to
            ' the single global g_hDumpFile -> byte-interleaved PCM from
            ' different devices / sample rates / channel counts -> the
            ' resulting WAV is unplayable garbage.
            IF g_dumpIp = 0 OR devIP = g_dumpIp THEN
            ' AudioThreads (two streaming devices) hitting this block
            ' simultaneously would both read g_dumpWavReady=0, both set
            ' g_dumpFileIdx=1, both call OpenNewDumpFile on the same path ->
            ' the second OPEN either fails or opens a second handle to the
            ' same file -> subsequent PUT$ calls interleave -> WAV corruption.
            EnterCriticalSection g_csDump
            IF g_dumpWavReady = 0 THEN
                ' First packet - set format and open file
                g_dumpCodec = pktCodec
                g_dumpSampleRate = RateEnumToHz(pktRateEnum)
                g_dumpChannels = pktCh
                IF pktCodec = %CODEC_ID_PCM THEN
                    g_dumpBits = pktBits
                ELSE
                    g_dumpBits = 16  ' ADPCM decodes to 16-bit
                END IF
                g_dumpFileIdx = 1
                OpenNewDumpFile
            END IF
            LeaveCriticalSection g_csDump

            ' For PCM codec: write raw payload directly (it's already PCM)
            IF pktCodec = %CODEC_ID_PCM AND g_hDumpFile THEN
                LOCAL dumpLen AS LONG
                dumpLen = LEN(recvBuf) - %PKT_HDR_SZ
                IF dumpLen > 0 THEN
                    EnterCriticalSection g_csDump
                    IF g_hDumpFile THEN
                        PUT$ #g_hDumpFile, PEEK$(STRPTR(recvBuf) + %PKT_HDR_SZ, dumpLen)
                        g_dumpDataSize = g_dumpDataSize + dumpLen
                        ' Auto-split at 1 GB
                        IF g_dumpDataSize > 1073741780 THEN
                            UpdateWavHeader
                            CLOSE g_hDumpFile
                            g_hDumpFile = 0
                            INCR g_dumpFileIdx
                            OpenNewDumpFile
                        END IF
                    END IF
                    LeaveCriticalSection g_csDump
                END IF
            END IF
            ' For ADPCM: decoded PCM is written after decode (see below)
            END IF   ' close the g_dumpIp filter IF
        END IF

        ' ---- Format change detection ----
        ' Compare packet format with the format currently open in WaveOut.
        ' If any field changed (codec, channels, sample rate, bits), close
        ' WaveOut and reopen with the new format. This allows on-the-fly
        ' format changes via AT+HOTRESTART on the ESP side.
        IF pktCodec <> devCodec OR pktCh <> nCh OR _
           pktRateEnum <> curRateEnum OR pktBits <> devBits THEN
            LOCAL sOldFmt AS STRING, sNewFmt AS STRING
            sOldFmt = CodecName(devCodec) & " " & TRIM$(STR$(devSmpRate)) & "Hz " & _
                      TRIM$(STR$(nCh)) & "ch " & TRIM$(STR$(devBits)) & "-bit"
            sNewFmt = CodecName(pktCodec) & " " & TRIM$(STR$(RateEnumToHz(pktRateEnum))) & "Hz " & _
                      TRIM$(STR$(pktCh)) & "ch " & TRIM$(STR$(pktBits)) & "-bit"
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": format changed (" & sOldFmt & " -> " & sNewFmt & ") - reopening WaveOut"

            ' Close old WaveOut
            IF waveOut THEN
                waveOutReset waveOut
                FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
                    waveOutUnprepareHeader waveOut, whdrs(wIdx2), SIZEOF(WAVEHDR)
                NEXT wIdx2
                waveOutClose waveOut
                waveOut = 0
            END IF

            ' 1.5: Close WAV dump on format change (old header = corrupted file)
            EnterCriticalSection g_csDump
            IF g_hDumpFile THEN
                UpdateWavHeader : CLOSE g_hDumpFile : g_hDumpFile = 0
            END IF
            g_dumpWavReady = 0
            LeaveCriticalSection g_csDump

            ' Update local format variables from packet
            devCodec = pktCodec
            nCh = pktCh
            curRateEnum = pktRateEnum
            devBits = pktBits
            devSmpRate = RateEnumToHz(curRateEnum)

            ' Update g_Devs so UI shows the new format
            EnterCriticalSection g_csDev
            g_Devs(idx).dwCodec = devCodec
            g_Devs(idx).dwChannels = nCh
            g_Devs(idx).dwSmpRate = devSmpRate
            g_Devs(idx).dwBits = devBits
            LeaveCriticalSection g_csDev

            ' Jump to WaveOut reopen (resets decoder, sets up WAVEFORMATEX,
            ' opens WaveOut, prepares headers)
            GOTO reopen_waveout
        END IF

        ' ---- Sequence number + loss detection (MOVED HERE from pcm_ready) ----
        ' Compute lostPkt/oooPkt NOW, before PLC and decode, so PLC can
        ' insert a concealment frame for the lost packet(s) BEFORE the current
        ' packet is decoded and played.

        ' PREVIOUS BUG: this was computed in pcm_ready (END of iteration).
        ' lostPkt from iteration B (packet N+1, after losing N) was used in
        ' iteration C (packet N+2) — so PLC fired one packet LATE, replacing
        ' the good packet N+2 with a copy of N+1 instead of concealing the
        ' actually-lost packet N. Net effect: 2 effective losses (N dropped
        ' as silence + N+2 overwritten with a repeat) instead of 1 concealed.
        ' Now PLC fires in the SAME iteration that detects the loss, concealing
        ' N while still decoding N+1 normally → 0 effective losses on a single
        ' packet drop.
        seqNum = CVWRD(PEEK$(STRPTR(recvBuf), 2))
        lostPkt = 0
        oooPkt  = 0
        LOCAL seqDiff AS LONG
        seqDiff = 0
        IF pktRecv > 0 THEN
            seqDiff = (CLNG(seqNum) - CLNG(lastSeq) - 1) AND &HFFFF&
            IF seqDiff > 0 THEN
                IF seqDiff < 32768 THEN
                    lostPkt = seqDiff
                ELSE
                    oooPkt = 1
                END IF
            END IF
        END IF

        ' ROBUST DETECTION (FIX: false-positive at natural seq overflow):
        ' The previous heuristic   ' lastSeq > 1000 AND seqNum < 100' fired
        ' FALSE POSITIVE at natural seq_num overflow (every ~11 min at
        ' 100 pkt/s) - when seq wraps 65535->0, the heuristic reset the
        ' jitter buffer, causing an unwanted audio gap.

        ' New heuristic uses BOTH seq_num AND timestamp_ms from the packet
        ' header (offset 2, uint32). ESP sets timestamp_ms = frame_count *
        ' frame_ms, RESETS TO 0 on every start_streaming. uint32_t wraps at
        ' ~49 days, so in normal operation timestamp_ms is monotonic.

        ' Three conditions must ALL be true:
        '   1. pktTimestamp < 1000 ms -- packet is from a fresh stream (<1s in)
        '   2. lastSeq > 1000          -- old stream had run a while
        '   3. seqDiff > 32768         -- seq jumped BACKWARDS in modular
        '                               arithmetic (rules out natural overflow,
        '                               where seqDiff = 0 or small N for N lost)

        ' Why condition 3 rules out natural overflow:
        '   - Normal wrap (65535 -> 0, no loss): seqDiff = 0
        '   - Wrap with N lost packets (65530 -> 5): seqDiff = N (small)
        '   - True HOTRESTART (30000 -> 0): seqDiff = 35534 (large, >32768)
        ' Only a true restart produces a large backward modular jump.
        LOCAL pktTimestamp AS DWORD
        pktTimestamp = PEEK(DWORD, STRPTR(recvBuf) + 2)
        IF pktRecv > 0 AND lastSeq > 1000 AND seqDiff > 32768 AND pktTimestamp < 1000 THEN
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": HOTRESTART detected (seq " & TRIM$(STR$(lastSeq)) & _
                   " -> " & TRIM$(STR$(seqNum)) & ") - resetting jitter buffer"
            ' Unified pipeline reset via macro.
            ResetPipelineState
            ITERATE DO   ' 2.4-fix: skip current packet, let next iter do startup skip
        END IF

        ' ---- LATE PACKET DISCARD (out-of-order) ----
        ' If packet is out-of-order (seq <= lastSeq = already played),
        ' DISCARD it. Playing a late packet produces a backwards audio segment
        ' -> click. This is what WebRTC/GStreamer do.
        IF oooPkt THEN
            EnterCriticalSection g_csDev
            g_Devs(idx).dwPktOOO    = g_Devs(idx).dwPktOOO + 1
            g_Devs(idx).dwLastPktTick = GetTickCount()
            LeaveCriticalSection g_csDev
            ITERATE DO
        END IF
        lastSeq = seqNum

        ' ---- BWSOLA PLC loss detection ----
        ' BWSOLA is BILATERAL: needs L_WSOLA (prev good packet) AND R_WSOLA (next good packet).
        ' On loss detection (lostPkt > 0): copy lastPcm into plcLBuf (stable storage since
        ' decodePtr gets overwritten on next decode) and set plcPending. Reconstruction runs
        ' AFTER current packet is decoded, using plcLBuf (L) + decodePtr (R).
        ' Multi-packet bursts get 1 BWSOLA frame + up to 3 simple repeats for additional losses.

        IF lostPkt > 0 AND lastPcmPtr <> 0 AND lastPcmLen > 0 AND ringPtr <> 0 THEN
            ' Save L_WSOLA = lastPcm into plcLBuf (stable storage, bufSz bytes).
            IF %BWSOLA_PLC_ENABLE AND plcLBuf <> 0 AND lastPcmLen <= bufSz THEN
                CopyMemory plcLBuf, lastPcmPtr, lastPcmLen
                plcLPtr = plcLBuf
                plcLLen = lastPcmLen
                plcPending = lostPkt
                IF g_bVerbose THEN
                    AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & ": BWSOLA PLC pending (lost=" & TRIM$(STR$(lostPkt)) & " len=" & TRIM$(STR$(lastPcmLen)) & ")"
                END IF
            ELSE
                ' 3.5-fix: Fallback (BWSOLA disabled or alloc failed): repeat
                ' lastPcm lostPkt times (was only 1x, leaving gaps on multi-loss).
                ' 1.5-fix: Cap to 4 frames max (lostPkt can be up to 32767 -> would
                ' flood ring buffer + CPU. Stats still record full loss count below).
                ' This runs BEFORE decode, so lastPcmPtr holds the previous good packet.
                LOCAL plcRep AS LONG
                LOCAL concealLoss AS LONG
                concealLoss = lostPkt
                IF concealLoss > 4 THEN concealLoss = 4
                FOR plcRep = 1 TO concealLoss
                    IF RingWrite(ringPtr, ringCap, ringW, ringR, ringUsed, _
                                 lastPcmPtr, lastPcmLen) THEN
                        INCR overflowCount
                        IF overflowCount <= 3 OR (overflowCount MOD 100) = 0 THEN
                            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                                   ": PLC(repeat) RingWrite dropped oldest (overflow) overflows=" & _
                                   TRIM$(STR$(overflowCount)) & " used=" & TRIM$(STR$(ringUsed))
                        END IF
                    END IF
                NEXT plcRep
            END IF
        END IF

        ' ---- Raw PCM passthrough (codec == 6) ----
        ' ESP sends raw PCM without compression.
        '   16-bit: payload is int16 samples - copy directly (bit-perfect).
        '   24-bit: payload is 3 bytes/sample (LE, sign-extended 24-bit).
        '     If WaveOut is 24-bit (sound card supports it): copy directly.
        '     If WaveOut is 16-bit (fallback): convert 24→16 by taking
        '       mid+hi bytes (>>8), losing the low 8 bits.
        IF pktCodec = %CODEC_ID_PCM THEN
            IF devBits = 24 AND outBits = 16 THEN
                ' 24-bit ESP → 16-bit WaveOut: downsample by >>8.
                ' Take bytes [1,2] of each 3-byte sample (mid+hi).
                LOCAL pcm24Len AS LONG
                LOCAL pcm24Src AS BYTE PTR
                LOCAL pcm16Dst AS BYTE PTR
                LOCAL si24 AS LONG
                pcm24Len = LEN(recvBuf) - %PKT_HDR_SZ
                pcm24Src = STRPTR(recvBuf) + %PKT_HDR_SZ
                pcm16Dst = decodePtr
                LOCAL outIdx AS LONG
                outIdx = 0
                FOR si24 = 0 TO pcm24Len - 3 STEP 3
                    @pcm16Dst[outIdx] = @pcm24Src[si24 + 1]   ' mid byte → lo
                    INCR outIdx
                    @pcm16Dst[outIdx] = @pcm24Src[si24 + 2]   ' hi byte → hi
                    INCR outIdx
                    IF outIdx >= bufSz - 1 THEN EXIT FOR
                NEXT si24
                pcmLen = outIdx
            ELSE
                ' Bit-perfect passthrough (16->16 or 24->24): copy directly.
                pcmLen = LEN(recvBuf) - %PKT_HDR_SZ
                IF pcmLen > bufSz THEN
                    AddLog "[AUD] PCM frame " & TRIM$(STR$(pcmLen)) & _
                           " bytes exceeds bufSz " & TRIM$(STR$(bufSz)) & _
                           " - truncating (check ESP frame_ms)"
                    pcmLen = bufSz
                END IF
                IF pcmLen > 0 THEN
                    POKE$ decodePtr, PEEK$(STRPTR(recvBuf) + %PKT_HDR_SZ, pcmLen)
                END IF
            END IF
            GOTO pcm_ready
        END IF

        ' From here on: ADPCM decoding path (codec == 5)
        IF LEN(recvBuf) < %PKT_HDR_SZ + %DVI4_HDR_SZ THEN ITERATE DO

        IF nCh = 1 THEN
            ' ---- MONO: single DVI4 block ----
            adpcmLen = LEN(recvBuf) - %PKT_HDR_SZ - %DVI4_HDR_SZ
            IF adpcmLen <= 0 THEN ITERATE DO

            ' Clip adpcmLen to what the PCM buffer can hold.
            ' DecodeImaAdpcm writes srcLen*4 bytes (2 samples x 2 bytes per
            ' input byte); the buffer is WAVE_BUF_SZ bytes. Without this
            ' clip, large frames (e.g. 48kHz/40ms -> adpcmLen=960 -> 3840
            ' bytes) overflow the 1920-byte heap buffer.
            IF adpcmLen > (bufSz \ 4) THEN adpcmLen = bufSz \ 4

            predictor = CVI(PEEK$(STRPTR(recvBuf) + 16, 2))
            stepIndex = PEEK(BYTE, STRPTR(recvBuf) + 18)

            pcmLen = adpcmLen * 4
            IF pcmLen > bufSz THEN pcmLen = bufSz

            DecodeImaAdpcm STRPTR(recvBuf) + 20, adpcmLen, decodePtr, predictor, stepIndex

        ELSE
            ' ---- STEREO: two independent DVI4 blocks ----
            ' [pkt_header 16] [dvi4_hdr_L 4] [adpcm_L] [dvi4_hdr_R 4] [adpcm_R]
            ' BUGFIX: subtract BOTH DVI4 headers, not just one.
            ' DVI4 block was seeked at the clipped position (middle of L data)
            ' instead of after the full L block on the wire. Now: clip only for
            ' decode, but offsetR uses the original wire length.
            LOCAL trueAdpcmL AS LONG
            trueAdpcmL = (LEN(recvBuf) - %PKT_HDR_SZ - 2 * %DVI4_HDR_SZ) \ 2
            IF trueAdpcmL <= 0 THEN ITERATE DO
            adpcmLenL = trueAdpcmL
            IF adpcmLenL > (bufSz \ 8) THEN adpcmLenL = bufSz \ 8

            ' Decode left channel (clipped length, starts at +20)
            predictor = CVI(PEEK$(STRPTR(recvBuf) + 16, 2))
            stepIndex = PEEK(BYTE, STRPTR(recvBuf) + 18)
            DecodeImaAdpcm STRPTR(recvBuf) + 20, adpcmLenL, decodePtr, predictor, stepIndex
            pcmLenL = adpcmLenL * 4

            ' Decode right channel: offsetR after FULL left ADPCM on the wire
            LOCAL offsetR AS LONG
            offsetR = %PKT_HDR_SZ + %DVI4_HDR_SZ + trueAdpcmL
            IF offsetR + %DVI4_HDR_SZ > LEN(recvBuf) THEN ITERATE DO

            predictor2 = CVI(PEEK$(STRPTR(recvBuf) + offsetR, 2))
            stepIndex2 = PEEK(BYTE, STRPTR(recvBuf) + offsetR + 2)
            DecodeImaAdpcm STRPTR(recvBuf) + offsetR + 4, adpcmLenL, decodePtr + pcmLenL, predictor2, stepIndex2
            pcmLenR = adpcmLenL * 4

            ' Interleave L,R into the PCM buffer
            ' Left is at decodePtr, Right is at decodePtr + pcmLenL
            ' We need to interleave: L0,R0,L1,R1...
            ' BUGFIX: pL and pOut point to the SAME buffer (decodePtr).
            ' Writing R0 to pOut[1] overwrites pL[1] before it's read!
            ' Fix: Copy BOTH channels to temp areas before interleaving.
            ' Buffer layout: [L 640B] [R 640B] [tempR 640B] [tempL 640B] [unused]
            ' Total: 2560B out of 3840B available.
            LOCAL pL AS INTEGER PTR
            LOCAL pR AS INTEGER PTR
            LOCAL pOut AS INTEGER PTR
            LOCAL nSamp AS LONG
            LOCAL si AS LONG
            LOCAL pTempL AS INTEGER PTR
            LOCAL pTempR AS INTEGER PTR
            pL = decodePtr
            pR = decodePtr + pcmLenL
            nSamp = pcmLenL \ 2
                      ' Copy both channels to safe temp areas beyond the decoded region.
            ' pTempR at byte offset 2*pcmLenL, pTempL at byte offset 3*pcmLenL
            ' (offsets scale with pcmLenL; only safe because bufSz = 4*WAVE_BUF_SZ for stereo)
            ' 1.7-fix: Use separate interleaveBuf (heap overflow fix per Mikhail #1)
            ' Old code wrote pTempR/L at decodePtr+2/3*pcmLenL which overflows
            ' decodePtr (bufSz=7680) when pcmLenL > 1920. interleaveBuf is its own bufSz.
            pTempR = interleaveBuf
            pTempL = interleaveBuf + pcmLenL

            FOR si = 0 TO nSamp - 1
                @pTempR[si] = @pR[si]
                @pTempL[si] = @pL[si]
            NEXT si
            ' Interleave from temp buffers (safe, no overwriting)
            pOut = decodePtr
            FOR si = 0 TO nSamp - 1
                @pOut = @pTempL[si]
                INCR pOut
                @pOut = @pTempR[si]
                INCR pOut
            NEXT si

            pcmLen = pcmLenL + pcmLenR
            ' Bounds check for stereo - clip to allocated buffer size
            IF pcmLen > bufSz THEN pcmLen = bufSz
        END IF

pcm_ready:
        INCR pktRecv
        IF pktRecv = 1 THEN
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": FIRST pkt ACCEPTED (pktRecv=1) - audio pipeline active"
        END IF

        ' ---- Stats update ----
        ' NOTE: seqNum / lostPkt / oooPkt / lastSeq are now computed at the
        ' TOP of the loop (before PLC + decode), not here. Out-of-order
        ' packets are already discarded via ITERATE DO up there. By the time
        ' we reach pcm_ready, lostPkt holds the gap count for THIS packet
        EnterCriticalSection g_csDev
        g_Devs(idx).dwPktRecv   = pktRecv
        g_Devs(idx).wLastSeq    = lastSeq
        ' LONG (pcmLen) to the QUAD counter. dwBytesRecv is QUAD (64-bit,
        ' see types.inc) so no 4GB wraparound - the old   ' cosmetic' note is
        ' obsolete since the field type was widened from DWORD to QUAD.
        g_Devs(idx).dwBytesRecv = g_Devs(idx).dwBytesRecv + CQUD(pcmLen)
        g_Devs(idx).dwLastPktTick = GetTickCount()   ' stall detection: alive
        IF lostPkt THEN g_Devs(idx).dwPktLost = g_Devs(idx).dwPktLost + lostPkt
        LeaveCriticalSection g_csDev

        ' ---- WAV dump: ADPCM decoded PCM ----
        ' For ADPCM codec: write decoded 16-bit PCM from decodePtr to WAV file.
        ' (PCM codec is already handled above with raw payload write.)
        ' so only the selected device's decoded PCM is dumped.
        IF g_bDumping AND pktCodec = %CODEC_ID AND g_hDumpFile AND pcmLen > 0 _
           AND (g_dumpIp = 0 OR devIP = g_dumpIp) THEN
            EnterCriticalSection g_csDump
            IF g_hDumpFile THEN
                PUT$ #g_hDumpFile, PEEK$(decodePtr, pcmLen)
                g_dumpDataSize = g_dumpDataSize + pcmLen
                ' Auto-split at 1 GB
                IF g_dumpDataSize > 1073741780 THEN
                    UpdateWavHeader
                    CLOSE g_hDumpFile
                    g_hDumpFile = 0
                    INCR g_dumpFileIdx
                    OpenNewDumpFile
                END IF
            END IF
            LeaveCriticalSection g_csDump
        END IF

        ' ---- BWSOLA PLC reconstruction (bilateral, per Yeh et al. 2013) ----
        ' If plcPending > 0, the PREVIOUS iteration detected a loss and saved
        ' L_WSOLA in plcLBuf. The current packet (just decoded into decodePtr)
        ' is R_WSOLA. Run BWSOLA_PLC_Frame to reconstruct the lost frame, then
        ' RingWrite the concealed frame BEFORE the current packet (ring order:
        ' ..., N-1, concealed-N, N+1, ...). This block runs BEFORE the
        ' save-lastPcm + producer RingWrite below, so the concealed frame is
        ' queued first.
        IF plcPending > 0 AND plcLPtr <> 0 AND plcLLen > 0 AND _
           decodePtr <> 0 AND pcmLen > 0 AND ringPtr <> 0 THEN
            IF %BWSOLA_PLC_ENABLE AND outBits = 16 AND plcOutBuf <> 0 THEN
                LOCAL plcOut AS INTEGER PTR
                LOCAL plcGot AS LONG
                LOCAL plcOutSmp AS LONG
                plcOut = plcOutBuf
                plcOutSmp = plcLLen \ 2     ' output sample count = L sample count
                plcGot = BWSOLA_PLC_Frame(plcLPtr, plcLLen \ 2, _
                                          decodePtr, pcmLen \ 2, _
                                          plcOut, plcOutSmp, nCh, devSmpRate)
                IF plcGot > 0 THEN
                    IF RingWrite(ringPtr, ringCap, ringW, ringR, ringUsed, _
                                 plcOutBuf, plcGot) THEN
                        INCR overflowCount
                        IF overflowCount <= 3 OR (overflowCount MOD 100) = 0 THEN
                            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                                   ": BWSOLA PLC RingWrite dropped oldest (overflow) overflows=" & _
                                   TRIM$(STR$(overflowCount)) & " used=" & TRIM$(STR$(ringUsed))
                        END IF
                    END IF
                    IF g_bVerbose THEN
                        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                               ": BWSOLA PLC reconstructed " & TRIM$(STR$(plcGot)) & _
                               " bytes (L=" & TRIM$(STR$(plcLLen)) & " R=" & TRIM$(STR$(pcmLen)) & ")"
                    END IF
                ELSE
                    ' BWSOLA failed (outLen <= 0 or invalid params) - fallback
                    ' to frame-repeat using the saved L copy in plcLBuf.
                    IF RingWrite(ringPtr, ringCap, ringW, ringR, ringUsed, _
                                 plcLPtr, plcLLen) THEN
                        INCR overflowCount
                    END IF
                    AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                           ": BWSOLA PLC failed (plcGot=0), frame-repeat fallback"
                END IF
            ELSE
                ' BWSOLA disabled or outBits != 16 or no plcOutBuf: fallback
                ' frame-repeat (V2 behavior). plcLPtr = plcLBuf holds the L copy.
                IF RingWrite(ringPtr, ringCap, ringW, ringR, ringUsed, _
                             plcLPtr, plcLLen) THEN
                    INCR overflowCount
                    IF overflowCount <= 3 OR (overflowCount MOD 100) = 0 THEN
                        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                               ": PLC(repeat) RingWrite dropped oldest (overflow) overflows=" & _
                               TRIM$(STR$(overflowCount)) & " used=" & TRIM$(STR$(ringUsed))
                    END IF
                END IF
            END IF
            ' Multi-packet loss (plcPending > 1): simple frame-repeat for
            ' additional lost packets (spec simplification 5: BWSOLA for the
            ' first gap, simple repeat for the rest). Cap at 3 additional
            ' repeats to avoid flooding the ring on large bursts (matches
            ' the paper's 1-4 packet loss test range).
            IF plcPending > 1 THEN
                LOCAL extraLoss AS LONG
                LOCAL extraMax AS LONG
                IF plcPending - 1 > 3 THEN
                    extraMax = 3
                ELSE
                    extraMax = plcPending - 1
                END IF
                FOR extraLoss = 1 TO extraMax
                    IF RingWrite(ringPtr, ringCap, ringW, ringR, ringUsed, _
                                 plcLPtr, plcLLen) THEN
                        INCR overflowCount
                    END IF
                NEXT extraLoss
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": PLC multi-loss " & TRIM$(STR$(plcPending)) & _
                       " pkts: 1 BWSOLA + " & TRIM$(STR$(extraMax)) & " repeat"
            END IF
        END IF
        plcPending = 0

        ' 3.6-fix: lastPcmLen save moved AFTER frame alignment (was before,
        ' causing PLC frame size mismatch with aligned frameBytes).

        ' ---- Fade-in ramp (startup click suppression, 2nd second) ----
        ' Applied AFTER the 1-second skip completes. Linearly scales sample
        ' amplitude from 0 to 1 over fadeSamplesRemaining samples (1 second
        ' at the current sample rate). Operates IN-PLACE on decodePtr.
        ' Handles frame boundaries: if the frame is shorter than the remaining
        ' ramp, fades the whole frame and continues on the next. If longer,
        ' fades the first N samples and leaves the rest at full amplitude.
        ' 16-bit only (ADPCM decodes to 16-bit; 24-bit PCM skips — acceptable
        ' since the skip already removed the main transient).
        IF fadeSamplesRemaining > 0 AND pcmLen > 0 AND outBits = 16 THEN
            ' Linear has a derivative discontinuity at the start (slope jumps
            ' from 0 to 1/N) which itself produces a faint click. Raised-cosine
            ' = 0.5 * (1 - COS(pi * progress)) has zero slope at both ends,
            ' eliminating the discontinuity. progress = (samples_faded_so_far)
            ' / fadeSamplesTotal, in [0,1]. COS cost is negligible (~16k calls
            ' for a 1s fade @ 16kHz, < 1ms total).
            LOCAL pFade AS INTEGER PTR
            LOCAL fadeFrameSamples AS LONG
            LOCAL fadeI AS LONG
            LOCAL fadeScale AS DOUBLE
            LOCAL fadeProg AS DOUBLE
            pFade = decodePtr
            fadeFrameSamples = pcmLen \ 2   ' 16-bit samples in this frame
            IF fadeFrameSamples < fadeSamplesRemaining THEN
                ' Frame shorter than remaining ramp — fade whole frame,
                ' continue ramp on next frame
                FOR fadeI = 0 TO fadeFrameSamples - 1
                    fadeProg = (fadeSamplesTotal - fadeSamplesRemaining + fadeI) / fadeSamplesTotal
                    fadeScale = 0.5# * (1.0# - COS(3.141592653589793## * fadeProg))
                    @pFade[fadeI] = CINT(@pFade[fadeI] * fadeScale)
                NEXT fadeI
                fadeSamplesRemaining = fadeSamplesRemaining - fadeFrameSamples
            ELSE
                ' Ramp completes within this frame — fade first N samples,
                ' leave the rest at full amplitude
                FOR fadeI = 0 TO fadeSamplesRemaining - 1
                    fadeProg = (fadeSamplesTotal - fadeSamplesRemaining + fadeI) / fadeSamplesTotal
                    fadeScale = 0.5# * (1.0# - COS(3.141592653589793## * fadeProg))
                    @pFade[fadeI] = CINT(@pFade[fadeI] * fadeScale)
                NEXT fadeI
                fadeSamplesRemaining = 0
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": fade-in complete, full amplitude"
            END IF
        END IF

        ' ==================================================================
        ' PRODUCER: decoded PCM -> ring
        ' ==================================================================
        IF pcmLen > 0 AND ringPtr <> 0 THEN
            ' Lock frame size from the first played frame. Used by the
            ' consumer (FeedWaveOut) to know how many bytes to pull per
            ' waveOutWrite. Format changes trigger reopen_waveout which
            ' resets frameBytes=0, so the next frame re-locks.
            IF frameBytes = 0 THEN
                frameBytes = pcmLen
                AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                       ": frameBytes=" & TRIM$(STR$(frameBytes))
            ELSEIF pcmLen <> frameBytes AND pcmLen > 0 THEN
                ' Without this, RingWrite writes pcmLen bytes but consumer
                ' reads frameBytes -> misaligned ring -> garbage audio.
                IF pcmLen < frameBytes THEN
                    ' Pad with silence (zeros) at the end
                    FillMemory decodePtr + pcmLen, frameBytes - pcmLen, 0
                    pcmLen = frameBytes
                ELSEIF pcmLen > frameBytes THEN
                    ' Truncate to frameBytes (discard extra samples)
                    pcmLen = frameBytes
                END IF
            END IF
            ' 3.6-fix: save lastPcmLen AFTER alignment so PLC uses correct frame size
            IF pcmLen > 0 THEN
                lastPcmPtr = decodePtr
                lastPcmLen = pcmLen
            END IF

            ' Producer writes decoded PCM into the ring. RingWrite drops
            ' OLDEST bytes on overflow (keeps newest audio, advances
            ' ringR) - no packet drop, no re-prebuffer chaos.
            IF RingWrite(ringPtr, ringCap, ringW, ringR, ringUsed, _
                         decodePtr, pcmLen) THEN
                INCR overflowCount
                IF overflowCount <= 3 OR (overflowCount MOD 100) = 0 THEN
                    AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                           ": ring overflow (drop oldest) overflows=" & _
                           TRIM$(STR$(overflowCount)) & " used=" & TRIM$(STR$(ringUsed))
                END IF
            END IF
        END IF

        ' ---- Gate: start / soft-refill ----
        ' ringPlaying=0 means the consumer is paused. We unpause it when
        ' the ring has enough data. On FIRST start we wait for the full
        ' ringTargetBytes (cold prebuffer). After the first underrun we
        ' only wait for ringMinBytes (faster resume, accepts that WiFi
        ' jitter is still bursty).
        IF ringPlaying = 0 THEN
            IF ringEverStarted = 0 THEN
                feedNeed = ringTargetBytes
            ELSE
                feedNeed = ringMinBytes
            END IF
            IF ringUsed >= feedNeed THEN
                ringPlaying = 1
                ' After underrun re-prebuffers, this was spamming 20+ logs/sec.
                IF ringEverStarted = 0 THEN
                    ringEverStarted = 1
                    AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                           ": ring play ON used=" & TRIM$(STR$(ringUsed)) & _
                           " need=" & TRIM$(STR$(feedNeed))
                END IF
            END IF
        END IF

        ' ---- CONSUMER: pull frames from ring -> WaveOut ----
        ' FeedWaveOut is a GOSUB target defined after the main DO...LOOP.
        ' It scans for a free WaveOut buffer, RingReads frameBytes from
        ' the ring into it, and waveOutWrites. On ring underrun (ringUsed
        ' < frameBytes while ringPlaying=1), it injects ONE silence frame
        ' (RingFillZero) to avoid a hard WaveOut click, then sets
        ' ringPlaying=0 so the gate above re-prebuffers to ringMinBytes.
        GOSUB FeedWaveOut

        ' ---- Adaptive (grow only) ----
        ' Every JITTER_ADAPT_MS, check underrunCount. If >= threshold,
        ' grow ringTargetBytes by 20ms (clamped to ringMaxBytes). No shrink
        ' path - simpler, avoids oscillation, overflow drops oldest anyway.
        ' Also bump ringMinBytes to ringTargetBytes\2 so the soft-refill
        ' gate tracks the new target.
        IF (GetTickCount() - lastAdaptTick) >= %JITTER_ADAPT_MS THEN
            IF underrunCount >= %JITTER_UNDERRUN_THRESHOLD THEN
                IF ringTargetBytes < ringMaxBytes THEN
                    ringTargetBytes = ringTargetBytes + bytesPerMs * 20
                    IF ringTargetBytes > ringMaxBytes THEN ringTargetBytes = ringMaxBytes
                    IF ringMinBytes < (ringTargetBytes \ 2) THEN
                        ringMinBytes = ringTargetBytes \ 2
                    END IF
                    AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                           ": ring grow target=" & TRIM$(STR$(ringTargetBytes)) & _
                           " (underruns=" & TRIM$(STR$(underrunCount)) & ")"
                END IF
            END IF
            underrunCount = 0
            overflowCount = 0
            lastAdaptTick = GetTickCount()
        END IF

reopen_check:
        ' ---- Check for WaveOut reopen request (device change / sleep-wake) ----
        ' Set by WM_DEVICECHANGE / WM_POWERBROADCAST handlers. Old waveOut handle
        ' becomes invalid after sleep/wake -> reopen WaveOut + TCP (TCP also dies
        ' during sleep - Windows doesn't send FIN, server's RECV blocks forever).
        ' Suppress duplicate reopen within 5s (multiple sources set the flag).

        ' CS guards bReopenWaveOut + dwLastReopenTick; actual reopen runs outside CS.
        LOCAL bDoReopen AS LONG
        bDoReopen = 0
        LOCAL nowReopenTick AS DWORD
        nowReopenTick = GetTickCount()
        ' 1.2-fix: Restructured to avoid CS imbalance. Single Enter/Leave pair.
        ' Old code had: Leave(1558) + Enter/Leave(1564/1566) + Leave(1576) = 2E/3L.
        ' Now: one Enter, all decisions inside, one Leave. No ITERATE DO under CS.
        LOCAL bSuppressReopen AS LONG
        bSuppressReopen = 0
        EnterCriticalSection g_csDev
        IF g_Devs(idx).bReopenWaveOut THEN
            g_Devs(idx).bReopenWaveOut = 0
            IF g_Devs(idx).dwLastReopenTick > 0 AND _
               (nowReopenTick - g_Devs(idx).dwLastReopenTick) < 5000 THEN
                ' Suppress duplicate reopen within 5 seconds.
                IF waveOut = 0 THEN
                    ' Dead handle: force reopen despite suppression
                    g_Devs(idx).dwLastReopenTick = nowReopenTick
                    bDoReopen = 1
                ELSE
                    ' Alive: light reset only
                    bSuppressReopen = 1
                END IF
            ELSE
                g_Devs(idx).dwLastReopenTick = nowReopenTick
                bDoReopen = 1
            END IF
        END IF
        LeaveCriticalSection g_csDev

        IF bSuppressReopen THEN
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": reopen suppressed (duplicate within 5s)"
            ringW = 0 : ringR = 0 : ringUsed = 0
            ringPlaying = 0
            ITERATE DO
        END IF
        IF bDoReopen = 0 THEN GOTO skip_reopen_block
        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
               ": reopening WaveOut + TCP (device change / resume)"
        ' Close dead TCP socket and reconnect (TCP dies during sleep).
        IF devTransport = 1 AND fNum > 0 THEN
            TCP CLOSE #fNum
            ERRCLEAR
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": TCP closed for reconnect (sleep/wake recovery)"
            LOCAL reconOk2 AS LONG
            reconOk2 = 0
            DO
                EnterCriticalSection g_csDev
                stillRun = g_Devs(idx).dwRunning
                LeaveCriticalSection g_csDev
                IF stillRun = 0 THEN EXIT DO
                EnterCriticalSection g_csFile
                fNum = FREEFILE
                ERRCLEAR
                TCP OPEN PORT bindPort AT tcpHost AS #fNum TIMEOUT 2000
                LeaveCriticalSection g_csFile
                IF ERR = 0 THEN reconOk2 = 1 : EXIT DO
                SLEEP 1000
            LOOP
            IF reconOk2 = 0 THEN EXIT DO  ' stream stopped
            ' A1: update fAudioFile with new fNum from reconnect
            EnterCriticalSection g_csDev
            g_Devs(idx).fAudioFile = fNum
            LeaveCriticalSection g_csDev
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": TCP reconnected for sleep/wake recovery"
            ResetPipelineState
            ' Reopen WaveOut inline (NOT goto reopen_waveout - that path does
            ' full startup skip + state reset, discarding 1s of packets).
            IF waveOut <> 0 THEN
                LOCAL devReopenI AS LONG
                waveOutReset waveOut
                FOR devReopenI = 0 TO %NUM_WAVE_BUFS - 1
                    waveOutUnprepareHeader waveOut, whdrs(devReopenI), SIZEOF(WAVEHDR)
                NEXT devReopenI
                waveOutClose waveOut
                waveOut = 0
                EnterCriticalSection g_csDev
                g_Devs(idx).hWaveOut = 0
                LeaveCriticalSection g_csDev
            END IF
            ' Re-open WaveOut (re-snapshot devWaveDevice under CS in case user changed it).
            EnterCriticalSection g_csDev
                devWaveDevice = g_Devs(idx).dwWaveDevice
                LeaveCriticalSection g_csDev
                waveResult = waveOutOpen(waveOut, devWaveDevice, wfFmt, 0, 0, %CALLBACK_NULL)
                IF waveResult <> 0 AND outBits = 24 THEN
                    outBits = 16
                    wfFmt.wBitsPerSample  = 16
                    wfFmt.nBlockAlign     = nCh * 2
                    wfFmt.nAvgBytesPerSec = wfFmt.nSamplesPerSec * wfFmt.nBlockAlign
                    waveResult = waveOutOpen(waveOut, devWaveDevice, wfFmt, 0, 0, %CALLBACK_NULL)
                END IF
                ' 3.4-fix: Recalculate ring depth after possible 24->16 fallback
                bytesPerMs = (CLNG(actualSmpRate) * nCh * (outBits \ 8) + 999) \ 1000
                IF bytesPerMs < 1 THEN bytesPerMs = 1
                ringTargetBytes = bytesPerMs * %JITTER_INITIAL_MS
                ringMinBytes    = bytesPerMs * %JITTER_MIN_MS
                ringMaxBytes    = bytesPerMs * %JITTER_MAX_MS
                IF ringMaxBytes > (ringCap \ 2) THEN ringMaxBytes = ringCap \ 2
                IF ringTargetBytes > ringMaxBytes THEN ringTargetBytes = ringMaxBytes
                IF ringMinBytes > ringTargetBytes THEN ringMinBytes = ringTargetBytes
                IF waveResult = 0 THEN
                    EnterCriticalSection g_csDev
                    g_Devs(idx).hWaveOut = waveOut
                    LeaveCriticalSection g_csDev
                    FOR devReopenI = 0 TO %NUM_WAVE_BUFS - 1
                        whdrs(devReopenI).dwFlags = 0
                        whdrs(devReopenI).dwBufferLength = bufSz
                        prepRes = waveOutPrepareHeader(waveOut, whdrs(devReopenI), SIZEOF(WAVEHDR))
                        IF prepRes <> 0 THEN
                            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                                   ": waveOutPrepareHeader FAILED (reopen) buf=" & _
                                   TRIM$(STR$(devReopenI)) & " err=" & TRIM$(STR$(prepRes))
                            whdrs(devReopenI).dwFlags = whdrs(devReopenI).dwFlags OR %WHDR_INQUEUE
                        END IF
                    NEXT devReopenI
                    AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                           ": WaveOut reopened " & TRIM$(STR$(outBits)) & "-bit " & _
                           CodecName(devCodec) & " " & TRIM$(STR$(devSmpRate)) & "Hz " & _
                           TRIM$(STR$(nCh)) & "ch"
                ELSE
                    AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                           ": WaveOut reopen FAILED err=" & TRIM$(STR$(waveResult))
                END IF
                ' Continue main loop - new TCP data will arrive via TCP RECV
                ITERATE DO
            END IF
            ' For non-TCP (UDP) transport: use the reopen_waveout path
            GOTO reopen_waveout

skip_reopen_block:

        ' No direct waveOutWrite from decode path - FeedWaveOut GOSUB pulls from ring.

        ITERATE DO

reopen_waveout:
        ' 1.6-fix: Clear bReopenWaveOut so reopen_check doesn't fire again
        ' (format-change reopen and device-change reopen are independent triggers).
        EnterCriticalSection g_csDev
        g_Devs(idx).bReopenWaveOut = 0
        g_Devs(idx).dwLastReopenTick = GetTickCount()
        LeaveCriticalSection g_csDev
        ' ---- WaveOut reopen after format change ----
        ' Reached via GOTO from format-change detection. Old WaveOut is closed,
        ' devCodec/nCh/devSmpRate/devBits are updated from the packet.
        '   1. Reset ADPCM decoder state
        '   2. Set up WAVEFORMATEX with new format
        '   3. Open WaveOut (with 24->16 fallback)
        '   4. Prepare headers
        '   5. Continue the main loop

        ' Reset ADPCM decoder state - new stream may have different predictor
        predictor  = 0
        stepIndex  = 0
        predictor2 = 0
        stepIndex2 = 0
        lastSeq = 0

        ' Reset PLC + skip + fade state for new format. Ring state is reset LATER
        ' (after actualSmpRate/outBits recomputed so bytesPerMs reflects new format).
        lastPcmPtr = 0
        lastPcmLen = 0
        ' Re-arm startup skip (format change = new stream = mic may re-transient)
        skipFramesRemaining = 999

        ' 1.3: Reset packet/PLC/stats state that the manual block above missed.
        pktRecv = 0 : lostPkt = 0 : oooPkt = 0
        plcPending = 0 : plcLPtr = 0 : plcLLen = 0
        EnterCriticalSection g_csDev
        g_Devs(idx).dwPktRecv = 0 : g_Devs(idx).dwPktOOO = 0
        g_Devs(idx).dwPktLost = 0 : g_Devs(idx).wLastSeq = 0
        g_Devs(idx).dwStreamStart = GetTickCount()
        g_Devs(idx).dwLastPktTick = g_Devs(idx).dwStreamStart
        LeaveCriticalSection g_csDev

        ' ---- Close the old (stale) WaveOut if still open (safe no-op if waveOut = 0) ----
        IF waveOut <> 0 THEN
            LOCAL reopenI AS LONG
            waveOutReset waveOut
            FOR reopenI = 0 TO %NUM_WAVE_BUFS - 1
                waveOutUnprepareHeader waveOut, whdrs(reopenI), SIZEOF(WAVEHDR)
            NEXT reopenI
            waveOutClose waveOut
            waveOut = 0
            EnterCriticalSection g_csDev
            g_Devs(idx).hWaveOut = 0
            LeaveCriticalSection g_csDev
        END IF
        fadeSamplesRemaining = 0

        ' Set up WAVEFORMATEX - bits per sample depends on CODEC:
        '   ADPCM (codec=5): ALWAYS 16-bit (ESP dithers 24->16 before encoding)
        '   PCM (codec=6): uses devBits (16 or 24) from packet
        IF devCodec = %CODEC_ID_PCM THEN
            outBits = devBits
        ELSE
            outBits = 16
        END IF

        ' BWSOLA PLC forces 16-bit WaveOut on reopen (same as initial setup).
        IF %BWSOLA_PLC_ENABLE AND outBits = 24 THEN
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": BWSOLA forces 16-bit WaveOut on reopen (was 24-bit PCM)"
            outBits = 16
        END IF

        ' <<<DRIFT FIX>>> та же коррекция частоты при reopen
        actualSmpRate = EspActualRate(devSmpRate, devBits)
        IF actualSmpRate <> devSmpRate THEN
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": rate trim " & TRIM$(STR$(devSmpRate)) & "Hz -> " & _
                   TRIM$(STR$(actualSmpRate)) & "Hz (reopen, drift fix)"
        END IF
        wfFmt.wFormatTag      = %WAVE_FORMAT_PCM
        wfFmt.nChannels       = nCh
        wfFmt.nSamplesPerSec  = actualSmpRate
        wfFmt.wBitsPerSample  = outBits
        wfFmt.nBlockAlign     = nCh * (outBits \ 8)
        wfFmt.nAvgBytesPerSec = wfFmt.nSamplesPerSec * wfFmt.nBlockAlign
        wfFmt.cbSize          = 0

        ' Open WaveOut. Try 24-bit first if PCM 24-bit; if the card rejects
        ' it, retry with 16-bit and mark for on-the-fly 24->16 conversion.
        ' reopen path - user may have changed output device mid-stream).
        EnterCriticalSection g_csDev
        devWaveDevice = g_Devs(idx).dwWaveDevice
        LeaveCriticalSection g_csDev
        waveResult = waveOutOpen(waveOut, devWaveDevice, wfFmt, 0, 0, %CALLBACK_NULL)
        IF waveResult <> 0 AND outBits = 24 THEN
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": 24-bit WaveOut failed on reopen (err=" & TRIM$(STR$(waveResult)) & _
                   "), falling back to 16-bit"
            outBits = 16
            wfFmt.wBitsPerSample  = 16
            wfFmt.nBlockAlign     = nCh * 2
            wfFmt.nAvgBytesPerSec = wfFmt.nSamplesPerSec * wfFmt.nBlockAlign
            waveResult = waveOutOpen(waveOut, devWaveDevice, wfFmt, 0, 0, %CALLBACK_NULL)
        END IF

        IF waveResult = 0 THEN
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": WaveOut reopened " & TRIM$(STR$(outBits)) & "-bit " & _
                   CodecName(devCodec) & " " & TRIM$(STR$(devSmpRate)) & "Hz " & _
                   TRIM$(STR$(nCh)) & "ch"

            ' Store new waveOut handle
            EnterCriticalSection g_csDev
            g_Devs(idx).hWaveOut = waveOut
            LeaveCriticalSection g_csDev

            ' Prepare headers for the new WaveOut
            FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
                whdrs(wIdx2).dwFlags = 0
                whdrs(wIdx2).dwBufferLength = bufSz
                prepRes = waveOutPrepareHeader(waveOut, whdrs(wIdx2), SIZEOF(WAVEHDR))
                IF prepRes <> 0 THEN
                    AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                           ": waveOutPrepareHeader FAILED (fmt-change) buf=" & _
                           TRIM$(STR$(wIdx2)) & " err=" & TRIM$(STR$(prepRes))
                    whdrs(wIdx2).dwFlags = whdrs(wIdx2).dwFlags OR %WHDR_INQUEUE
                END IF
            NEXT wIdx2
        ELSE
            AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                   ": waveOutOpen FAILED on reopen err=" & TRIM$(STR$(waveResult)) & _
                   " - audio disabled until next format change"
        END IF

        ' ---- Ring reset after reopen ----
        ' Recompute bytesPerMs / targets from new format and clear ring state.
        ringW = 0 : ringR = 0 : ringUsed = 0
        ringPlaying = 0
        frameBytes = 0
        underrunCount = 0
        overflowCount = 0
        lastAdaptTick = GetTickCount()
        ' Full prebuffer on format change (treat as fresh stream)
        ringEverStarted = 0

        bytesPerMs = (CLNG(actualSmpRate) * nCh * (outBits \ 8) + 999) \ 1000
        IF bytesPerMs < 1 THEN bytesPerMs = 1
        ringTargetBytes = bytesPerMs * %JITTER_INITIAL_MS
        ringMinBytes    = bytesPerMs * %JITTER_MIN_MS
        ringMaxBytes    = bytesPerMs * %JITTER_MAX_MS
        IF ringMaxBytes > (ringCap \ 2) THEN ringMaxBytes = ringCap \ 2
        IF ringTargetBytes > ringMaxBytes THEN ringTargetBytes = ringMaxBytes
        IF ringMinBytes > ringTargetBytes THEN ringMinBytes = ringTargetBytes

        AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
               ": ring reset after reopen target=" & TRIM$(STR$(ringTargetBytes)) & _
               "B min=" & TRIM$(STR$(ringMinBytes)) & " max=" & TRIM$(STR$(ringMaxBytes))

        ITERATE DO
    LOOP

    GOTO aud_cleanup

FeedWaveOut:
    ' ---- CONSUMER: pull frameBytes from ring -> free WaveOut buffer ----
    ' GOSUB target. Called from the main loop after producer RingWrite +
    ' gate, AND from the UDP RECV timeout path (to drain ring while idle).
    ' Single-reader: only AudioThread calls this. No locks needed.
    IF waveOut = 0 OR ringPlaying = 0 OR frameBytes <= 0 OR ringPtr = 0 THEN
        RETURN
    END IF

    LOCAL feedFi AS LONG, freeIdx AS LONG, got AS LONG, wRes AS LONG

    DO
        ' Find a free (not INQUEUE) WaveOut buffer
        freeIdx = -1
        FOR feedFi = 0 TO %NUM_WAVE_BUFS - 1
            IF (whdrs(feedFi).dwFlags AND %WHDR_INQUEUE) = 0 THEN
                freeIdx = feedFi
                EXIT FOR
            END IF
        NEXT feedFi
        IF freeIdx < 0 THEN EXIT DO   ' WaveOut queue full - OK, not overflow

        IF ringUsed >= frameBytes THEN
            ' Pull one frame from the ring into the free WaveOut buffer
            got = RingRead(ringPtr, ringCap, ringW, ringR, ringUsed, _
                           pcmPtrs(freeIdx), frameBytes)
            IF got <= 0 THEN EXIT DO

            whdrs(freeIdx).dwBufferLength  = frameBytes
            whdrs(freeIdx).dwBytesRecorded = frameBytes
            wRes = waveOutWrite(waveOut, whdrs(freeIdx), SIZEOF(WAVEHDR))
            ' Check for device-loss errors (sleep/wake -> invalid handle).
            ' MMSYSERR_NOERROR=0, WAVERR_STILLPLAYING=33 (benign, no reopen).
            IF wRes <> 0 AND wRes <> 33 THEN
                EnterCriticalSection g_csDev
                g_Devs(idx).bReopenWaveOut = 1
                LeaveCriticalSection g_csDev
                EXIT DO
            END IF
        ELSE
            ' Soft underrun: ring has less than one frame while consumer
            ' is active. Inject ONE silence frame to avoid a hard WaveOut
            ' click, then pause the consumer (ringPlaying=0). The gate in
            ' the main loop will re-enable it once ringUsed >= ringMinBytes
            ' (faster refill after first underrun).
            IF ringEverStarted THEN
                INCR underrunCount
                IF underrunCount <= 3 OR (underrunCount MOD 50) = 0 THEN
                    AddLog "[AUD] dev #" & TRIM$(STR$(idx)) & _
                           ": ring underrun used=" & TRIM$(STR$(ringUsed)) & _
                           " need=" & TRIM$(STR$(frameBytes)) & _
                           " #" & TRIM$(STR$(underrunCount))
                END IF
                LOCAL inQ AS LONG, qFi AS LONG
                inQ = 0
                FOR qFi = 0 TO %NUM_WAVE_BUFS - 1
                    IF (whdrs(qFi).dwFlags AND %WHDR_INQUEUE) <> 0 THEN INCR inQ
                NEXT qFi
                IF inQ = 0 AND freeIdx >= 0 THEN
                    ' WaveOut truly empty — inject one silence frame
                    RingFillZero pcmPtrs(freeIdx), frameBytes
                    whdrs(freeIdx).dwBufferLength  = frameBytes
                    whdrs(freeIdx).dwBytesRecorded = frameBytes
                    ' 2.5: check result - dead WaveOut must trigger reopen
                    wRes = waveOutWrite(waveOut, whdrs(freeIdx), SIZEOF(WAVEHDR))
                    IF wRes <> 0 AND wRes <> 33 THEN
                        EnterCriticalSection g_csDev
                        g_Devs(idx).bReopenWaveOut = 1
                        LeaveCriticalSection g_csDev
                    END IF
                END IF
                ringPlaying = 0   ' pause consumer; gate will refill
            END IF
            EXIT DO
        END IF
    LOOP
    RETURN

aud_cleanup:
    ' ---- Cleanup ----
    IF waveOut THEN
        waveOutReset waveOut
        FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
            waveOutUnprepareHeader waveOut, whdrs(wIdx2), SIZEOF(WAVEHDR)
        NEXT wIdx2
        waveOutClose waveOut
    END IF

    ' Dump is NOT closed here - closed by ToggleDump / WM_CLOSE after WaitForAllThreads

    ' Transport may have been closed from outside (StopAllStreams) on shutdown,
    ' so use ERRCLEAR to handle already-closed file gracefully.
    ' TCP and UDP use different CLOSE statements.
    ' 1.3-fix: serialize CLOSE with g_csFile
    EnterCriticalSection g_csFile
    ERRCLEAR
    IF devTransport = 1 THEN
        TCP CLOSE #fNum
    ELSE
        UDP CLOSE #fNum
    END IF
    ERRCLEAR
    LeaveCriticalSection g_csFile

    FOR wIdx2 = 0 TO %NUM_WAVE_BUFS - 1
        IF pcmPtrs(wIdx2) THEN HeapFree g_hHeap, 0, pcmPtrs(wIdx2)
    NEXT wIdx2

    IF interleaveBuf THEN HeapFree g_hHeap, 0, interleaveBuf : interleaveBuf = 0
    IF decodePtr THEN HeapFree g_hHeap, 0, decodePtr : decodePtr = 0
    IF ringPtr   THEN HeapFree g_hHeap, 0, ringPtr   : ringPtr   = 0
    ' BWSOLA PLC: free L_WSOLA copy + reconstruction output buffers
    IF plcLBuf   THEN HeapFree g_hHeap, 0, plcLBuf   : plcLBuf   = 0
    IF plcOutBuf THEN HeapFree g_hHeap, 0, plcOutBuf : plcOutBuf = 0

    ' Clear state under CS so other threads see consistent state.
    ' NOTE: hAudioThread is NOT cleared here - the main thread is responsible
    ' for reading and closing the thread handle (StopCheckedStreams/WaitForAllThreads).
    ' Clearing it here would cause a handle leak if the main thread hasn't
    ' read it yet.
    EnterCriticalSection g_csDev
    g_Devs(idx).fAudioFile = 0
    g_Devs(idx).dwRunning  = 0
    g_Devs(idx).hWaveOut   = 0
    LeaveCriticalSection g_csDev

    FUNCTION = 0
END FUNCTION

#ENDIF
