' ============================================================================
'  ui_refresh.inc — Incremental ListView + StatusBar refresh
' ============================================================================
#IF NOT %DEF(%EASSP_UI_REFRESH_INC)
%EASSP_UI_REFRESH_INC = 1

' ============================================================================
'  Incremental UI refresh (point-update)
'    - UiBuildStrings: build display strings from a DeviceInfo snapshot
'    - UiFindRow: 1-based ListView row for a device index (via USER data)
'    - UiSetTextIf: LISTVIEW SET TEXT only when string differs from cache
'    - UiSyncStructure: insert/delete rows only when g_uiDirty
'    - UiPaintDevice: per-cell point update (uses g_UiCache)
'    - UiUpdateStatusBar: parts only when g_sbDirty; STATIC cache for text
'    - RefreshUI: orchestrates the above
' ============================================================================

' UiBuildStrings - Build all display strings for one device row.
' d is passed BYREF (DeviceInfo). Strings are returned via BYREF parameters.
SUB UiBuildStrings(d AS DeviceInfo, _
        sTitle AS STRING, sIpPort AS STRING, sStatus AS STRING, _
        sRate AS STRING, sBits AS STRING, sCh AS STRING, sCodec AS STRING, _
        sRssi AS STRING, sHeap AS STRING, sFw AS STRING, _
        sPkts AS STRING, sLost AS STRING, sDur AS STRING)
    LOCAL tick AS DWORD, totalSecs AS LONG, mins AS LONG, secs AS LONG

    IF LEN(TRIM$(d.sHostname)) > 0 THEN
        sTitle = TRIM$(d.sHostname) & " (" & TRIM$(d.sMac) & ")"
    ELSE
        sTitle = TRIM$(d.sMac)
    END IF

    sIpPort = FormatIP(d.dwIP) & ":" & TRIM$(STR$(d.dwPort))

    SELECT CASE d.dwStatus
        CASE %STS_STREAM: sStatus = "Streaming"
        CASE %STS_ERROR:  sStatus = "Error"
        CASE ELSE:        sStatus = "Idle"
    END SELECT

    sRate = TRIM$(STR$(d.dwSmpRate)) & " Hz"
    IF d.dwBits > 0 THEN
        sBits = TRIM$(STR$(d.dwBits)) & "-bit"
    ELSE
        sBits = "-"
    END IF

    SELECT CASE d.dwChannels
        CASE 1: sCh = "mono"
        CASE 2: sCh = "stereo"
        CASE ELSE: sCh = TRIM$(STR$(d.dwChannels))
    END SELECT

    SELECT CASE d.dwCodec
        CASE %CODEC_ID:     sCodec = "ADPCM"
        CASE %CODEC_ID_PCM: sCodec = "PCM"
        CASE ELSE:          sCodec = TRIM$(STR$(d.dwCodec))
    END SELECT
    SELECT CASE d.dwTransport
        CASE 1:  sCodec = sCodec & "/TCP"
        CASE 0:  sCodec = sCodec & "/UDP"
        CASE ELSE: sCodec = sCodec & "/NONE"
    END SELECT

    sRssi = STR$(d.dwRSSI) & " dBm"
    IF d.dwFreeHeap >= 1024 THEN
        sHeap = LTRIM$(STR$(d.dwFreeHeap \ 1024)) & "KB"
    ELSE
        sHeap = LTRIM$(STR$(d.dwFreeHeap)) & "B"
    END IF
    sFw   = TRIM$(d.sFirmware)
    sPkts = STR$(d.dwPktRecv)
    sLost = STR$(d.dwPktLost)

    IF d.dwHBActive THEN
        tick = GetTickCount()
        totalSecs = (tick - d.dwStreamStart) \ 1000
        mins = totalSecs \ 60
        secs = totalSecs MOD 60
        sDur = LTRIM$(STR$(mins)) & "m " & LTRIM$(STR$(secs)) & "s"
    ELSE
        sDur = "-"
    END IF
END SUB

' UiFindRow - Find 1-based ListView row for a device array index.
' Returns 0 if the device is not currently in the ListView.
FUNCTION UiFindRow(BYVAL devIdx AS LONG) AS LONG
    LOCAL n AS LONG, r AS LONG, u AS LONG
    LISTVIEW GET COUNT g_hDlg, %IDC_LISTVIEW TO n
    FOR r = 1 TO n
        LISTVIEW GET USER g_hDlg, %IDC_LISTVIEW, r TO u
        IF u = devIdx THEN
            FUNCTION = r
            EXIT FUNCTION
        END IF
    NEXT r
    FUNCTION = 0
