' ============================================================================
'  ui_dialogs.inc — UI dialogs (Add Device, Discovery Ports)
' ============================================================================
#IF NOT %DEF(%UI_DIALOGS_INC)
%UI_DIALOGS_INC = 1

CALLBACK FUNCTION AddDeviceProc()
    SELECT CASE AS LONG CB.MSG
        CASE %WM_COMMAND
            IF CB.CTL = %IDOK THEN
                LOCAL sIP AS STRING
                LOCAL sPort AS STRING
                LOCAL targetIP AS DWORD
                LOCAL targetPort AS DWORD

                CONTROL GET TEXT CB.HNDL, %IDC_AD_IP TO sIP
                CONTROL GET TEXT CB.HNDL, %IDC_AD_PORT TO sPort

                sIP = TRIM$(sIP)
                sPort = TRIM$(sPort)

                IF LEN(sIP) = 0 THEN
                    MSGBOX "Enter device IP address", %MB_ICONWARNING, "Add Device"
                    FUNCTION = 1
                    EXIT FUNCTION
                END IF

                ' Parse IP address (dotted quad -> DWORD in network byte order)
                targetIP = inet_addr(BYCOPY sIP)
                IF targetIP = %INADDR_NONE THEN
                    MSGBOX "Invalid IP address: " & sIP, %MB_ICONWARNING, "Add Device"
                    FUNCTION = 1
                    EXIT FUNCTION
                END IF

                ' Parse port (default 3950)
                IF LEN(sPort) = 0 THEN sPort = "3950"
                targetPort = VAL(sPort)
                IF targetPort < 1 OR targetPort > 65535 THEN
                    MSGBOX "Port must be 1-65535", %MB_ICONWARNING, "Add Device"
                    FUNCTION = 1
                    EXIT FUNCTION
                END IF

                ' DiscAnyOpen() replaces the old g_fDiscOpen check.
                IF DiscAnyOpen() THEN
                    SendDiscover targetIP, targetPort
                    AddLog "Manual DISCOVER sent to " & sIP & ":" & TRIM$(STR$(targetPort))
                    ' Save to INI: manual device list (auto-restored on restart)
                    ' + last IP/port (pre-fills Add Device dialog next time).
                    ManualDeviceAdd sIP, targetPort
                    SaveAddDeviceDefaults sIP, sPort
                ELSE
                    MSGBOX "No discovery ports open", %MB_ICONWARNING, "Add Device"
                END IF

                DIALOG END CB.HNDL, 1
                FUNCTION = 1
                EXIT FUNCTION

            ELSEIF CB.CTL = %IDCANCEL THEN
                DIALOG END CB.HNDL, 0
                FUNCTION = 1
                EXIT FUNCTION
            END IF
    END SELECT
END FUNCTION

SUB ShowAddDeviceDlg(BYVAL hParent AS DWORD)
    LOCAL hDlg AS DWORD
    LOCAL sDefIP AS STRING
    LOCAL sDefPort AS STRING

    ' Pre-fill with last entered values from INI (or defaults if first run).
    sDefIP   = LoadAddDeviceDefaultIP()
    sDefPort = LoadAddDeviceDefaultPort()

    DIALOG NEW hParent, "Add Device", , , 200, 90, %WS_POPUP OR %WS_BORDER OR _
              %WS_CAPTION OR %WS_SYSMENU OR %DS_CENTER, %WS_EX_DLGMODALFRAME TO hDlg

    CONTROL ADD LABEL,  hDlg, -1, "Device IP:", 10, 12, 50, 10
    CONTROL ADD TEXTBOX, hDlg, %IDC_AD_IP, sDefIP, 65, 10, 125, 12

    CONTROL ADD LABEL,  hDlg, -1, "Port:", 10, 32, 50, 10
    CONTROL ADD TEXTBOX, hDlg, %IDC_AD_PORT, sDefPort, 65, 30, 50, 12

    CONTROL ADD BUTTON, hDlg, %IDOK, "Discover", 45, 55, 50, 15, %WS_TABSTOP OR %BS_DEFPUSHBUTTON
    CONTROL ADD BUTTON, hDlg, %IDCANCEL, "Cancel", 105, 55, 50, 15, %WS_TABSTOP

    DIALOG SHOW MODAL hDlg, CALL AddDeviceProc
END SUB

