' ============================================================================
'  dump.inc — WAV dump (WriteWavHeader, OpenNewDumpFile, ToggleDump)
' ============================================================================
#IF NOT %DEF(%DUMP_INC)
%DUMP_INC = 1

' WriteWavHeader - Write 44-byte WAV header to the current dump file.
' Called on first packet and when auto-splitting to a new file.
SUB WriteWavHeader()
    IF g_hDumpFile = 0 THEN EXIT SUB

    LOCAL sampleRate AS DWORD
    LOCAL channels AS LONG
    LOCAL bitsPerSample AS LONG
    LOCAL byteRate AS DWORD
    LOCAL blockAlign AS LONG
    LOCAL hdr AS STRING

    sampleRate = g_dumpSampleRate
    channels = g_dumpChannels
    bitsPerSample = g_dumpBits
    byteRate = sampleRate * channels * (bitsPerSample \ 8)
    blockAlign = channels * (bitsPerSample \ 8)

    ' Build 44-byte WAV header
    hdr = "RIFF" & _
          MKL$(36) & _              ' file size - 8 (placeholder, updated on close)
          "WAVE" & _
          "fmt " & _
          MKL$(16) & _              ' fmt chunk size
          MKI$(1) & _               ' format tag = PCM
          MKI$(channels) & _        ' channels
          MKL$(sampleRate) & _      ' sample rate
          MKL$(byteRate) & _        ' byte rate
          MKI$(blockAlign) & _      ' block align
          MKI$(bitsPerSample) & _   ' bits per sample
          "data" & _
          MKL$(0)                   ' data size (placeholder, updated on close)

    PUT$ #g_hDumpFile, hdr
    g_dumpDataSize = 0
    g_dumpWavReady = 1
END SUB

' UpdateWavHeader - Update RIFF size and data size in the WAV header.
' Called when closing a file (on stop or auto-split).
' NOTE: PowerBASIC SEEK is 1-based (first byte = position 1), so we add 1
' to the 0-based WAV offsets.
SUB UpdateWavHeader()
    IF g_hDumpFile = 0 THEN EXIT SUB

    ' Update data chunk size (0-based offset 40 -> 1-based position 41)
    SEEK #g_hDumpFile, 41
    PUT$ #g_hDumpFile, MKL$(g_dumpDataSize)

    ' Update RIFF size (0-based offset 4 -> 1-based position 5)
    ' RIFF size = 36 + data size
    SEEK #g_hDumpFile, 5
    PUT$ #g_hDumpFile, MKL$(36 + g_dumpDataSize)

    ' Seek to end for further writes
    SEEK #g_hDumpFile, LOF(g_hDumpFile) + 1
END SUB

' OpenNewDumpFile - Open a new WAV dump file with the next index.
' Called on initial start and on 1 GB auto-split.
' IMPORTANT: caller MUST hold g_csDump. This function modifies
' g_hDumpFile / g_bDumping WITHOUT entering g_csDump (would be recursive).
SUB OpenNewDumpFile()
    LOCAL sFile AS STRING

    ' g_dumpBaseName (with timestamp) is set by ToggleDump; we only append index.
    sFile = g_dumpBaseName & "_" & TRIM$(STR$(g_dumpFileIdx)) & ".wav"

    EnterCriticalSection g_csFile
    g_hDumpFile = FREEFILE
    OPEN sFile FOR BINARY ACCESS WRITE LOCK SHARED AS #g_hDumpFile
    IF ERR THEN
        ERRCLEAR
        LeaveCriticalSection g_csFile
        AddLog "ERROR: cannot open " & sFile
        ' 10-fix: Caller (AudioThread) already holds g_csDump. Do NOT re-enter
        ' g_csDump here (recursive CS entry is safe but sloppy). Set fields directly.
        g_hDumpFile = 0
        g_bDumping = 0
        ' 1.4: Do NOT call CONTROL SET TEXT from AudioThread (deadlock with
        ' ToggleDump which takes g_csDump from GUI thread). Set a dirty flag;
        ' RefreshUI (GUI thread, no g_csDump) will reset the button caption.
        g_dumpBtnDirty = 1
        EXIT SUB
    END IF
    LeaveCriticalSection g_csFile

    WriteWavHeader
    AddLog "WAV dump: " & sFile & " (" & TRIM$(STR$(g_dumpSampleRate)) & _
           "Hz " & TRIM$(STR$(g_dumpChannels)) & "ch " & _
           TRIM$(STR$(g_dumpBits)) & "-bit)"
END SUB

' ToggleDump - Start/stop WAV audio dump to file.
'   When starting: opens dump_<timestamp>_1.wav for binary write, sets g_bDumping=1.
'   The audio thread writes decoded PCM to the WAV file.
'   Files auto-split at 1 GB. When stopping: WAV header is finalized.
'   The button caption toggles between "DUMP" and "STOP DUMP".
SUB ToggleDump()
    LOCAL sFile AS STRING
    LOCAL sBtn  AS STRING

    IF g_bDumping THEN
        ' ---- Stop dumping ----
        ' consistency with AudioThread reads. LONG writes are atomic on x86,
        ' but a single CS covers the flag + g_hDumpFile close as one atomic
        ' transition - no window where AudioThread sees g_bDumping=0 but
        ' g_hDumpFile still open and writes to a closing handle.
        EnterCriticalSection g_csDump
        g_bDumping = 0
        IF g_hDumpFile THEN
            UpdateWavHeader
            CLOSE g_hDumpFile
            g_hDumpFile = 0
        END IF
        LeaveCriticalSection g_csDump
        CONTROL SET TEXT g_hDlg, %IDC_BTN_DUMP, "DUMP"
        AddLog "WAV dump stopped (" & TRIM$(STR$(g_dumpFileIdx)) & " file(s))"
    ELSE
        ' ---- Start dumping ----
        LOCAL stDump AS SYSTEMTIME
        GetLocalTime stDump
        g_dumpBaseName = EXE.PATH$ & "dump_" & _
                RIGHT$("0" & TRIM$(STR$(stDump.wHour)), 2) & _
                RIGHT$("0" & TRIM$(STR$(stDump.wMinute)), 2) & _
                RIGHT$("0" & TRIM$(STR$(stDump.wSecond)), 2)
        g_dumpFileIdx = 0
        g_dumpWavReady = 0
        g_dumpCodec = 0
        g_dumpDataSize = 0
        ' Now only the selected device's PCM is dumped; other devices are
        ' ignored. If no device is selected, g_dumpIp=0 and ALL devices
        ' dump (legacy behavior, useful for single-device setups).
        LOCAL dumpSelIdx AS LONG
        dumpSelIdx = GetSelectedDevIdx()
        IF dumpSelIdx >= 0 THEN
            EnterCriticalSection g_csDev
            g_dumpIp = g_Devs(dumpSelIdx).dwIP
            LeaveCriticalSection g_csDev
            AddLog "WAV dump: filtering to selected dev #" & TRIM$(STR$(dumpSelIdx)) & _
                   " (IP=" & FormatIP(g_dumpIp) & ")"
        ELSE
            g_dumpIp = 0
            AddLog "WAV dump: no device selected - dumping ALL active streams (may mix PCM)"
        END IF
        ' Will open file on first packet (need format info from packet header).
        ' first-packet check (also under CS) sees a consistent flag+state.
        EnterCriticalSection g_csDump
        g_bDumping = 1
        LeaveCriticalSection g_csDump
        CONTROL SET TEXT g_hDlg, %IDC_BTN_DUMP, "STOP DUMP"
        AddLog "WAV dump: waiting for first packet to determine format..."
    END IF
END SUB

#ENDIF
