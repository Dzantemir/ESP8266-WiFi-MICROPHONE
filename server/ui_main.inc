' ============================================================================
'  ui_main.inc — Main dialog callback (MainDlgProc)
' ============================================================================
#IF NOT %DEF(%UI_MAIN_INC)
%UI_MAIN_INC = 1

CALLBACK FUNCTION MainDlgProc()

    SELECT CASE CB.MSG

        CASE %WM_INITDIALOG
            ' Set the dialog icon (caption bar + Alt+Tab task list) from the
            ' embedded #RESOURCE ICON, 100. Per PowerBASIC docs, DIALOG SET
            ' ICON takes the resource ID as "#<id>" (integral) or the text
            ' name, and assigns BOTH the small (caption) and large (Alt+Tab)
            ' icons in one call. No LoadImage/WM_SETICON needed — that is the
            ' raw SDK way; this is the DDT way.
            DIALOG SET ICON CB.HNDL, "#100"

            ' Set timer for UI refresh
            SetTimer CB.HNDL, %IDT_REFRESH, %REFRESH_MS, %NULL
            ' Initial layout
            ResizeControls
            ' Populate StatusBar immediately so it isn't blank for the first
            ' %REFRESH_MS (2s) until the timer's first tick. Safe: the bar and
            ' its 4 parts are created in WinMain before DIALOG SHOW MODAL, and
            ' RefreshUI has an hLV=0 guard.
            RefreshUI
            ' Initial button states (all disabled)
            UpdateButtonStates

        CASE %WM_ENTERSIZEMOVE
            ' Pause refresh timer during window move/resize
            KillTimer CB.HNDL, %IDT_REFRESH
            FUNCTION = 0
            EXIT FUNCTION

        CASE %WM_EXITSIZEMOVE
            ' Resume refresh timer and update after resize ends
            SetTimer CB.HNDL, %IDT_REFRESH, %REFRESH_MS, %NULL
            'RefreshUI
            FUNCTION = 0
            EXIT FUNCTION

        CASE %WM_SIZE
            ' Resize all controls atomically FIRST - this positions
            ' StatusBar at the bottom via DeferWindowPos with SWP_NOCOPYBITS,
            ' so no BitBlt of old pixels over buttons.
            ResizeControls
            ' THEN let StatusBar recalculate its internal parts (section
            ' widths, size grip). Since ResizeControls already positioned
            ' it correctly, the StatusBar's internal SetWindowPos detects
            ' no position change and is a no-op - no BitBlt, no artifacts.
            CONTROL SEND CB.HNDL, %IDC_STATUSBAR, %WM_SIZE, CB.WPARAM, CB.LPARAM
            FUNCTION = 1
            EXIT FUNCTION

        CASE %WM_GETMINMAXINFO
            ' Set minimum window size to prevent controls from overlapping
            LOCAL pmmi AS MINMAXINFO PTR
            pmmi = CB.LPARAM
            @pmmi.ptMinTrackSize.x = 500
            @pmmi.ptMinTrackSize.y = 300
            FUNCTION = 0       ' let default processing apply the changes
            EXIT FUNCTION

        CASE %WM_TIMER
            ' Distinguish timers by ID (CB.WPARAM = timer ID).
            IF CB.WPARAM = %IDT_DEVCHANGE THEN
                ' Debounce timer fired: all WM_DEVICECHANGE / WM_POWERBROADCAST
                ' events have settled (no new device changes for 2 s). Now it's
                ' safe to reopen WaveOut — all audio devices (HDMI, USB, etc.)
                ' have finished re-initializing. Flag reopen for all running
                ' streams.
                KillTimer CB.HNDL, %IDT_DEVCHANGE
                AddLog "[DEV] Device change debounce complete - flagging WaveOut reopen"
                LOCAL dt_rc AS LONG
                FOR dt_rc = 0 TO %MAX_DEVICES - 1
                    ' 6.2-fix: read dwRunning under CS
                    EnterCriticalSection g_csDev
                    IF g_Devs(dt_rc).dwRunning THEN
                        g_Devs(dt_rc).bReopenWaveOut = 1
                    END IF
                    LeaveCriticalSection g_csDev
                NEXT dt_rc
                FUNCTION = 1
                EXIT FUNCTION
            END IF
            ' Default: UI refresh timer (%IDT_REFRESH)
            RefreshUI
            FUNCTION = 1
            EXIT FUNCTION

        CASE %WM_COMMAND
            SELECT CASE CB.CTL
                CASE %IDC_BTN_START
                    IF CB.CTLMSG = %BN_CLICKED THEN
                        StartCheckedStreams
                        RefreshUI
                        FUNCTION = 1
                    END IF

                CASE %IDC_BTN_STOP
                    IF CB.CTLMSG = %BN_CLICKED THEN
                        StopCheckedStreams
                        RefreshUI
                        FUNCTION = 1
                    END IF

                CASE %IDC_BTN_STOPALL
                    IF CB.CTLMSG = %BN_CLICKED THEN
                        StopAllStreams
                        RefreshUI
                        FUNCTION = 1
                    END IF

                CASE %IDC_BTN_DUMP
                    IF CB.CTLMSG = %BN_CLICKED THEN
                        ToggleDump
                        FUNCTION = 1
                    END IF

                CASE %IDC_COMBO_DEVICE
                    IF CB.CTLMSG = %CBN_SELCHANGE THEN
                        ' Change output device for the SELECTED device in ListView.
                        ' If no device selected, change for all (global default).
                        LOCAL selIdx AS LONG
                        LOCAL selDevIdx AS LONG
                        selDevIdx = GetSelectedDevIdx()
                        COMBOBOX GET SELECT g_hDlg, %IDC_COMBO_DEVICE TO selIdx
                        IF selIdx <= 1 THEN
                            IF selDevIdx >= 0 THEN
                                SetDeviceWaveOutput selDevIdx, -1
                            ELSE
                                AddLog "Select a device in ListView first, then choose output"
                            END IF
                        ELSE
                            IF selDevIdx >= 0 THEN
                                SetDeviceWaveOutput selDevIdx, selIdx - 2
                            ELSE
                                AddLog "Select a device in ListView first, then choose output"
                            END IF
                        END IF
                        FUNCTION = 1
                    END IF
            END SELECT

        CASE %WM_NOTIFY
            LOCAL pnmh AS NMHDR PTR
            pnmh = CB.LPARAM
            ' Handle ListView checkbox state changes for immediate button update
            IF @pnmh.hwndFrom THEN
                LOCAL hLV AS DWORD
                CONTROL HANDLE CB.HNDL, %IDC_LISTVIEW TO hLV
                IF @pnmh.hwndFrom = hLV AND @pnmh.code = %LVN_ITEMCHANGED THEN
                    LOCAL uChanged AS DWORD
                    LOCAL uNewState AS DWORD
                    LOCAL uOldState AS DWORD
                    uChanged  = PEEK(DWORD, CB.LPARAM + 28)   ' NM_LISTVIEW.uChanged offset
                    uNewState = PEEK(DWORD, CB.LPARAM + 20)   ' NM_LISTVIEW.uNewState offset
                    uOldState = PEEK(DWORD, CB.LPARAM + 24)   ' NM_LISTVIEW.uOldState offset
                    IF (uChanged AND %LVIF_STATE) THEN
                        ' Checkbox state image changed? (bits 12-15)
                        IF (uNewState AND &HF000) <> (uOldState AND &HF000) THEN
                            UpdateButtonStates
                        END IF
                    END IF
                END IF
                ' Right-click on ListView → show context menu
                IF @pnmh.hwndFrom = hLV AND @pnmh.code = %NM_RCLICK THEN
                    LOCAL hPopup AS DWORD
                    LOCAL pt AS POINTAPI
                    LOCAL i AS LONG
                    LOCAL nCount AS LONG
                    LOCAL nChecked AS LONG
                    LOCAL nStreaming AS LONG

                    MENU NEW POPUP TO hPopup
                    IF hPopup THEN
                        ' Count checked + streaming for menu state
                        ' under CS to avoid cosmetic races with AudioThread
                        ' (which writes these under CS). Without this, the
                        ' context menu could show a stale checkmark or
                        ' grayed-out state for one menu invocation.
                        LOCAL ctxDevIdx AS LONG
                        DIM ctxHBActive(%MAX_DEVICES - 1) AS LOCAL LONG
                        DIM ctxWaveDev(%MAX_DEVICES - 1) AS LOCAL LONG
                        EnterCriticalSection g_csDev
                        FOR ctxDevIdx = 0 TO %MAX_DEVICES - 1
                            ctxHBActive(ctxDevIdx) = g_Devs(ctxDevIdx).dwHBActive
                            ctxWaveDev(ctxDevIdx)  = g_Devs(ctxDevIdx).dwWaveDevice
                        NEXT ctxDevIdx
                        LeaveCriticalSection g_csDev

                        ' LISTVIEW GET COUNT, and %LVM_GETITEM (lParam
                        ' retrieval) with LISTVIEW GET USER. Loop index   ' i'
                        ' is 0-based for IsItemChecked (checkbox state still
                        ' uses CONTROL SEND); LISTVIEW GET USER uses i + 1.
                        LOCAL lvParam AS LONG
                        LISTVIEW GET COUNT g_hDlg, %IDC_LISTVIEW TO nCount
                        FOR i = 0 TO nCount - 1
                            IF IsItemChecked(hLV, i) THEN INCR nChecked
                            LISTVIEW GET USER g_hDlg, %IDC_LISTVIEW, i + 1 TO lvParam
                            IF lvParam >= 0 AND lvParam < %MAX_DEVICES THEN
                                IF ctxHBActive(lvParam) THEN INCR nStreaming
                            END IF
                        NEXT i

                        ' Build menu
                        MENU ADD STRING, hPopup, "Start Stream", %IDM_CTX_START, %MFS_ENABLED
                        MENU ADD STRING, hPopup, "Stop Stream",  %IDM_CTX_STOP,  %MFS_ENABLED

                        ' Output Device submenu (for the selected device)
                        LOCAL hSubOutput AS DWORD
                        LOCAL selDev AS LONG
                        LOCAL numDevs AS LONG
                        LOCAL di AS LONG
                        LOCAL capsOut AS WAVEOUTCAPS
                        LOCAL sDevName AS STRING

                        MENU NEW POPUP TO hSubOutput
                        selDev = GetSelectedDevIdx()

                        ' "Default (WAVE_MAPPER)" - ID = &H2100
                        MENU ADD STRING, hSubOutput, "Default (WAVE_MAPPER)", &H2100, %MFS_ENABLED
                        IF selDev >= 0 AND ctxWaveDev(selDev) = -1 THEN
                            MENU SET STATE hSubOutput, BYCMD &H2100, %MFS_CHECKED
                        END IF

                        ' Enumerate WaveOut devices
                        numDevs = waveOutGetNumDevs()
                        FOR di = 0 TO numDevs - 1
                            IF waveOutGetDevCaps(di, capsOut, SIZEOF(capsOut)) = 0 THEN
                                sDevName = EXTRACT$(capsOut.szPname, CHR$(0))
                                IF LEN(sDevName) = 0 THEN sDevName = "Device " & TRIM$(STR$(di))
                                ' ID = &H2101 + di (device 0 = &H2101, 1 = &H2102, ...)
                                MENU ADD STRING, hSubOutput, TRIM$(STR$(di)) & ": " & sDevName, &H2101 + di, %MFS_ENABLED
                                IF selDev >= 0 AND ctxWaveDev(selDev) = di THEN
                                    MENU SET STATE hSubOutput, BYCMD &H2101 + di, %MFS_CHECKED
                                END IF
                            END IF
                        NEXT di

                        MENU ADD STRING, hPopup, "-", 0, 0
                        MENU ADD POPUP, hPopup, "Output Device", hSubOutput, %MFS_ENABLED
                        MENU ADD STRING, hPopup, "-", 0, 0
                        MENU ADD STRING, hPopup, "Select All", %IDM_CTX_SELECTALL, %MFS_ENABLED
                        MENU ADD STRING, hPopup, "Clear All",  %IDM_CTX_CLEARALL,  %MFS_ENABLED
                        MENU ADD STRING, hPopup, "-", 0, 0
                        MENU ADD STRING, hPopup, "Stop All Streams", %IDM_CTX_STOPALL, %MFS_ENABLED

                        ' Enable/disable based on state
                        IF nChecked = 0 THEN
                            MENU SET STATE hPopup, BYCMD %IDM_CTX_START, %MFS_GRAYED
                            MENU SET STATE hPopup, BYCMD %IDM_CTX_STOP,  %MFS_GRAYED
                        END IF
                        IF nStreaming = 0 THEN
                            MENU SET STATE hPopup, BYCMD %IDM_CTX_STOPALL, %MFS_GRAYED
                        END IF
                        IF nCount = 0 THEN
                            MENU SET STATE hPopup, BYCMD %IDM_CTX_SELECTALL, %MFS_GRAYED
                            MENU SET STATE hPopup, BYCMD %IDM_CTX_CLEARALL,  %MFS_GRAYED
                        END IF
                        ' Output Device submenu grayed via outState in MENU ADD POPUP

                        ' Show menu at cursor position
                        GetCursorPos pt
                        LOCAL ctxCmd AS LONG
                        MENU CONTEXT hPopup, pt.x, pt.y, %TPM_LEFTALIGN OR %TPM_TOPALIGN OR %TPM_RETURNCMD OR %TPM_RIGHTBUTTON TO ctxCmd
                        ' MENU CONTEXT handles cleanup automatically
                        ' Per MSDN, DestroyMenu is recursive: it destroys the menu
                        ' AND all its submenus. hSubOutput was attached to hPopup
                        ' via AppendMenu(MF_POPUP), so DestroyMenu hPopup already
                        ' freed it. Calling DestroyMenu hSubOutput again was a
                        ' use-after-free (usually silent, occasionally crashes on
                        ' heap introspection). hSubOutput is now a dangling handle
                        ' - do not touch it.

                        ' Handle selected menu item
                        SELECT CASE ctxCmd
                            CASE %IDM_CTX_START
                                StartCheckedStreams
                                RefreshUI
                            CASE %IDM_CTX_STOP
                                StopCheckedStreams
                                RefreshUI
                            CASE %IDM_CTX_STOPALL
                                StopAllStreams
                                RefreshUI
                            CASE %IDM_CTX_SELECTALL
                                ' Check all items in ListView
                                FOR i = 0 TO nCount - 1
                                    SetItemChecked hLV, i, 1
                                NEXT i
                                UpdateButtonStates
                            CASE %IDM_CTX_CLEARALL
                                ' Uncheck all items in ListView
                                FOR i = 0 TO nCount - 1
                                    SetItemChecked hLV, i, 0
                                NEXT i
                                UpdateButtonStates
                            CASE &H2100
                                ' "Default (WAVE_MAPPER)" selected for the focused device
                                IF selDev >= 0 THEN
                                    SetDeviceWaveOutput selDev, -1
                                END IF
                            CASE &H2101 TO &H21FF
                                ' Specific device ID = ctxCmd - &H2101
                                IF selDev >= 0 THEN
                                    SetDeviceWaveOutput selDev, ctxCmd - &H2101
                                END IF
                        END SELECT
                        FUNCTION = 1
                    END IF
                END IF
            END IF
            ' Do NOT set FUNCTION = 1 here - let other notifications
            ' (HDN_BEGINTRACK, HDN_DIVIDERDBLCLICK, etc.) pass through
            ' to default processing so column resizing works

        CASE %WM_UDP_DISC_BASE TO %WM_UDP_DISC_BASE + %MAX_DISC_PORTS - 1
            ' Late FD_READ arriving during shutdown: sockets are being torn
            ' down. Drop silently - do NOT touch g_DiscPorts or call UDP RECV.
            IF g_bShuttingDown THEN
                FUNCTION = 1
                EXIT FUNCTION
            END IF
            IF LO(WORD, CB.LPARAM) = %FD_READ THEN
                LOCAL dpIdx AS LONG
                dpIdx = CB.MSG - %WM_UDP_DISC_BASE
                IF dpIdx >= 0 AND dpIdx < %MAX_DISC_PORTS THEN
                    IF g_DiscPorts(dpIdx).isOpen THEN
                        DiscoveryProc dpIdx
                    END IF
                END IF
                ' NOTE: Do NOT re-arm UDP NOTIFY here.
                ' is auto re-armed by the next UDP RECV (which DiscoveryProc
                ' calls above). It is NOT   ' persistent' in the sense of firing
                ' continuously - it fires once per RECV. But re-issuing the
                ' UDP NOTIFY statement manually here would register a SECOND
                ' notification request and can leak internal handles, so we
                ' correctly do nothing. The single NOTIFY in DiscOpenPort()
                ' plus DiscoveryProc's UDP RECV is the proper re-arm cycle.
            END IF
            FUNCTION = 1
            EXIT FUNCTION

        CASE %WM_APP_LOG
            ' Shutdown: free heap only. Skip CONTROL SEND into the dying
            ' (or already destroyed) log edit control - that path is what
            ' hung/crashed the closer before.
            IF g_bShuttingDown THEN
                IF CB.LPARAM THEN
                    HeapFree g_hHeap, 0, CB.LPARAM
                END IF
                FUNCTION = 1
                EXIT FUNCTION
            END IF
            IF CB.LPARAM THEN
                LOCAL pLogBuf AS BYTE PTR
                LOCAL nLogLen AS LONG
                LOCAL sLogLine AS STRING
                LOCAL nCurLen AS LONG
                pLogBuf = CB.LPARAM
                nLogLen = CB.WPARAM
                IF nLogLen > 0 THEN
                    sLogLine = PEEK$(pLogBuf, nLogLen)
                    CONTROL SEND g_hDlg, %IDC_LOG, %WM_GETTEXTLENGTH, 0, 0 TO nCurLen
                    IF nCurLen > 32768 THEN
                        CONTROL SET TEXT g_hDlg, %IDC_LOG, ""
                        nCurLen = 0
                    END IF
                    CONTROL SEND g_hDlg, %IDC_LOG, %EM_SETSEL, nCurLen, nCurLen
                    CONTROL SEND g_hDlg, %IDC_LOG, %EM_REPLACESEL, 0, STRPTR(sLogLine)
                END IF
                HeapFree g_hHeap, 0, pLogBuf
            END IF
            FUNCTION = 1
            EXIT FUNCTION

        CASE %WM_SYSCOMMAND
            ' Per MS spec: low 4 bits of wParam are reserved by the system
            ' and must be masked off before comparing command IDs.
            ' Without masking, some Windows versions may set bits that
            ' cause the comparison to fail silently.
            IF (CB.WPARAM AND &HFFF0) = %IDM_ADD_DEVICE THEN
                ShowAddDeviceDlg CB.HNDL
                FUNCTION = 1
                EXIT FUNCTION
            END IF

            IF (CB.WPARAM AND &HFFF0) = %IDM_ADD_PORT THEN
                ShowDiscPortsDlg CB.HNDL
                RefreshUI
                FUNCTION = 1
                EXIT FUNCTION
            END IF

            IF (CB.WPARAM AND &HFFF0) = %IDM_ABOUT THEN
                MSGBOX "EASSP Server v1.0" & $CRLF & _
                       "ESP8266 WiFi Microphone" & $CRLF & _
                       "IMA ADPCM | WaveOut | UDP:3950" & $CRLF & _
                       "PowerBASIC | Zero Dependencies", _
                       %MB_OK OR %MB_ICONINFORMATION, "About EASSP Server"
                FUNCTION = 1
                EXIT FUNCTION
            END IF

        CASE %WM_DEVICECHANGE
            ' Windows sends WM_DEVICECHANGE when audio devices are
            ' disconnected/reconnected — including HDMI audio re-init after
            ' sleep/wake. After sleep/wake, MULTIPLE devices may reinitialize
            ' (HDMI audio + USB sound card), so Windows sends several
            ' WM_DEVICECHANGE events in rapid succession.

            ' Instead of immediately flagging reopen on each event (which
            ' causes duplicate reopens and races), we DEBOUNCE: (re)set a 2s
            ' timer on each event. When the timer fires (2s after the LAST
            ' WM_DEVICECHANGE), all devices have settled and we reopen WaveOut
            ' once. The timer is reset on every new event, so only the final
            ' burst end triggers the reopen.

            IF CB.WPARAM = 7 THEN  ' %DBT_DEVNODES_CHANGED
                ' (Re)set the debounce timer. KillTimer + SetTimer resets
                ' the countdown to 2s from now.
                KillTimer CB.HNDL, %IDT_DEVCHANGE
                SetTimer CB.HNDL, %IDT_DEVCHANGE, %DEVCHANGE_DEBOUNCE_MS, %NULL
            END IF
            FUNCTION = 1
            EXIT FUNCTION

        CASE %WM_POWERBROADCAST
            ' Windows sends WM_POWERBROADCAST on sleep/resume:
            '   PBT_APMSUSPEND (4)       — BEFORE sleep (system is suspending)
            '   PBT_APMRESUMEAUTOMATIC (18) — AFTER wake (always sent)
            '   PBT_APMRESUMESUSPEND (7)  — AFTER wake (if triggered by user input)

            ' On PBT_APMSUSPEND: immediately flag WaveOut reopen. This closes
            ' the TCP socket NOW (before the network stack goes down), so when
            ' the system wakes up, the audio thread is already in the reconnect
            ' state. No need to wait for WM_DEVICECHANGE debounce.

            ' On resume (18 or 7): start the debounce timer — after sleep,
            ' multiple WM_DEVICECHANGE events fire as audio devices re-init,
            ' and the debounce timer catches the last one (2s after devices
            ' settle). The TCP reconnect happens inside bReopenWaveOut handling.
            IF CB.WPARAM = 4 THEN  ' PBT_APMSUSPEND — going to sleep
                AddLog "[PWR] System suspending (sleep) - flagging WaveOut + TCP reopen"
                LOCAL ps_rc AS LONG
                FOR ps_rc = 0 TO %MAX_DEVICES - 1
                    ' 6.2-fix: read dwRunning under CS
                    EnterCriticalSection g_csDev
                    IF g_Devs(ps_rc).dwRunning THEN
                        g_Devs(ps_rc).bReopenWaveOut = 1
                    END IF
                    LeaveCriticalSection g_csDev
                NEXT ps_rc
            ELSEIF CB.WPARAM = 18 OR CB.WPARAM = 7 THEN  ' PBT_APMRESUMEAUTOMATIC / PBT_APMRESUMESUSPEND
                AddLog "[PWR] System resumed from sleep - flagging WaveOut + TCP reopen"
                KillTimer CB.HNDL, %IDT_DEVCHANGE
                SetTimer CB.HNDL, %IDT_DEVCHANGE, %DEVCHANGE_DEBOUNCE_MS, %NULL
            END IF
            FUNCTION = 1
            EXIT FUNCTION

        CASE %WM_CLOSE

            ' Correct shutdown order (root cause of WerFault: DestroyWindow
            ' with live UDP NOTIFY on g_hDlg -> AV):
            '   hide -> signal -> wait HB -> DiscCloseAll (drops NOTIFY)
            '        -> snapshot -> DIALOG END (HWND destroy now safe)
            LOCAL c AS LONG, w AS LONG
            LOCAL tmsg AS MSG_type
            LOCAL saIdx AS LONG
            LOCAL wr AS LONG, t0 AS DWORD, lr AS LONG
            DIM stopIPs(%MAX_DEVICES - 1) AS DWORD
            DIM stopPorts(%MAX_DEVICES - 1) AS DWORD
            DIM stopSlots(%MAX_DEVICES - 1) AS LONG
            LOCAL stopCount AS LONG
            stopCount = 0

            ' 0) Block further AddLog PostMessage first (worker threads no-op)
            g_bShuttingDown = 1
            g_bRunning = 0
            KillTimer CB.HNDL, %IDT_REFRESH
            KillTimer CB.HNDL, %IDT_DEVCHANGE

            ' 1) Window disappears NOW; user does not see the cleanup lag.
            ShowWindow CB.HNDL, %SW_HIDE

            ' 2) Collect stop targets + clear stream flags under CS.
            '    Capture IP/Port/slot BEFORE clearing dwHBActive so
            '    SendStopForced can route CMD_STOP to each device.
            EnterCriticalSection g_csDev
            FOR saIdx = 0 TO %MAX_DEVICES - 1
                IF g_Devs(saIdx).dwActive AND g_Devs(saIdx).dwHBActive THEN
                    stopIPs(stopCount)   = g_Devs(saIdx).dwIP
                    stopPorts(stopCount) = g_Devs(saIdx).dwPort
                    stopSlots(stopCount) = g_Devs(saIdx).dwDiscSlot
                    INCR stopCount
                END IF
                g_Devs(saIdx).dwRunning  = 0
                IF g_Devs(saIdx).dwHBActive THEN
                    g_Devs(saIdx).dwHBActive = 0
                    g_Devs(saIdx).dwStatus   = %STS_IDLE
                END IF
            NEXT saIdx
            LeaveCriticalSection g_csDev

            ' 2b) Send CMD_STOP to each active device OUTSIDE CS.
            '     Uses SendStopForced: bypasses g_bShuttingDown guard
            '     in DiscSendOne (already =1 at step 0). Sockets are
            '     still open here - DiscCloseAll runs at step 4.
            '     HB is already blocked by g_bShuttingDown=1, so no
            '     stray CONFIGURE can restart the stream after STOP.
            FOR saIdx = 0 TO stopCount - 1
                SendStopForced stopIPs(saIdx), stopPorts(saIdx), stopSlots(saIdx)
            NEXT saIdx

            ' 3) Wait HB to exit WHILE window is hidden (user does not see).
            '    HB must be dead before DiscCloseAll (HB UDP SEND on closed
            '    socket = AV) AND before DIALOG END (NOTIFY on dead HWND = AV).
            '    Pump queued messages so the GUI thread does not stall and so
            '    FD_READ/WM_APP_LOG do not pile up against the about-to-die HWND.
            IF g_hHbTh THEN
                t0 = GetTickCount()
                DO
                    wr = WaitForSingleObject(g_hHbTh, 50)
                    IF wr = %WAIT_OBJECT_0 OR wr = %WAIT_FAILED THEN EXIT DO
                    ' Drain WM_APP_LOG (free heap) while HWND still valid
                    DO WHILE PeekMessage(tmsg, CB.HNDL, %WM_APP_LOG, %WM_APP_LOG, %PM_REMOVE)
                        IF tmsg.lParam THEN
                            HeapFree g_hHeap, 0, tmsg.lParam
                        END IF
                    LOOP
                    ' Drain late UDP NOTIFY FD_READ (drop, sockets close next)
                    DO WHILE PeekMessage(tmsg, CB.HNDL, %WM_UDP_DISC_BASE, _
                            %WM_UDP_DISC_BASE + %MAX_DISC_PORTS - 1, %PM_REMOVE)
                    LOOP
                    IF (GetTickCount() - t0) >= 2000 THEN EXIT DO
                LOOP
                ' 2.4: Only zero g_hHbTh if thread actually exited (so PBMAIN
                ' can reap a stuck HB thread). If timeout, keep the handle so
                ' PBMAIN's second-chance wait can still join it. Do NOT DiscCloseAll
                ' yet if HB is alive (it may UDP SEND on closing sockets).
                IF wr = %WAIT_OBJECT_0 OR wr = %WAIT_FAILED THEN
                    THREAD CLOSE g_hHbTh TO lr
                    g_hHbTh = 0
                ELSE
                    ' HB did not exit in 2s - keep handle, skip DiscCloseAll.
                    ' PBMAIN will try again; sockets leaked (OS reclaims on exit).
                    ' 4.8: AddLog is no-op after g_bShuttingDown=1. Use OutputDebugString.
                    LOCAL sDbg AS ASCIIZ * 128
                    sDbg = "[SHUTDOWN] HB did not exit in 2s - skipping DiscCloseAll"
                    OutputDebugString sDbg
                    ' Snapshot + END only; leak sockets to avoid crash.
                    GOTO wm_close_snapshot
                END IF
            END IF

            ' 4) Close sockets + drop UDP NOTIFY. MUST happen while g_hDlg is
            '    still alive (NOTIFY is bound to it) and BEFORE DIALOG END.
            DiscCloseAll

            ' 5) Final log drain (HeapFree only - no CONTROL SEND into edit)
            DO WHILE PeekMessage(tmsg, CB.HNDL, %WM_APP_LOG, %WM_APP_LOG, %PM_REMOVE)
                IF tmsg.lParam THEN
                    HeapFree g_hHeap, 0, tmsg.lParam
                END IF
            LOOP

wm_close_snapshot:
            ' 6) Snapshot UI state for INI (window still alive, hidden)
            GetWindowRect CB.HNDL, g_saveWndRect
            FOR c = 0 TO %LV_COL_DUR
                w = 0
                LISTVIEW GET COLUMN g_hDlg, %IDC_LISTVIEW, c + 1 TO w
                g_saveColW(c) = w
            NEXT c

            FUNCTION = 1
            EXIT FUNCTION

         CASE %WM_DESTROY
        PostQuitMessage 0
        EXIT FUNCTION

        END SELECT
END FUNCTION

#ENDIF