' RefreshPortsListView - populate the ListView with current g_DiscPorts
' %LVM_INSERTITEM with LISTVIEW INSERT ITEM, and %LVM_SETITEMTEXT with
' LISTVIEW SET TEXT. Removed the LV_ITM struct (DDT handles it internally).
' Items/columns are 1-based: lvRow now starts at 1; Status is column 2.
SUB RefreshPortsListView(BYVAL hDlg AS DWORD)
    LOCAL hLV AS DWORD
    LOCAL i AS LONG
    LOCAL szText AS ASCIIZ * 32
    LOCAL lvRow AS LONG

    CONTROL HANDLE hDlg, %IDC_DP_LISTVIEW TO hLV
    IF hLV = 0 THEN EXIT SUB
    LISTVIEW RESET hDlg, %IDC_DP_LISTVIEW

    lvRow = 0
    FOR i = 0 TO %MAX_DISC_PORTS - 1
        IF g_DiscPorts(i).isOpen THEN
            INCR lvRow                       ' 1-based item position
            szText = TRIM$(STR$(g_DiscPorts(i).port))
            LISTVIEW INSERT ITEM hDlg, %IDC_DP_LISTVIEW, lvRow, 0, szText
            LISTVIEW SET USER hDlg, %IDC_DP_LISTVIEW, lvRow, i

            szText = "Open"
            LISTVIEW SET TEXT hDlg, %IDC_DP_LISTVIEW, lvRow, 2, szText
        END IF
    NEXT i
    IF lvRow >= %MAX_DISC_PORTS THEN
        CONTROL DISABLE hDlg, %IDC_DP_ADD
    ELSE
        CONTROL ENABLE hDlg, %IDC_DP_ADD
    END IF
END SUB

' PortInputProc - validate-only port number dialog callback.
' Stores the validated port in g_tmpPort (1..65535) on OK, 0 on cancel.
' Does NOT call DiscOpenPort - that is the caller's responsibility, so the
' caller can implement a proper transactional Change (open NEW first, only
' close OLD if open succeeded).
CALLBACK FUNCTION PortInputProc()
    SELECT CASE AS LONG CB.MSG
        CASE %WM_COMMAND
            IF CB.CTL = %IDOK THEN
                LOCAL sPort AS STRING, nPort AS LONG
                CONTROL GET TEXT CB.HNDL, 101 TO sPort
                sPort = TRIM$(sPort)
                nPort = VAL(sPort)
                IF nPort > 0 AND nPort < 65536 THEN
                    g_tmpPort = nPort
                    DIALOG END CB.HNDL, 1
                    FUNCTION = 1
                ELSE
                    MSGBOX "Port must be 1-65535.", %MB_ICONWARNING, "Port"
                    FUNCTION = 1
                END IF
            ELSEIF CB.CTL = %IDCANCEL THEN
                g_tmpPort = 0
                DIALOG END CB.HNDL, 0
                FUNCTION = 1
            END IF
    END SELECT
END FUNCTION

