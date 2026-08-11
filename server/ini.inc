' ============================================================================
'  ini.inc — INI file persistence (device output, manual devices, add-device defaults)
' ============================================================================
#IF NOT %DEF(%INI_INC)
%INI_INC = 1

' SaveDeviceOutput - Save a single device's WaveOut ID to INI
SUB SaveDeviceOutput(BYVAL sMac AS STRING, BYVAL devId AS LONG)
    LOCAL sKey AS STRING
    ' Remove colons from MAC: "9C:9C:1F:8D:AA:C5" → "9C9C1F8DAAC5"
    sKey = REMOVE$(sMac, ":")
    IF LEN(g_sIniFile) = 0 THEN EXIT SUB
    WritePrivateProfileString "devices", BYCOPY sKey, BYCOPY TRIM$(STR$(devId)), BYCOPY g_sIniFile
END SUB

' LoadDeviceOutput - Load a device's WaveOut ID from INI (returns -1 if not found)
FUNCTION LoadDeviceOutput(BYVAL sMac AS STRING) AS LONG
    LOCAL sKey AS STRING
    sKey = REMOVE$(sMac, ":")
    IF LEN(g_sIniFile) = 0 THEN FUNCTION = -1 : EXIT FUNCTION
    FUNCTION = GetPrivateProfileInt("devices", BYCOPY sKey, -1, BYCOPY g_sIniFile)
END FUNCTION

' ManualDeviceAdd - Append an IP:port to the [manual_devices] section.
' Deduplicates by IP (if same IP already saved, updates its port).
SUB ManualDeviceAdd(BYVAL sIP AS STRING, BYVAL nPort AS LONG)
    IF LEN(g_sIniFile) = 0 THEN EXIT SUB
    IF LEN(sIP) = 0 THEN EXIT SUB

    LOCAL i AS LONG, n AS LONG
    LOCAL sEntry AS STRING, sExisting AS STRINGZ*256, sIPOnly AS STRING
    LOCAL bFound AS LONG

    n = GetPrivateProfileInt("manual_devices", "count", 0, BYCOPY g_sIniFile)
    IF n > 64 THEN n = 64
    bFound = 0
    ' Check for duplicate IP (update port if found)
    FOR i = 1 TO n
        sExisting = SPACE$(256)
        GetPrivateProfileString "manual_devices", BYCOPY TRIM$(STR$(i)), "", BYREF sExisting, 256, BYCOPY g_sIniFile
        sExisting = TRIM$(sExisting, ANY CHR$(0, 32))
        ' Extract IP part (before ":")
        LOCAL colon AS LONG
        colon = INSTR(sExisting, ":")
        IF colon > 0 THEN
            sIPOnly = LEFT$(sExisting, colon - 1)
        ELSE
            sIPOnly = sExisting
        END IF
        IF sIPOnly = sIP THEN
            ' Same IP — update this entry with new port
            sEntry = sIP & ":" & TRIM$(STR$(nPort))
            WritePrivateProfileString "manual_devices", BYCOPY TRIM$(STR$(i)), _
                                       BYCOPY sEntry, BYCOPY g_sIniFile
            bFound = 1
            EXIT FOR
        END IF
    NEXT i

    IF bFound = 0 THEN
        IF n >= 64 THEN
            ' no more slots
        ELSE
            ' New entry — append
            INCR n
            sEntry = sIP & ":" & TRIM$(STR$(nPort))
            WritePrivateProfileString "manual_devices", BYCOPY TRIM$(STR$(n)), _
                                       BYCOPY sEntry, BYCOPY g_sIniFile
            WritePrivateProfileString "manual_devices", "count", _
                                       BYCOPY TRIM$(STR$(n)), BYCOPY g_sIniFile
            AddLog "[INI] Saved manual device #" & TRIM$(STR$(n)) & ": " & sEntry
        END IF
    END IF
END SUB

