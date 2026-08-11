' ============================================================================
'  ui_layout.inc — Window layout (ResizeControls)
' ============================================================================
#IF NOT %DEF(%UI_LAYOUT_INC)
%UI_LAYOUT_INC = 1

SUB ResizeControls()
    LOCAL hStatus AS DWORD
    LOCAL statusH AS LONG
    LOCAL btnH AS LONG
    LOCAL logH AS LONG
    LOCAL lvH AS LONG
    LOCAL y AS LONG
    LOCAL hDWP AS DWORD
    LOCAL hLV AS DWORD
    LOCAL hLog AS DWORD
    LOCAL hBtn1 AS DWORD
    LOCAL hBtn2 AS DWORD
    LOCAL hBtn3 AS DWORD
    LOCAL hBtn4 AS DWORD
    LOCAL hCombo AS DWORD
    LOCAL hLabel AS DWORD
    LOCAL dwFlags AS DWORD
    LOCAL cx AS DWORD
    LOCAL cy AS DWORD
    LOCAL sx AS DWORD




    DIALOG GET CLIENT g_hDlg TO cx, cy


    IF cx < 400 THEN EXIT SUB
    IF cy < 250 THEN EXIT SUB


    CONTROL GET CLIENT g_hDlg, %IDC_STATUSBAR TO sx, statusH


    btnH = 26
    ' Proportional log height: 25% of available space, clamped 60..250
    logH = (cy - statusH - btnH - 12) \ 4
    IF logH < 60 THEN logH = 60
    IF logH > 250 THEN logH = 250

    ' ListView fills remaining space
    lvH = cy - statusH - btnH - logH - 12
    IF lvH < 60 THEN
        lvH = 60
        logH = cy - statusH - lvH - btnH - 12
        IF logH < 40 THEN logH = 40
    END IF

    ' Get all control handles for DeferWindowPos
    CONTROL HANDLE g_hDlg, %IDC_LISTVIEW    TO hLV
    CONTROL HANDLE g_hDlg, %IDC_LOG         TO hLog
    CONTROL HANDLE g_hDlg, %IDC_BTN_START   TO hBtn1
    CONTROL HANDLE g_hDlg, %IDC_BTN_STOP    TO hBtn2
    CONTROL HANDLE g_hDlg, %IDC_BTN_STOPALL TO hBtn3
    CONTROL HANDLE g_hDlg, %IDC_BTN_DUMP    TO hBtn4
    CONTROL HANDLE g_hDlg, %IDC_COMBO_DEVICE TO hCombo
    CONTROL HANDLE g_hDlg, %IDC_LBL_OUTPUT   TO hLabel
    CONTROL HANDLE g_hDlg, %IDC_STATUSBAR TO hStatus

    ' SWP_NOCOPYBITS: prevents "ghost" artifacts (Windows BitBlts old pixels otherwise).
    dwFlags = %SWP_NOZORDER OR %SWP_NOACTIVATE OR %SWP_NOCOPYBITS

    ' DeferWindowPos: atomic repositioning prevents controls overlapping during resize.
    hDWP = BeginDeferWindowPos(9)
    IF hDWP THEN
        ' ListView: x=2, y=2
        hDWP = DeferWindowPos(hDWP, hLV, %NULL, 2, 2, cx - 4, lvH, dwFlags)

        ' Log: x=2, y=lvH+4
        y = lvH + 4
        hDWP = DeferWindowPos(hDWP, hLog, %NULL, 2, y, cx - 4, logH, dwFlags)

        ' Button row: y=lvH+logH+8
        '   Start(2,90) Stop(96,90) DUMP(190,90) "Output:"(286) ComboBox(328) StopAll(cx-92,90)
        y = lvH + logH + 8
        hDWP = DeferWindowPos(hDWP, hBtn1, %NULL, 2, y, 90, btnH, dwFlags)
        hDWP = DeferWindowPos(hDWP, hBtn2, %NULL, 96, y, 90, btnH, dwFlags)
        hDWP = DeferWindowPos(hDWP, hBtn4, %NULL, 190, y, 90, btnH, dwFlags)
        hDWP = DeferWindowPos(hDWP, hLabel, %NULL, 286, y + 6, 40, 20, dwFlags)
        ' ComboBox: x=328, width = cx-328-92-10 = cx-430 (stretch to Stop All)
        hDWP = DeferWindowPos(hDWP, hCombo, %NULL, 328, y + 3, cx - 430, 200, dwFlags)
        hDWP = DeferWindowPos(hDWP, hBtn3, %NULL, cx - 92, y, 90, btnH, dwFlags)

        ' StatusBar at bottom, full width, in DeferWindowPos batch (prevents BitBlt smearing).
        y = cy - statusH
        hDWP = DeferWindowPos(hDWP, hStatus, %NULL, 0, y, cx, statusH, dwFlags)

        EndDeferWindowPos hDWP
    END IF


END SUB

#ENDIF