CALLBACK FUNCTION DiscPortsProc()
    SELECT CASE AS LONG CB.MSG
        CASE %WM_INITDIALOG
            RefreshPortsListView CB.HNDL
            ' Disable Close/Change initially (no selection)
            CONTROL DISABLE CB.HNDL, %IDC_DP_CLOSE
            CONTROL DISABLE CB.HNDL, %IDC_DP_CHANGE
            FUNCTION = 1
        CASE %WM_NOTIFY
            ' Enable/disable Close/Change based on ListView selection
            LOCAL pnmh AS NMHDR PTR
            pnmh = CB.LPARAM
            IF @pnmh.idFrom = %IDC_DP_LISTVIEW THEN
                IF @pnmh.code = %LVN_ITEMCHANGED THEN
                    ' (returns 0-based, -1 = none) with LISTVIEW GET SELECT
                    ' (returns 1-based, 0 = none). Removed unused hLVN local.
                    LOCAL selN AS LONG
                    LISTVIEW GET SELECT CB.HNDL, %IDC_DP_LISTVIEW TO selN
                    IF selN > 0 THEN
                        CONTROL ENABLE CB.HNDL, %IDC_DP_CLOSE
                        CONTROL ENABLE CB.HNDL, %IDC_DP_CHANGE
                    ELSE
                        CONTROL DISABLE CB.HNDL, %IDC_DP_CLOSE
                        CONTROL DISABLE CB.HNDL, %IDC_DP_CHANGE
                    END IF
                END IF
            END IF
        CASE %WM_COMMAND
            SELECT CASE AS LONG CB.CTL
                CASE %IDC_DP_ADD
                    ' Show mini add-port dialog. PortInputProc validates the
                    ' number only; we call DiscOpenPort here so the failure
                    ' path (port in use / no slots) leaves the user back at
                    ' the dialog instead of silently doing nothing.
                    LOCAL hMini AS DWORD
                    g_tmpPort = 0
                    DIALOG NEW CB.HNDL, "Add Port", , , 180, 70, _
                              %WS_POPUP OR %WS_CAPTION OR %WS_SYSMENU OR %DS_CENTER, _
                              %WS_EX_DLGMODALFRAME TO hMini
                    CONTROL ADD LABEL,  hMini, 100, "Port:", 10, 10, 50, 12
                    CONTROL ADD TEXTBOX, hMini, 101, "3951", 10, 25, 80, 12, %WS_TABSTOP
                    CONTROL ADD BUTTON,  hMini, %IDOK, "OK", 100, 25, 35, 14, %BS_DEFPUSHBUTTON OR %WS_TABSTOP
                    CONTROL ADD BUTTON,  hMini, %IDCANCEL, "Cancel", 138, 25, 35, 14, %WS_TABSTOP
                    DIALOG SHOW MODAL hMini, CALL PortInputProc
                    IF g_tmpPort > 0 THEN
                        IF DiscOpenPort(g_tmpPort) THEN
                            AddLog "Discovery port " & TRIM$(STR$(g_tmpPort)) & " added"
                            SaveDiscPortsToIni
                        ELSE
                            MSGBOX "Could not open port " & TRIM$(STR$(g_tmpPort)) & "." & $CRLF & _
                                   "It may already be in use or all slots are full.", _
                                   %MB_ICONWARNING, "Add Port"
                        END IF
                    END IF
                    RefreshPortsListView CB.HNDL
                    ' After refresh, disable Close/Change (selection cleared)
                    CONTROL DISABLE CB.HNDL, %IDC_DP_CLOSE
                    CONTROL DISABLE CB.HNDL, %IDC_DP_CHANGE
                    FUNCTION = 1
                CASE %IDC_DP_CLOSE
                    ' Close selected port. DiscClosePortEx returns the port
                    ' number BEFORE clearing the slot, so we can log the
                    ' correct port (old code logged 0 because it read the
                    ' already-cleared slot field).
                    LOCAL selIdx AS LONG, lvParam AS LONG, closedPort AS LONG
                    LISTVIEW GET SELECT CB.HNDL, %IDC_DP_LISTVIEW TO selIdx
                    IF selIdx = 0 THEN
                        MSGBOX "Select a port to close first.", %MB_ICONINFORMATION, "Discovery Ports"
                        FUNCTION = 1 : EXIT FUNCTION
                    END IF
                    LISTVIEW GET USER CB.HNDL, %IDC_DP_LISTVIEW, selIdx TO lvParam
                    IF lvParam >= 0 AND lvParam < %MAX_DISC_PORTS THEN
                        IF g_DiscPorts(lvParam).isOpen THEN
                            closedPort = DiscClosePortEx(lvParam)
                            SaveDiscPortsToIni
                            AddLog "Discovery port " & TRIM$(STR$(closedPort)) & " closed"
                        END IF
                    END IF
                    RefreshPortsListView CB.HNDL
                    ' After refresh, disable Close/Change (selection cleared)
                    CONTROL DISABLE CB.HNDL, %IDC_DP_CLOSE
                    CONTROL DISABLE CB.HNDL, %IDC_DP_CHANGE
                    FUNCTION = 1
                CASE %IDC_DP_CHANGE
                    ' Change selected port number (TRANSACTIONAL).
                    '   1. PortInputProc validates the new number (does NOT open it).
                    '   2. If new == old -> no-op.
                    '   3. Try DiscOpenPort(new). On success, close OLD and save.
                    '   4. If DiscOpenPort fails, OLD port stays open and user
                    '      sees an error (no port left half-closed).
                    ' The old logic called AddPortMiniProc (which opened the
                    ' new port unconditionally) then closed the old one - if
                    ' open failed the user lost both ports.
                    LOCAL selC AS LONG, lvParamC AS LONG
                    LOCAL hChg AS DWORD, sOldPort AS STRING
                    LOCAL oldPort AS LONG, nNew AS LONG, chgResult AS LONG

                    LISTVIEW GET SELECT CB.HNDL, %IDC_DP_LISTVIEW TO selC
                    IF selC = 0 THEN
                        MSGBOX "Select a port to change first.", %MB_ICONINFORMATION, "Discovery Ports"
                        FUNCTION = 1 : EXIT FUNCTION
                    END IF
                    LISTVIEW GET USER CB.HNDL, %IDC_DP_LISTVIEW, selC TO lvParamC
                    IF lvParamC < 0 OR lvParamC >= %MAX_DISC_PORTS THEN
                        FUNCTION = 1 : EXIT FUNCTION
                    END IF
                    IF g_DiscPorts(lvParamC).isOpen = 0 THEN
                        FUNCTION = 1 : EXIT FUNCTION
                    END IF

                    oldPort  = g_DiscPorts(lvParamC).port
                    sOldPort = TRIM$(STR$(oldPort))
                    g_tmpPort = 0

                    DIALOG NEW CB.HNDL, "Change Port", , , 180, 70, _
                              %WS_POPUP OR %WS_CAPTION OR %WS_SYSMENU OR %DS_CENTER, _
                              %WS_EX_DLGMODALFRAME TO hChg
                    CONTROL ADD LABEL,  hChg, 100, "New port:", 10, 10, 60, 12
                    CONTROL ADD TEXTBOX, hChg, 101, sOldPort, 10, 25, 80, 12, %WS_TABSTOP
                    CONTROL ADD BUTTON,  hChg, %IDOK, "OK", 100, 25, 35, 14, %BS_DEFPUSHBUTTON OR %WS_TABSTOP
                    CONTROL ADD BUTTON,  hChg, %IDCANCEL, "Cancel", 138, 25, 35, 14, %WS_TABSTOP
                    DIALOG SHOW MODAL hChg, CALL PortInputProc TO chgResult

                    IF chgResult = 1 THEN
                        nNew = g_tmpPort
                        IF nNew = oldPort THEN
                            ' no-op (user confirmed the same port)
                        ELSEIF DiscOpenPort(nNew) THEN
                            ' Open NEW first - only then close OLD. If the
                            ' close fails we still have the new port working.
                            DiscClosePortEx lvParamC
                            SaveDiscPortsToIni
                            AddLog "Port changed " & sOldPort & " -> " & TRIM$(STR$(nNew))
                        ELSE
                            MSGBOX "Could not open port " & TRIM$(STR$(nNew)) & "." & $CRLF & _
                                   "Old port " & sOldPort & " kept open.", _
                                   %MB_ICONWARNING, "Change Port"
                        END IF
                    END IF
                    RefreshPortsListView CB.HNDL
                    ' After refresh, disable Close/Change (selection cleared)
                    CONTROL DISABLE CB.HNDL, %IDC_DP_CLOSE
                    CONTROL DISABLE CB.HNDL, %IDC_DP_CHANGE
                    FUNCTION = 1
                CASE %IDC_DP_DONE
                    DIALOG END CB.HNDL, 1
                    FUNCTION = 1
            END SELECT
    END SELECT