' ManualDeviceLoadAll - Read all saved manual devices and send DISCOVER to each.
' Called on startup (WM_INITDIALOG) after discovery socket is open.
SUB ManualDeviceLoadAll()
    IF LEN(g_sIniFile) = 0 THEN EXIT SUB
    IF DiscAnyOpen() = 0 THEN EXIT SUB    ' no discovery sockets open

    LOCAL i AS LONG, n AS LONG
    LOCAL sEntry AS STRINGZ*256, sIP AS STRING, sPort AS STRING
    LOCAL colon AS LONG, nPort AS LONG, targetIP AS DWORD
    LOCAL sentCount AS LONG

    n = GetPrivateProfileInt("manual_devices", "count", 0, BYCOPY g_sIniFile)
    IF n > 64 THEN n = 64
    IF n = 0 THEN EXIT SUB

    AddLog "[INI] Loading " & TRIM$(STR$(n)) & " saved manual device(s) from INI..."

    FOR i = 1 TO n
        sEntry = SPACE$(256)
        GetPrivateProfileString "manual_devices", BYCOPY TRIM$(STR$(i)), _
                                 "", BYREF sEntry, 256, BYCOPY g_sIniFile
        sEntry = TRIM$(sEntry, ANY CHR$(0, 32))
        IF LEN(sEntry) = 0 THEN ITERATE FOR

        ' Parse "IP:port"
        colon = INSTR(sEntry, ":")
        IF colon > 0 THEN
            sIP = LEFT$(sEntry, colon - 1)
            sPort = MID$(sEntry, colon + 1)
        ELSE
            sIP = sEntry
            sPort = "3950"
        END IF

        ' Validate + send DISCOVER
        targetIP = inet_addr(BYCOPY sIP)
        IF targetIP <> %INADDR_NONE THEN
            nPort = VAL(sPort)
            IF nPort < 1 OR nPort > 65535 THEN nPort = 3950
            SendDiscover targetIP, nPort
            INCR sentCount
            AddLog "[INI]   #" & TRIM$(STR$(i)) & ": " & sIP & ":" & TRIM$(STR$(nPort)) & " - DISCOVER sent"
        END IF
    NEXT i

    IF sentCount > 0 THEN
        AddLog "[INI] Sent DISCOVER to " & TRIM$(STR$(sentCount)) & _
               " saved device(s). Waiting for INFO responses..."
    END IF
END SUB

' SaveAddDeviceDefaults - Save last IP/port entered in Add Device dialog.
SUB SaveAddDeviceDefaults(BYVAL sIP AS STRING, BYVAL sPort AS STRING)
    IF LEN(g_sIniFile) = 0 THEN EXIT SUB
    WritePrivateProfileString "add_device", "last_ip", BYCOPY sIP, BYCOPY g_sIniFile
    WritePrivateProfileString "add_device", "last_port", BYCOPY sPort, BYCOPY g_sIniFile
END SUB

' LoadAddDeviceDefaultIP - Returns last IP or default "10.1.30.46" if none saved.
FUNCTION LoadAddDeviceDefaultIP() AS STRING
    IF LEN(g_sIniFile) = 0 THEN FUNCTION = "10.1.30.46" : EXIT FUNCTION
    LOCAL s AS STRINGZ*64
    GetPrivateProfileString "add_device", "last_ip", "10.1.30.46",BYREF s, 64, BYCOPY g_sIniFile
    FUNCTION = TRIM$(s, ANY CHR$(0, 32))
END FUNCTION

' LoadAddDeviceDefaultPort - Returns last port or "3950" if none saved.
FUNCTION LoadAddDeviceDefaultPort() AS STRING
    IF LEN(g_sIniFile) = 0 THEN FUNCTION = "3950" : EXIT FUNCTION
    LOCAL s AS STRINGZ*16
    GetPrivateProfileString "add_device", "last_port", "3950", _
                             BYREF s, 16, BYCOPY g_sIniFile
    FUNCTION = TRIM$(s, ANY CHR$(0, 32))
END FUNCTION

#ENDIF