END FUNCTION

' UiSetTextIf - LISTVIEW SET TEXT only when newStr differs from cacheField.
' cacheField is BYREF ASCIIZ; PB auto-converts ASCIIZ to STRING for the
' comparison and back to ASCIIZ on assignment. Eliminates redundant GUI
' work each timer tick for cells whose values did not change.
SUB UiSetTextIf(BYVAL lvRow AS LONG, BYVAL col0 AS LONG, _
                BYVAL newStr AS STRING, cacheField AS ASCIIZ)
    IF newStr <> cacheField THEN
        LISTVIEW SET TEXT g_hDlg, %IDC_LISTVIEW, lvRow, col0 + 1, newStr
        cacheField = newStr
    END IF
END SUB

' UiSyncStructure - Insert/delete ListView rows to match snap().
' Called only when g_uiDirty (device added or timed out). Seeds empty cache
' entries so UiPaintDevice writes every cell on the next pass.
SUB UiSyncStructure(snap() AS DeviceInfo)
    LOCAL lvCount AS LONG, lvIdx AS LONG, devIdx AS LONG, i AS LONG
    LOCAL t AS STRING, ipPort AS STRING, st AS STRING
    LOCAL r AS STRING, b AS STRING, c AS STRING, co AS STRING
    LOCAL rs AS STRING, h AS STRING, f AS STRING
    LOCAL p AS STRING, l AS STRING, d AS STRING

    ' Phase 1: remove rows whose device is inactive.
    ' lvCount MUST be fetched before the delete loop (was a bug: the local
    ' was uninitialized in the old code and could skip/garbage-collect rows).
    LISTVIEW GET COUNT g_hDlg, %IDC_LISTVIEW TO lvCount
    FOR lvIdx = lvCount TO 1 STEP -1
        LISTVIEW GET USER g_hDlg, %IDC_LISTVIEW, lvIdx TO devIdx
        IF devIdx < 0 OR devIdx >= %MAX_DEVICES THEN
            LISTVIEW DELETE ITEM g_hDlg, %IDC_LISTVIEW, lvIdx
        ELSEIF snap(devIdx).dwActive = 0 THEN
            AddLog "Device gone: MAC=" & TRIM$(snap(devIdx).sMac)
            LISTVIEW DELETE ITEM g_hDlg, %IDC_LISTVIEW, lvIdx
            g_UiCache(devIdx).fInList = 0
            g_UiCache(devIdx).lvRow   = 0
        END IF
    NEXT lvIdx

    ' Phase 2: insert rows for newly-active devices not yet in the LV.
    FOR i = 0 TO %MAX_DEVICES - 1
        IF snap(i).dwActive = 0 THEN
            g_UiCache(i).fInList = 0
            g_UiCache(i).lvRow   = 0
            ITERATE FOR
        END IF

        lvIdx = UiFindRow(i)
        IF lvIdx = 0 THEN
            UiBuildStrings snap(i), t, ipPort, st, r, b, c, co, rs, h, f, p, l, d
            LISTVIEW GET COUNT g_hDlg, %IDC_LISTVIEW TO lvCount
            lvIdx = lvCount + 1
            LISTVIEW INSERT ITEM g_hDlg, %IDC_LISTVIEW, lvIdx, 0, t
            LISTVIEW SET USER g_hDlg, %IDC_LISTVIEW, lvIdx, i

            ' Seed empty cache so UiPaintDevice writes every cell this pass.
            g_UiCache(i).fInList = 1
            g_UiCache(i).lvRow   = lvIdx
            g_UiCache(i).sTitle  = ""
            g_UiCache(i).sIpPort = ""
            g_UiCache(i).sStatus = ""
            g_UiCache(i).sRate   = ""
            g_UiCache(i).sBits   = ""
            g_UiCache(i).sCh     = ""
            g_UiCache(i).sCodec  = ""
            g_UiCache(i).sRssi   = ""
            g_UiCache(i).sHeap   = ""
            g_UiCache(i).sFw     = ""
            g_UiCache(i).sPkts   = ""
            g_UiCache(i).sLost   = ""
            g_UiCache(i).sDur    = ""
        ELSE
            g_UiCache(i).fInList = 1
            g_UiCache(i).lvRow   = lvIdx
        END IF
    NEXT i
END SUB

