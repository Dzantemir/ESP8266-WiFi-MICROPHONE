' ============================================================================
'  ui_listview.inc — ListView helpers (InitListView, checkboxes, selection, buttons)
' ============================================================================
#IF NOT %DEF(%UI_LISTVIEW_INC)
%UI_LISTVIEW_INC = 1

' PopulateDeviceCombo - Enumerate WaveOut devices and fill the ComboBox.
' Adds "Default (WAVE_MAPPER)" as first entry, then all physical/virtual
' devices (speakers, VB-Cable, etc.). User selects which device receives
' the audio — select "CABLE Input" to route to a virtual microphone.
SUB PopulateDeviceCombo()
    LOCAL numDevs AS LONG
    LOCAL i AS LONG
    LOCAL caps AS WAVEOUTCAPS
    LOCAL sName AS STRING
    LOCAL mmRes AS LONG

    ' Start with "Default" (WAVE_MAPPER = -1)
    COMBOBOX ADD g_hDlg, %IDC_COMBO_DEVICE, "Default (WAVE_MAPPER)"

    ' Enumerate all WaveOut devices
    numDevs = waveOutGetNumDevs()
    FOR i = 0 TO numDevs - 1
        mmRes = waveOutGetDevCaps(i, caps, SIZEOF(caps))
        IF mmRes = 0 THEN
            sName = EXTRACT$(caps.szPname, CHR$(0))
            IF LEN(sName) = 0 THEN sName = "Device " & TRIM$(STR$(i))
            COMBOBOX ADD g_hDlg, %IDC_COMBO_DEVICE, _
                TRIM$(STR$(i)) & ": " & sName
        END IF
    NEXT i

    ' Select "Default" (index 0 in the combo = WAVE_MAPPER)
    COMBOBOX SELECT g_hDlg, %IDC_COMBO_DEVICE, 1
END SUB

' InitListView - Create columns in the ListView
SUB InitListView()
    ' Columns are 1-based in DDT. %LV_COL_* equates are 0-based, so add +1.
    ' format& : 0=left, 1=right, 2=center.

    ' Col 1 - Hostname (MAC)
    LISTVIEW INSERT COLUMN g_hDlg, %IDC_LISTVIEW, %LV_COL_MAC + 1, "Name (MAC)", 200, 0

    ' Col 2 - IP:Port
    LISTVIEW INSERT COLUMN g_hDlg, %IDC_LISTVIEW, %LV_COL_IP + 1, "IP:Port", 135, 0

    ' Col 3 - Status
    LISTVIEW INSERT COLUMN g_hDlg, %IDC_LISTVIEW, %LV_COL_STATUS + 1, "Status", 75, 0

    ' Col 4 - Sample Rate
    LISTVIEW INSERT COLUMN g_hDlg, %IDC_LISTVIEW, %LV_COL_RATE + 1, "Rate", 55, 0

    ' Col 5 - Bits
    LISTVIEW INSERT COLUMN g_hDlg, %IDC_LISTVIEW, %LV_COL_BITS + 1, "Bits", 40, 0

    ' Col 6 - Channels
    LISTVIEW INSERT COLUMN g_hDlg, %IDC_LISTVIEW, %LV_COL_CH + 1, "Ch", 35, 0

    ' Col 7 - Codec
    LISTVIEW INSERT COLUMN g_hDlg, %IDC_LISTVIEW, %LV_COL_CODEC + 1, "Codec", 60, 0

    ' Col 8 - RSSI
    LISTVIEW INSERT COLUMN g_hDlg, %IDC_LISTVIEW, %LV_COL_RSSI + 1, "RSSI", 60, 0

    ' Col 9 - Heap
    LISTVIEW INSERT COLUMN g_hDlg, %IDC_LISTVIEW, %LV_COL_HEAP + 1, "Heap", 60, 0

    ' Col 10 - Firmware
    LISTVIEW INSERT COLUMN g_hDlg, %IDC_LISTVIEW, %LV_COL_FW + 1, "FW", 55, 0

    ' Col 11 - Packets
    LISTVIEW INSERT COLUMN g_hDlg, %IDC_LISTVIEW, %LV_COL_PKTS + 1, "Pkts", 60, 0

    ' Col 12 - Lost
    LISTVIEW INSERT COLUMN g_hDlg, %IDC_LISTVIEW, %LV_COL_LOST + 1, "Lost", 50, 0

    ' Col 13 - Duration
    LISTVIEW INSERT COLUMN g_hDlg, %IDC_LISTVIEW, %LV_COL_DUR + 1, "Duration", 70, 0

    ' Extended styles: full row select + grid lines + checkboxes + double buffer
    ' LVS_EX_DOUBLEBUFFER prevents flicker during rapid updates (ComCtl32 v6+)
    LISTVIEW SET STYLEXX g_hDlg, %IDC_LISTVIEW, _
        %LVS_EX_FULLROWSELECT OR %LVS_EX_GRIDLINES OR %LVS_EX_CHECKBOXES OR %LVS_EX_DOUBLEBUFFER
