' ============================================================================
'  util.inc — Utility functions (FormatIP, AddLog, InitStepTable)
' ============================================================================
#IF NOT %DEF(%UTIL_INC)
%UTIL_INC = 1

' FormatIP - Convert network-order DWORD IP to "a.b.c.d"
FUNCTION FormatIP(BYVAL dwIP AS DWORD) AS STRING
    LOCAL p AS BYTE PTR
    p = VARPTR(dwIP)
    FUNCTION = USING$("#_.#_.#_.#", @p, @p[1], @p[2], @p[3])
END FUNCTION

' AddLog - Append timestamped message to log textbox
SUB AddLog(BYVAL sText AS STRING)
    ' Shutdown guard: once g_bShuttingDown=1, do not PostMessage into the
    ' dying dialog (CONTROL SEND into a half-destroyed edit hangs the closer).
    ' Worker threads become silent no-ops; remaining heap is freed on exit.
    IF g_bShuttingDown THEN EXIT SUB

    LOCAL st AS SYSTEMTIME
    LOCAL sLine AS STRING

    GetLocalTime st

    sLine = "[" & RIGHT$("0" & TRIM$(STR$(st.wHour)), 2) & ":" & _
                  RIGHT$("0" & TRIM$(STR$(st.wMinute)), 2) & ":" & _
                  RIGHT$("0" & TRIM$(STR$(st.wSecond)), 2) & "] " & _
            sText & $CRLF

    ' Use PostMessage (async) instead of SendMessage - worker threads would
    ' deadlock when GUI is in WaitForSingleObject during shutdown.
    '
    ' PostMessage only stores a raw DWORD pointer - PowerBASIC does NOT
    ' reference-count the string. Allocate a heap copy (NUL-terminated),
    ' pass its pointer in lParam and byte length in wParam; GUI thread
    ' reads exactly that many bytes, then frees the heap block.
    LOCAL nLen  AS LONG
    LOCAL pBuf  AS BYTE PTR
    nLen = LEN(sLine)
    pBuf = HeapAlloc(g_hHeap, 0, nLen + 1)
    IF pBuf THEN
        POKE$ pBuf, sLine
        @pBuf[nLen] = 0          ' NUL terminator
        IF PostMessage(g_hDlg, %WM_APP_LOG, nLen, pBuf) = 0 THEN
            HeapFree g_hHeap, 0, pBuf
        END IF
    END IF
END SUB

' InitStepTable - Initialize IMA ADPCM lookup tables
SUB InitStepTable()
    ARRAY ASSIGN g_StepTable() = _
        7, 8, 9, 10, 11, 12, 13, 14, _
        16, 17, 19, 21, 23, 25, 28, 31, _
        34, 37, 41, 45, 50, 55, 60, 66, _
        73, 80, 88, 97, 107, 118, 130, 143, _
        157, 173, 190, 209, 230, 253, 279, 307, _
        337, 371, 408, 449, 494, 544, 598, 658, _
        724, 796, 876, 963, 1060, 1166, 1282, 1411, _
        1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, _
        3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, _
        7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, _
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, _
        32767

    ARRAY ASSIGN g_IndexTable() = -1, -1, -1, -1, 2, 4, 6, 8
END SUB

#ENDIF
