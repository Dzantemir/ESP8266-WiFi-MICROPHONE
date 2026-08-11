' ============================================================================
'  EASSP Server - ESP8266 WiFi Microphone
'  PowerBASIC 10 for Windows
'  Protocol: EASSP over UDP port 3950
'  Audio: IMA ADPCM (DVI4/RFC3551) -> 16-bit PCM, sample rate from device
'  Playback: WaveOut API
'  GUI: DDT + ListView + StatusBar, resizable
' ============================================================================

#COMPILE EXE
#DIM ALL
#OPTION VERSION5
#REGISTER ALL
' ---- Resources: icon, manifest (Common Controls v6), version info ----
#RESOURCE ICON, 100, "eassp_server.ico"
#RESOURCE MANIFEST, 1, "eassp_server.manifest"
' Version info: FILEVERSION/PRODUCTVERSION are binary (4 x 16-bit).
' STRINGINFO "0419" (Russian) / "04E4" (Windows Multilingual).
#RESOURCE VERSIONINFO
#RESOURCE FILEFLAGS      0
#RESOURCE FILEVERSION    1, 0, 0, 0
#RESOURCE PRODUCTVERSION 2, 0, 0, 0
#RESOURCE STRINGINFO     "0419", "04E4"
#RESOURCE VERSION$ "Comments",         "ESP8266 WiFi Microphone Receiver"
#RESOURCE VERSION$ "CompanyName",      "EASSP"
#RESOURCE VERSION$ "FileDescription",  "EASSP Server - ESP8266 WiFi Microphone"
#RESOURCE VERSION$ "FileVersion",      "1.0.0.0"
#RESOURCE VERSION$ "InternalName",     "eassp_server"
#RESOURCE VERSION$ "LegalCopyright",   "Copyright (c) 2024 EASSP Project"
#RESOURCE VERSION$ "OriginalFilename", "eassp_server.exe"
#RESOURCE VERSION$ "ProductName",      "EASSP Server"
#RESOURCE VERSION$ "ProductVersion",   "2.0 (firmware v2.0)"

' ---- Win32 API includes ----
$INCLUDE "WIN32API.INC"
' %INADDR_NONE and %FD_READ (used by UDP NOTIFY lParam decoding per
' PBWin.txt). Win32Api.inc v10.01.0019 does NOT transitively include
' WinSock2.inc / ws2def.inc, and WinSock.inc (v1) is an empty stub
' ("[not translated at this time]"). WinSock2.inc itself includes
' ws2def.inc, so this single line is sufficient. Per WinSock2.inc:14,
' it MUST appear AFTER $INCLUDE "Win32API.INC".
$INCLUDE "WINSOCK2.INC"

' ---- Project includes ----
$INCLUDE "config.inc"
$INCLUDE "types.inc"

' ============================================================================
'  GLOBAL VARIABLES
' ============================================================================
'  GLOBAL VARIABLES (in globals.inc)
' ============================================================================
$INCLUDE "globals.inc"
$INCLUDE "util.inc"
$INCLUDE "discovery.inc"
$INCLUDE "net_cmd.inc"
$INCLUDE "ini.inc"
$INCLUDE "ui_listview.inc"
$INCLUDE "ui_layout.inc"
$INCLUDE "audio_codec.inc"
$INCLUDE "dump.inc"
$INCLUDE "stream.inc"
$INCLUDE "heartbeat.inc"
$INCLUDE "device.inc"
$INCLUDE "audio_thread.inc"
$INCLUDE "ui_refresh.inc"
$INCLUDE "ui_dialogs.inc"
$INCLUDE "ui_main.inc"