' UiPaintDevice - Point-update one device's ListView row.
' Static columns (rate/bits/ch/codec/fw) are written only when forceStatic
' or d.bFmtDirty; dynamic columns (status/rssi/heap/pkts/lost/dur) are
' compared every tick (cheap: UiSetTextIf short-circuits if unchanged).
SUB UiPaintDevice(BYVAL i AS LONG, d AS DeviceInfo, BYVAL forceStatic AS LONG)
    LOCAL lvIdx AS LONG
    LOCAL sTitle AS STRING, sIpPort AS STRING, sStatus AS STRING
    LOCAL sRate AS STRING, sBits AS STRING, sCh AS STRING, sCodec AS STRING
    LOCAL sRssi AS STRING, sHeap AS STRING, sFw AS STRING
    LOCAL sPkts AS STRING, sLost AS STRING, sDur AS STRING

    lvIdx = UiFindRow(i)
    IF lvIdx = 0 THEN EXIT SUB

    g_UiCache(i).lvRow   = lvIdx
    g_UiCache(i).fInList = 1

    UiBuildStrings d, sTitle, sIpPort, sStatus, sRate, sBits, sCh, sCodec, _
                   sRssi, sHeap, sFw, sPkts, sLost, sDur

    ' hostname / IP can change without format dirty - cheap to compare.
    UiSetTextIf lvIdx, %LV_COL_MAC, sTitle,  g_UiCache(i).sTitle
    UiSetTextIf lvIdx, %LV_COL_IP,  sIpPort, g_UiCache(i).sIpPort

    IF forceStatic OR d.bFmtDirty THEN
        UiSetTextIf lvIdx, %LV_COL_RATE,  sRate,  g_UiCache(i).sRate
        UiSetTextIf lvIdx, %LV_COL_BITS,  sBits,  g_UiCache(i).sBits
        UiSetTextIf lvIdx, %LV_COL_CH,    sCh,    g_UiCache(i).sCh
        UiSetTextIf lvIdx, %LV_COL_CODEC, sCodec, g_UiCache(i).sCodec
        UiSetTextIf lvIdx, %LV_COL_FW,    sFw,    g_UiCache(i).sFw
    END IF

    ' Dynamic every timer tick - SET only on change.
    UiSetTextIf lvIdx, %LV_COL_STATUS, sStatus, g_UiCache(i).sStatus
    UiSetTextIf lvIdx, %LV_COL_RSSI,   sRssi,   g_UiCache(i).sRssi
    UiSetTextIf lvIdx, %LV_COL_HEAP,   sHeap,   g_UiCache(i).sHeap
    UiSetTextIf lvIdx, %LV_COL_PKTS,   sPkts,   g_UiCache(i).sPkts
    UiSetTextIf lvIdx, %LV_COL_LOST,   sLost,   g_UiCache(i).sLost
    UiSetTextIf lvIdx, %LV_COL_DUR,    sDur,    g_UiCache(i).sDur
END SUB

' UiUpdateStatusBar - point-update statusbar parts and text.
' Parts (SET PARTS) only when g_sbDirty or first call. Per-part text only
' when the value changed (cached in STATIC locals across timer ticks).
SUB UiUpdateStatusBar(BYVAL cnt AS LONG, BYVAL strmCnt AS LONG)
    LOCAL sbI AS LONG, sbPorts AS STRING
    LOCAL udpText AS STRING, udpW AS LONG
    LOCAL sbDevIdx AS LONG, devText AS STRING
    STATIC lastCnt AS LONG, lastStrm AS LONG, lastOut AS STRING
    STATIC partsReady AS LONG
    ' firstText forces one-time write of cnt/strm text on the very first
    ' call. PB STATICs are zero-initialized, so lastCnt=0 matches cnt=0 on
    ' first call and the change-check would skip the write, leaving the
    ' statusbar blank until a device appears.
    STATIC firstText AS LONG

    ' Parts layout only when discovery ports changed (or first call).
    IF g_sbDirty OR (partsReady = 0) THEN
        sbPorts = ""
        FOR sbI = 0 TO %MAX_DISC_PORTS - 1
            IF g_DiscPorts(sbI).isOpen THEN
                IF LEN(sbPorts) > 0 THEN sbPorts = sbPorts & ","
                sbPorts = sbPorts & TRIM$(STR$(g_DiscPorts(sbI).port))
            END IF
        NEXT sbI
        IF LEN(sbPorts) = 0 THEN sbPorts = "none"
        udpText = " UDP:" & sbPorts
        udpW = LEN(udpText) * 6 + 12
        IF udpW < 60 THEN udpW = 60
        STATUSBAR SET PARTS g_hDlg, %IDC_STATUSBAR, 100, 100, udpW, 9999
        STATUSBAR SET TEXT g_hDlg, %IDC_STATUSBAR, 3, 0, udpText
        g_sbDirty = 0
        partsReady = 1
    END IF

    IF firstText = 0 OR cnt <> lastCnt THEN
        STATUSBAR SET TEXT g_hDlg, %IDC_STATUSBAR, 1, 0, " Devices:" & STR$(cnt)
        lastCnt = cnt
    END IF
    IF firstText = 0 OR strmCnt <> lastStrm THEN
        STATUSBAR SET TEXT g_hDlg, %IDC_STATUSBAR, 2, 0, " Streaming:" & STR$(strmCnt)
        lastStrm = strmCnt
    END IF

    COMBOBOX GET SELECT g_hDlg, %IDC_COMBO_DEVICE TO sbDevIdx
    IF sbDevIdx >= 1 THEN
        COMBOBOX GET TEXT g_hDlg, %IDC_COMBO_DEVICE, sbDevIdx TO devText
        devText = " Output: " & devText
    ELSE
        devText = " Output: none"
    END IF
    IF firstText = 0 OR devText <> lastOut THEN
        STATUSBAR SET TEXT g_hDlg, %IDC_STATUSBAR, 4, 0, devText
        lastOut = devText
    END IF

    firstText = 1