END SUB

' GetSelectedDevIdx - Get device array index for the focused ListView item.
FUNCTION GetSelectedDevIdx() AS LONG
    LOCAL lvIdx AS LONG
    LOCAL lParam AS LONG

    LISTVIEW GET SELECT g_hDlg, %IDC_LISTVIEW TO lvIdx
    IF lvIdx = 0 THEN
        FUNCTION = -1
        EXIT FUNCTION
    END IF

    LISTVIEW GET USER g_hDlg, %IDC_LISTVIEW, lvIdx TO lParam
    IF lParam >= 0 AND lParam < %MAX_DEVICES THEN
        FUNCTION = lParam
    ELSE
        FUNCTION = -1
    END IF
END FUNCTION

' IsItemChecked - Check if ListView item checkbox is checked
FUNCTION IsItemChecked(BYVAL hLV AS DWORD, BYVAL iItem AS LONG) AS LONG
    FUNCTION = ListView_GetCheckState(hLV, iItem)
END FUNCTION

' SetItemChecked - Set ListView item checkbox state
' bCheck: 1 = checked (state image 2), 0 = unchecked (state image 1)
SUB SetItemChecked(BYVAL hLV AS DWORD, BYVAL iItem AS LONG, BYVAL bCheck AS LONG)
    ListView_SetCheckState hLV, iItem, bCheck
END SUB

' ===========================================================================
' SetDeviceWaveOutput - CS-safe write to g_Devs(devIdx).dwWaveDevice.
' Snapshots sMac/sHostname under CS for log + SaveDeviceOutput outside CS.
' ===========================================================================
SUB SetDeviceWaveOutput(BYVAL devIdx AS LONG, BYVAL newDev AS LONG)
    LOCAL saveMac AS STRING
    LOCAL saveHost AS STRING
    LOCAL sDev AS STRING

    EnterCriticalSection g_csDev
    g_Devs(devIdx).dwWaveDevice = newDev
    saveMac  = g_Devs(devIdx).sMac
    saveHost = g_Devs(devIdx).sHostname
    LeaveCriticalSection g_csDev

    SaveDeviceOutput saveMac, newDev

    IF newDev = -1 THEN
        sDev = "Default (WAVE_MAPPER) (will apply on next stream start)"
    ELSE
        sDev = TRIM$(STR$(newDev)) & " (will apply on next stream start)"
    END IF
    AddLog "Output device for " & TRIM$(saveHost) & ": " & sDev
END SUB

' UpdateButtonStates - Enable/disable buttons based on checkbox & stream state
' Snapshots g_Devs under CS, then does ListView reads + button updates outside CS.
SUB UpdateButtonStates()
    LOCAL hLV AS DWORD
    LOCAL i AS LONG
    LOCAL nCount AS LONG
    LOCAL bCanStart AS LONG
    LOCAL bCanStop AS LONG
    LOCAL bHasStream AS LONG
    LOCAL devIdx AS LONG
    DIM snap(%MAX_DEVICES - 1) AS LOCAL DeviceInfo

    CONTROL HANDLE g_hDlg, %IDC_LISTVIEW TO hLV
    IF hLV = 0 THEN EXIT SUB

    ' LISTVIEW GET COUNT / GET USER are 1-based; IsItemChecked uses 0-based index.
    LISTVIEW GET COUNT g_hDlg, %IDC_LISTVIEW TO nCount

    EnterCriticalSection g_csDev
    FOR i = 0 TO %MAX_DEVICES - 1
        snap(i) = g_Devs(i)
    NEXT i
    LeaveCriticalSection g_csDev

    ' All ListView reads + button state computation now CS-free
    FOR i = 0 TO nCount - 1
        LISTVIEW GET USER g_hDlg, %IDC_LISTVIEW, i + 1 TO devIdx

        IF devIdx < 0 OR devIdx >= %MAX_DEVICES THEN ITERATE FOR

        IF IsItemChecked(hLV, i) THEN
            IF snap(devIdx).dwActive AND snap(devIdx).dwHBActive = 0 THEN bCanStart = 1
            IF snap(devIdx).dwHBActive THEN bCanStop = 1
        END IF

        IF snap(devIdx).dwHBActive THEN bHasStream = 1
    NEXT i

    IF bCanStart  THEN CONTROL ENABLE  g_hDlg, %IDC_BTN_START   ELSE CONTROL DISABLE g_hDlg, %IDC_BTN_START
    IF bCanStop   THEN CONTROL ENABLE  g_hDlg, %IDC_BTN_STOP    ELSE CONTROL DISABLE g_hDlg, %IDC_BTN_STOP
    IF bHasStream THEN CONTROL ENABLE  g_hDlg, %IDC_BTN_STOPALL ELSE CONTROL DISABLE g_hDlg, %IDC_BTN_STOPALL
END SUB

#ENDIF