FUNCTION PBMAIN() AS LONG
    LOCAL lResult AS LONG
    LOCAL i AS LONG
    LOCAL icex AS INIT_COMMON_CONTROLSEX

    ' ---- Single instance check (BEFORE any dialog creation) ----
    ' CreateMutex is declared in WinBase.inc as:
    '   CreateMutex(lpMutexAttributes AS SECURITY_ATTRIBUTES, BYVAL bInitialOwner AS LONG, lpName AS ASCIIZ) AS DWORD
    ' First param is BYREF. We pass a properly initialized SECURITY_ATTRIBUTES
    ' struct so the handle is inheritable if needed. (Passing BYVAL 0 here is
    ' also valid in PB - it sends a NULL pointer = default security descriptor;
    ' the old comment claiming it   ' corrupts the stack' was incorrect.)
    LOCAL hMutex AS DWORD
    LOCAL sa AS SECURITY_ATTRIBUTES
    sa.nLength = SIZEOF(sa)
    sa.lpSecurityDescriptor = 0
    sa.bInheritHandle = 0
    LOCAL sMutexName AS ASCIIZ * 64
    sMutexName = "EASSP_Server_SingleInstance_Mutex"
    hMutex = CreateMutex(sa, 1, sMutexName)
    IF hMutex = 0 THEN
        MSGBOX "Failed to create mutex.", %MB_ICONERROR, "EASSP Server"
        EXIT FUNCTION
    END IF
    IF GetLastError() = %ERROR_ALREADY_EXISTS THEN
        ' Another instance is already running — bring it to front and exit.
        ' FindWindow is declared as: FindWindow(lpClassName AS ASCIIZ, lpWindowName AS ASCIIZ)
        ' Both params are BYREF ASCIIZ. Pass BYVAL 0 for lpClassName (NULL = any class),
        ' and an ASCIIZ string for the window title.
        LOCAL hExisting AS DWORD
        LOCAL sTitle AS ASCIIZ * 128
        sTitle = $APP_TITLE
        hExisting = FindWindow(BYVAL 0, sTitle)
        IF hExisting THEN
            IF IsIconic(hExisting) THEN
                ShowWindow hExisting, %SW_RESTORE
            END IF
            LOCAL dwCurThread AS DWORD, dwForeThread AS DWORD
            LOCAL hForeWnd AS DWORD
            hForeWnd = GetForegroundWindow()
            dwCurThread = GetCurrentThreadId()
            dwForeThread = GetWindowThreadProcessId(hForeWnd, BYVAL 0)
            IF dwCurThread <> dwForeThread THEN
                AttachThreadInput dwCurThread, dwForeThread, 1
                SetForegroundWindow hExisting
                AttachThreadInput dwCurThread, dwForeThread, 0
            ELSE
                SetForegroundWindow hExisting
            END IF
            IF GetForegroundWindow() <> hExisting THEN
                FlashWindow hExisting, 1
            END IF
        END IF
        CloseHandle hMutex
        EXIT FUNCTION
    END IF

    ' Allocate arrays
    REDIM g_Devs(%MAX_DEVICES - 1) AS GLOBAL DeviceInfo
    REDIM g_UiCache(0 TO %MAX_DEVICES - 1) AS GLOBAL DeviceUiCache
    REDIM g_StepTable(88) AS GLOBAL LONG
    REDIM g_IndexTable(7) AS GLOBAL LONG
    ' Each slot holds fNum/port/isOpen; isOpen=0 means unused.
    REDIM g_DiscPorts(0 TO %MAX_DISC_PORTS - 1) AS GLOBAL DiscPort
    REDIM g_saveColW(0 TO %LV_COL_DUR) AS GLOBAL LONG
    g_DiscPortCount = 0
    FOR i = 0 TO %MAX_DEVICES - 1
        g_Devs(i).dwDiscSlot = -1
    NEXT i

    ' Init common controls (ListView + StatusBar)
    icex.dwSize = SIZEOF(icex)
    icex.dwICC  = %ICC_LISTVIEW_CLASSES OR %ICC_BAR_CLASSES
    InitCommonControlsEx icex

    ' Init globals
    g_bRunning = 1
    g_bShuttingDown = 0
    g_hHeap = GetProcessHeap()
    InitializeCriticalSection g_csDev
    InitializeCriticalSection g_csDump   ' dump file race protection
    InitializeCriticalSection g_csFile   ' FREEFILE+OPEN serialization

    ' Build INI file path: same folder as .exe, filename "eassp_server.ini"
    ' EXE.PATH$ returns the .exe directory WITH trailing backslash (PowerBASIC built-in).
    g_sIniFile = EXE.PATH$ & "eassp_server.ini"

    ' Init IMA ADPCM Step Table
    InitStepTable

    ' ---- Create main dialog ----
    ' Load saved window position/size from INI (or use defaults).
    ' Compute EXACT x/y before DIALOG NEW — never pass -1, because
    ' DIALOG NEW with -1 creates the window at (0,0) first and then
    ' moves it, causing a visible flash at top-left corner.
    LOCAL dlgX AS LONG, dlgY AS LONG, dlgW AS LONG, dlgH AS LONG
    dlgX = GetPrivateProfileInt("window", "x", -1, BYCOPY g_sIniFile)
    dlgY = GetPrivateProfileInt("window", "y", -1, BYCOPY g_sIniFile)
    dlgW = GetPrivateProfileInt("window", "w", 750, BYCOPY g_sIniFile)
    dlgH = GetPrivateProfileInt("window", "h", 480, BYCOPY g_sIniFile)
    ' Clamp width/height to minimums (matching WM_GETMINMAXINFO)
    IF dlgW < 500 THEN dlgW = 750
    IF dlgH < 300 THEN dlgH = 480
    ' Clamp position to visible screen (avoid off-screen if monitor setup changed)
    LOCAL screenW AS LONG, screenH AS LONG
    screenW = GetSystemMetrics(%SM_CXSCREEN)
    screenH = GetSystemMetrics(%SM_CYSCREEN)
    ' If no saved position or off-screen: center on screen explicitly
    IF dlgX < 0 OR dlgX > screenW - 100 THEN
        dlgX = (screenW - dlgW) \ 2
    END IF
    IF dlgY < 0 OR dlgY > screenH - 100 THEN
        dlgY = (screenH - dlgH) \ 2
    END IF

    ' ---- Create main dialog (HIDDEN — no %WS_VISIBLE) ----
    ' DIALOG NEW in PowerBASIC shows the window immediately if %WS_VISIBLE
    ' is in the style. By omitting it, the window is created but not shown.
    ' We set the exact position NOW (computed above), so when DIALOG SHOW
    ' MODAL makes it visible, it appears directly at the right place —
    ' no flash at (0,0).
    ' 2.1-fix: DIALOG DEFAULT FONT must be set BEFORE DIALOG NEW
    DIALOG DEFAULT FONT "Tahoma", 9

    DIALOG NEW PIXELS, 0, $APP_TITLE, dlgX, dlgY, dlgW, dlgH, _
        %WS_OVERLAPPED OR %WS_CAPTION OR %WS_SYSMENU OR _
        %WS_MINIMIZEBOX OR %WS_THICKFRAME OR %WS_CLIPCHILDREN, _
        %WS_EX_CONTROLPARENT OR %WS_EX_APPWINDOW TO g_hDlg

    ' ---- ListView (Report mode) ----
    CONTROL ADD LISTVIEW, g_hDlg, %IDC_LISTVIEW, "", _
        2, 2, 746, 280, _
        %WS_CHILD OR %WS_VISIBLE OR %WS_TABSTOP OR _
        %LVS_REPORT OR %LVS_SINGLESEL OR %LVS_SHOWSELALWAYS, _
        %WS_EX_CLIENTEDGE

    ' ---- Buttons (initially disabled until checkboxes are used) ----
    CONTROL ADD BUTTON, g_hDlg, %IDC_BTN_START,   "Start Stream", 2, 290, 90, 26
    CONTROL ADD BUTTON, g_hDlg, %IDC_BTN_STOP,    "Stop Stream",  96, 290, 90, 26
    CONTROL ADD BUTTON, g_hDlg, %IDC_BTN_DUMP,    "DUMP",        190, 290, 90, 26
    CONTROL DISABLE g_hDlg, %IDC_BTN_START
    CONTROL DISABLE g_hDlg, %IDC_BTN_STOP
    CONTROL ENABLE  g_hDlg, %IDC_BTN_DUMP

    ' ---- WaveOut device selection ComboBox ----
    CONTROL ADD LABEL, g_hDlg, %IDC_LBL_OUTPUT, "Output:", 286, 293, 40, 12
    CONTROL ADD COMBOBOX, g_hDlg, %IDC_COMBO_DEVICE, , 328, 290, 320, 200, _
        %WS_CHILD OR %WS_VISIBLE OR %WS_TABSTOP OR _
        %CBS_DROPDOWNLIST OR %WS_VSCROLL, %WS_EX_CLIENTEDGE

    ' ---- Stop All button (right-anchored) ----
    CONTROL ADD BUTTON, g_hDlg, %IDC_BTN_STOPALL, "Stop All",    658, 290, 90, 26
    CONTROL DISABLE g_hDlg, %IDC_BTN_STOPALL

    ' ---- Log textbox ----
    CONTROL ADD TEXTBOX, g_hDlg, %IDC_LOG, "", _
        2, 320, 746, 110, _
        %WS_CHILD OR %WS_VISIBLE OR %WS_VSCROLL OR _
        %ES_MULTILINE OR %ES_READONLY OR %ES_AUTOVSCROLL, _
        %WS_EX_CLIENTEDGE

    ' Monospace font for log
    LOCAL hMono AS DWORD
    FONT NEW "Courier New", 9 TO hMono
    CONTROL SET FONT g_hDlg, %IDC_LOG, hMono

    ' ---- StatusBar (multi-part) ----
    CONTROL ADD STATUSBAR , g_hDlg, %IDC_STATUSBAR, "", _
        0, 0, 0, 0, _
        %WS_CHILD OR %WS_VISIBLE OR %SBARS_SIZEGRIP

    ' Define StatusBar parts (4 columns):
    '   [0] EASSP Server   [1] Devices: N   [2] Streaming: N   [3] UDP:ports / Output: device
    ' Parts are set up in ResizeControls (widths depend on window width).
    ' Initial dummy parts:
    STATUSBAR SET PARTS g_hDlg, %IDC_STATUSBAR,  100, 100, 100, 9999

    ' ---- Init ListView columns ----
    InitListView

    ' ---- Restore saved ListView column widths from INI ----
    ' LISTVIEW SET COLUMN (1-based columns: colIdx + 1).
    LOCAL colIdx AS LONG, colW AS LONG
    FOR colIdx = 0 TO %LV_COL_DUR
        colW = GetPrivateProfileInt("listview", "col" & TRIM$(STR$(colIdx)), _
                -1, BYCOPY g_sIniFile)
        IF colW > 0 THEN
            LISTVIEW SET COLUMN g_hDlg, %IDC_LISTVIEW, colIdx + 1, colW
        END IF
    NEXT colIdx

    ' ---- Populate WaveOut device ComboBox ----
    PopulateDeviceCombo

    ' System menu: About + Add Device
    LOCAL hSysMenu AS DWORD
    hSysMenu = GetSystemMenu(g_hDlg, 0)
        MENU ADD STRING, hSysMenu, "-", 0, 0
    MENU ADD STRING, hSysMenu, "Add Device...", %IDM_ADD_DEVICE, %MF_ENABLED
    MENU ADD STRING, hSysMenu, "Discovery Ports...", %IDM_ADD_PORT, %MF_ENABLED
    MENU ADD STRING, hSysMenu, "About...", %IDM_ABOUT, %MF_ENABLED

    ' ---- Init network () ----
    ' InitDiscovery now reads [discovery] ports=... from INI and opens
    ' each port via DiscOpenPort. Returns count of open ports (0 = fatal).
    IF InitDiscovery() = 0 THEN
        AddLog "Cannot bind ANY UDP discovery socket! Check [discovery] ports in INI."
    ELSE
        THREAD CREATE HeartbeatThread(0) TO g_hHbTh
        IF g_hHbTh = 0 THEN
            AddLog "FATAL: THREAD CREATE HeartbeatThread failed - discovery will not run"
        END IF
        ' (InitDiscovery already logged "EASSP Server started. Listening on N UDP port(s)")
        ' Load saved manual devices from INI and send DISCOVER to each.
        ' Devices that respond will appear in the ListView automatically.
        ' Small delay so the discovery socket is fully ready before we blast
        ' DISCOVERs (HeartbeatThread just started).
        SLEEP 200
        ManualDeviceLoadAll
    END IF

    ' RefreshUI before showing dialog (sets statusbar part widths / UDP port list).
    'g_uiDirty = 1
    'g_sbDirty = 1
    'RefreshUI

    ' ---- Show dialog (MODAL with callback) ----
    DIALOG SHOW MODELESS  g_hDlg, CALL MainDlgProc TO lResult

