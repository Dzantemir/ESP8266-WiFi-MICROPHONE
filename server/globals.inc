' ============================================================================
'  globals.inc — All global variables (included after types.inc)
' ============================================================================
#IF NOT %DEF(%EASSP_GLOBALS_INC)
%EASSP_GLOBALS_INC = 1

GLOBAL g_hDlg          AS DWORD
GLOBAL g_hHeap         AS DWORD
GLOBAL g_DiscPorts()   AS DiscPort
GLOBAL g_DiscPortCount AS LONG
GLOBAL g_hHbTh         AS DWORD
GLOBAL g_bRunning      AS LONG
GLOBAL g_bShuttingDown AS LONG   ' 1 = shutdown in progress: AddLog becomes no-op
GLOBAL g_seqCnt        AS LONG
GLOBAL g_bVerbose      AS LONG

' UI dirty flags
GLOBAL g_uiDirty       AS LONG
GLOBAL g_sbDirty       AS LONG

GLOBAL g_sIniFile      AS STRING
GLOBAL g_csDev         AS CRITICAL_SECTION
GLOBAL g_Devs()        AS DeviceInfo
GLOBAL g_UiCache()     AS DeviceUiCache
GLOBAL g_tmpPort       AS LONG

' Dump mode
GLOBAL g_bDumping      AS LONG
GLOBAL g_hDumpFile     AS LONG
GLOBAL g_dumpCodec     AS LONG
GLOBAL g_dumpIp        AS DWORD
GLOBAL g_csDump        AS CRITICAL_SECTION
GLOBAL g_csFile        AS CRITICAL_SECTION
GLOBAL g_dumpWavReady  AS LONG
GLOBAL g_dumpSampleRate AS DWORD
GLOBAL g_dumpChannels  AS LONG
GLOBAL g_dumpBits      AS LONG
GLOBAL g_dumpDataSize  AS QUAD
GLOBAL g_dumpFileIdx   AS LONG
GLOBAL g_dumpBaseName  AS STRING
GLOBAL g_dumpBtnDirty  AS LONG   ' 1.4: set by AudioThread on dump-open-fail; RefreshUI resets button

GLOBAL g_StepTable()   AS LONG
GLOBAL g_IndexTable()  AS LONG

GLOBAL g_saveWndRect  AS RECT
GLOBAL g_saveColW()   AS LONG

#ENDIF