END SUB

' RefreshUI - Public entry (timer / init / button handlers).
'   g_uiDirty (set by UpdateDevice/HB-timeout)  -> structure sync (insert/delete)
'   Always                                    -> per-cell point update
'   g_sbDirty (set by DiscOpenPort/Close)      -> statusbar parts recompute
' WM_SETREDRAW + InvalidateRect happen ONLY on structural change (was every 2s).
SUB RefreshUI()
    LOCAL i AS LONG, cnt AS LONG, strmCnt AS LONG
    LOCAL hLV AS DWORD, doStruct AS LONG, forceAll AS LONG
    LOCAL selIdx AS LONG, findIdx AS LONG, chkParam AS LONG, lvCount AS LONG
    DIM snap(%MAX_DEVICES - 1) AS LOCAL DeviceInfo

    CONTROL HANDLE g_hDlg, %IDC_LISTVIEW TO hLV
    IF hLV = 0 THEN EXIT SUB

    EnterCriticalSection g_csDev
    FOR i = 0 TO %MAX_DEVICES - 1
        snap(i) = g_Devs(i)
        ' Clear per-device format dirty: GUI will paint this pass.
        g_Devs(i).bFmtDirty = 0
    NEXT i
    doStruct = g_uiDirty
    g_uiDirty = 0
    LeaveCriticalSection g_csDev

    IF doStruct THEN
        CONTROL SEND g_hDlg, %IDC_LISTVIEW, %WM_SETREDRAW, 0, 0
        selIdx = GetSelectedDevIdx()
        UiSyncStructure snap()
        ' After structural change force full static + dynamic paint.
        forceAll = 1
    END IF

    cnt = 0
    strmCnt = 0
    FOR i = 0 TO %MAX_DEVICES - 1
        IF snap(i).dwActive = 0 THEN ITERATE FOR
        INCR cnt
        IF snap(i).dwHBActive THEN INCR strmCnt
        UiPaintDevice i, snap(i), forceAll OR snap(i).bFmtDirty
    NEXT i

    IF doStruct THEN
        ' Restore selection by device index.
        IF selIdx >= 0 THEN
            LISTVIEW GET COUNT g_hDlg, %IDC_LISTVIEW TO lvCount
            FOR findIdx = 1 TO lvCount
                LISTVIEW GET USER g_hDlg, %IDC_LISTVIEW, findIdx TO chkParam
                IF chkParam = selIdx THEN
                    LISTVIEW SELECT g_hDlg, %IDC_LISTVIEW, findIdx
                    EXIT FOR
                END IF
            NEXT findIdx
        END IF
        CONTROL SEND g_hDlg, %IDC_LISTVIEW, %WM_SETREDRAW, 1, 0
        ' Invalidate ONLY when structure changed - not every 2s.
        InvalidateRect hLV, BYVAL %NULL, %FALSE
    END IF

    UiUpdateStatusBar cnt, strmCnt
    UpdateButtonStates

    ' 1.4: If AudioThread set g_dumpBtnDirty (dump-open-fail), reset button
    ' caption to DUMP. This runs in the GUI thread - no deadlock risk.
    IF g_dumpBtnDirty THEN
        g_dumpBtnDirty = 0
        CONTROL SET TEXT g_hDlg, %IDC_BTN_DUMP, "DUMP"
    END IF
END SUB

' ============================================================================
'  Main Dialog Callback
' ============================================================================

#ENDIF