DO

  DIALOG DOEVENTS

LOOP WHILE ISWIN(g_hDlg)

    ' ---- Cleanup AFTER modal (window already destroyed) ----
    ' WM_CLOSE already did: HB wait + THREAD CLOSE + DiscCloseAll + snapshot.
    ' g_hHbTh is 0 by now. The blocks below are safety nets (idempotent).
    g_bShuttingDown = 1
    g_bRunning = 0

    LOCAL wr AS LONG, lr AS LONG, t0 AS DWORD

    ' 1) HB - normally already joined/closed in WM_CLOSE (g_hHbTh = 0).
    '    If WM_CLOSE's wait timed out (HB stuck), this is a second chance:
    '    wait again, then close ONLY if signaled.
    ' 3-fix: Don't zero g_hHbTh on timeout. If HB is still alive, DiscCloseAll
    ' would close sockets underneath it -> crash. Guard DiscCloseAll with hbDead.
    LOCAL hbDead AS LONG
    hbDead = 1   ' assume dead (no HB thread)
    IF g_hHbTh THEN
        hbDead = 0
        t0 = GetTickCount()
        DO
            wr = WaitForSingleObject(g_hHbTh, 50)
            IF wr = %WAIT_OBJECT_0 OR wr = %WAIT_FAILED THEN EXIT DO
            IF (GetTickCount() - t0) >= 2000 THEN EXIT DO
        LOOP
        IF wr = %WAIT_OBJECT_0 OR wr = %WAIT_FAILED THEN
            THREAD CLOSE g_hHbTh TO lr
            g_hHbTh = 0
            hbDead = 1
        END IF
        ' If timeout: keep g_hHbTh, hbDead=0 - don't DiscCloseAll
    END IF

    ' 2) Audio threads (if any survived)
    FOR i = 0 TO %MAX_DEVICES - 1
        IF g_Devs(i).hAudioThread THEN
            ' 1.7-fix: 500ms too short for TCP reconnect (2s timeout). Use 5000ms.
            wr = WaitForSingleObject(g_Devs(i).hAudioThread, 5000)
            IF wr = %WAIT_OBJECT_0 OR wr = %WAIT_FAILED THEN
                THREAD CLOSE g_Devs(i).hAudioThread TO lr
            END IF
            g_Devs(i).hAudioThread = 0
        END IF
    NEXT i

    ' 3) Dump
    EnterCriticalSection g_csDump
    g_bDumping = 0
    IF g_hDumpFile THEN
        UpdateWavHeader
        CLOSE g_hDumpFile
        g_hDumpFile = 0
    END IF
    LeaveCriticalSection g_csDump

    ' 4) Discovery sockets - only if HB is dead (else HB may UDP SEND on closed socket).
    '    If HB is still alive (timeout), leak sockets - OS reclaims on process exit.
    IF hbDead THEN DiscCloseAll

    ' 5) INI save (after window destroyed — no GUI hang)
    IF g_saveWndRect.nRight > g_saveWndRect.nLeft THEN
        WritePrivateProfileString "window", "x", BYCOPY TRIM$(STR$(g_saveWndRect.nLeft)), BYCOPY g_sIniFile
        WritePrivateProfileString "window", "y", BYCOPY TRIM$(STR$(g_saveWndRect.nTop)), BYCOPY g_sIniFile
        WritePrivateProfileString "window", "w", BYCOPY TRIM$(STR$(g_saveWndRect.nRight - g_saveWndRect.nLeft)), BYCOPY g_sIniFile
        WritePrivateProfileString "window", "h", BYCOPY TRIM$(STR$(g_saveWndRect.nBottom - g_saveWndRect.nTop)), BYCOPY g_sIniFile
    END IF
    FOR i = 0 TO %LV_COL_DUR
        IF g_saveColW(i) > 0 THEN
            WritePrivateProfileString "listview", "col" & TRIM$(STR$(i)), _
                BYCOPY TRIM$(STR$(g_saveColW(i))), BYCOPY g_sIniFile
        END IF
    NEXT i

    IF hMono THEN FONT END hMono
    IF hMutex THEN CloseHandle hMutex
    DeleteCriticalSection g_csDev
    DeleteCriticalSection g_csDump
    DeleteCriticalSection g_csFile

    FUNCTION = lResult
END FUNCTION