END FUNCTION

SUB ShowDiscPortsDlg(BYVAL hParent AS DWORD)
    LOCAL hDlg AS DWORD

    DIALOG NEW hParent, "Discovery Ports", , , 275, 180, _
              %WS_POPUP OR %WS_CAPTION OR %WS_SYSMENU OR %DS_CENTER, _
              %WS_EX_DLGMODALFRAME TO hDlg

    CONTROL ADD LISTVIEW, hDlg, %IDC_DP_LISTVIEW, "", _
               5, 5, 200, 140, _
               %WS_CHILD OR %WS_VISIBLE OR %WS_TABSTOP OR _
               %LVS_REPORT OR %LVS_SINGLESEL OR %LVS_SHOWSELALWAYS, _
               %WS_EX_CLIENTEDGE

    ' ListView columns (REFACTOR LISTVIEW-DDT): replaced
    ' %LVM_INSERTCOLUMN with LISTVIEW INSERT COLUMN (1-based columns)
    ' and %LVM_SETEXTENDEDLISTVIEWSTYLE with LISTVIEW SET STYLEXX.
    ' Removed LOCAL lvc AS LV_COL and szText (DDT handles them internally).
    LISTVIEW INSERT COLUMN hDlg, %IDC_DP_LISTVIEW, 1, "Port",   80, 0
    LISTVIEW INSERT COLUMN hDlg, %IDC_DP_LISTVIEW, 2, "Status", 80, 0
    LISTVIEW SET STYLEXX hDlg, %IDC_DP_LISTVIEW, _
        %LVS_EX_FULLROWSELECT OR %LVS_EX_GRIDLINES

    CONTROL ADD BUTTON, hDlg, %IDC_DP_ADD,    "Open New Port", 212, 5,   55, 16, %WS_TABSTOP
    CONTROL ADD BUTTON, hDlg, %IDC_DP_CLOSE,  "Close Port",    212, 25,  55, 16, %WS_TABSTOP
    CONTROL ADD BUTTON, hDlg, %IDC_DP_CHANGE, "Change Port",   212, 45,  55, 16, %WS_TABSTOP
    CONTROL ADD BUTTON, hDlg, %IDC_DP_DONE,   "Done",          212, 155, 55, 16, %WS_TABSTOP OR %BS_DEFPUSHBUTTON

    DIALOG SHOW MODAL hDlg, CALL DiscPortsProc
END SUB

#ENDIF
