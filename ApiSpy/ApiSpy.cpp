/********************************************************************
Module : ApiSpy.cpp - part of ApiSpy implementation 
             Written 2003 by Dmitri Leman
             for an article about CE Api Spy.
Purpose: Contains CE Api Spy GUI implementation

  This file was compiled using eMbedded Visual C++ 3.0 
  with Pocket PC 2002 SDK and 4.0 with Standard SDK.
********************************************************************/

#include <windows.h>
#include <Commdlg.h>
#ifdef WIN32_PLATFORM_PSPC
#include <Aygshell.h>
#pragma comment (lib,"aygshell.lib")
#else
#include <Commctrl.h>
#endif

#include "Resource.h"
#include "../common/SpyEngine.h"
#include "../common/HTrace.h"
#include "../common/SpyControl.h"
#include "../common/SysDecls.h"

#define TRACE_SIZE 0x100000

static const TCHAR s_szTitle[] = _T("ApiSpy");
static const TCHAR s_szClass[] = _T("ApiSpyClass");
static const TCHAR s_szTraceViewClass[] = _T("ApiSpyTraceClass");


typedef BOOL T_StartSpy();
typedef BOOL T_StopSpy();
typedef void T_DumpApis();

T_StartSpy * g_pStartSpy = NULL;
T_StopSpy * g_pStopSpy = NULL;
T_DumpApis * g_pDumpApis = NULL;

HINSTANCE g_hInstExe = NULL, g_hInstSpyDll = NULL;
HWND     g_hWndMain = NULL, g_hWndTrace = NULL, g_hWndMenuBar = NULL;
int g_iOptionsMenuPos = 4;
HMENU g_hOptionsMenu = NULL;
DWORD g_dwCheckedGroups = 0;

/*-------------------------------------------------------------------
   FUNCTION: LoadSpyDLL
   PURPOSE: Load Spy DLL
   RETURNS:
    TRUE on success, FALSE on failure
-------------------------------------------------------------------*/
BOOL LoadSpyDLL()
{
    if(g_hInstSpyDll)
        return TRUE;
    g_hInstSpyDll = LoadLibrary(s_szApiSpyDll);
    if(!g_hInstSpyDll)
    {
        HTRACE(TG_MessageBox, 
            _T("ERROR: failed to load SpyDLL %s. Err %d"), 
            s_szApiSpyDll, GetLastError());
        return FALSE;
    }
    g_pStartSpy = (T_StartSpy*)GetProcAddress
        (g_hInstSpyDll, _T("StartSpy"));
    g_pStopSpy = (T_StopSpy*)GetProcAddress
        (g_hInstSpyDll, _T("StopSpy"));
    g_pDumpApis = (T_DumpApis*)GetProcAddress
        (g_hInstSpyDll, _T("DumpApis"));
    if(!g_pStartSpy || !g_pStopSpy || !g_pDumpApis)
    {
        HTRACE(TG_MessageBox, 
            _T("ERROR: failed to get SpyDLL routines"));
        return FALSE;
    }
    return TRUE;
}//BOOL LoadSpyDLL()

/*-------------------------------------------------------------------
   FUNCTION: StartSpy
   PURPOSE: Load Spy DLL and call StartSpy to start monitoring
   RETURNS:
    TRUE on success, FALSE on failure
-------------------------------------------------------------------*/
BOOL StartSpy()
{
    if(!g_hInstSpyDll)
    {
        LoadSpyDLL();
    }
    if(!g_pStartSpy)
    {
        return FALSE;
    }
    return g_pStartSpy();
}

/*-------------------------------------------------------------------
   FUNCTION: StopSpy
   PURPOSE: call StopSpy to stop monitoring and unload Spy DLL
   RETURNS:
    TRUE on success, FALSE on failure
-------------------------------------------------------------------*/
BOOL StopSpy()
{
    if(!g_hInstSpyDll)
    {
        return TRUE;//If DLL was not loaded, don't need to stop
    }
    if(!g_pStopSpy)
    {
        return FALSE;
    }
    if(!g_pStopSpy())
    {
        return FALSE;
    }
    if(g_hInstSpyDll)
    {
        FreeLibrary(g_hInstSpyDll);
        g_hInstSpyDll = NULL;
    }
    return TRUE;
}

/*-------------------------------------------------------------------
   FUNCTION: DumpApis
   PURPOSE: Load Spy DLL and call DumpApis to dump all system APIs
-------------------------------------------------------------------*/
void DumpApis()
{
    if(!g_hInstSpyDll)
    {
        LoadSpyDLL();
    }
    if(!g_pDumpApis)
    {
        return;
    }
    g_pDumpApis();
}

/*-------------------------------------------------------------------
   FUNCTION: OnSave
   PURPOSE: Display Save File dialog and save trace buffer to a file
-------------------------------------------------------------------*/
void OnSave(HWND p_hWndOwner)
{
    OPENFILENAME l_Open;
    TCHAR l_szBuffer[_MAX_PATH];
    *l_szBuffer = 0;
    memset(&l_Open, 0, sizeof(l_Open));
    l_Open.lStructSize = sizeof(l_Open);
    l_Open.hwndOwner = p_hWndOwner;
    l_Open.lpstrFilter = _T("Text files\0*.TXT\0All files\0*.*\0");
    l_Open.lpstrFile = l_szBuffer;
    l_Open.nMaxFile = _MAX_PATH;
    l_Open.lpstrTitle = _T("Save trace buffer");
    if(!GetSaveFileName(&l_Open))
        return;
    TraceDumpBufferToFile(l_szBuffer);
}

/*-------------------------------------------------------------------
   FUNCTION: OnCreateWindow
   PURPOSE: Handle WM_CREATE message. Create menu bar and trace
    view window.
   RETURNS:
    TRUE on success, FALSE on failure
-------------------------------------------------------------------*/
BOOL OnCreateWindow
(
    HWND p_hWnd, 
    UINT p_uiMsg, 
    WPARAM p_wParam, 
    LPARAM p_lParam
)
{
    g_hWndMain = p_hWnd;
    g_hWndTrace = CreateWindowEx(0, s_szTraceViewClass, NULL, 
        WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, 
        g_hWndMain, NULL, NULL, NULL);
#ifdef WIN32_PLATFORM_PSPC
	// Create a Done button and size it.
	SHINITDLGINFO l_InitInfo;

	l_InitInfo.dwMask = SHIDIM_FLAGS;
	l_InitInfo.dwFlags = SHIDIF_DONEBUTTON | SHIDIF_SIZEDLG;
	l_InitInfo.hDlg = p_hWnd;
	SHInitDialog(&l_InitInfo);

	SHMENUBARINFO l_MenuBarInfo;
	memset(&l_MenuBarInfo, 0, sizeof(SHMENUBARINFO));
	l_MenuBarInfo.cbSize = sizeof(l_MenuBarInfo);
	l_MenuBarInfo.hwndParent = p_hWnd;
	l_MenuBarInfo.dwFlags = 0; 
	l_MenuBarInfo.nToolBarId = IDR_MENUBAR; 
	l_MenuBarInfo.hInstRes = g_hInstExe;
	if(!SHCreateMenuBar(&l_MenuBarInfo))
	{
		HTRACE(TG_MessageBox,
            _T("Warning: SHCreateMenuBar failed. Error = %d\r\n"),
			GetLastError());
	}
    g_hWndMenuBar = l_MenuBarInfo.hwndMB;
    HMENU l_hMenu = (HMENU)
        SendMessage(g_hWndMenuBar, SHCMBM_GETSUBMENU, 0,IDC_OPTIONS);
#else
    g_hWndMenuBar = CommandBar_Create(g_hInstExe, p_hWnd, 1);
    if(!g_hWndMenuBar)
    {
		HTRACE(TG_MessageBox,
            _T("Warning: CommandBar_Create failed. Error = %d\r\n"),
			GetLastError());
        return FALSE;
    }
    if(!CommandBar_InsertMenubar(g_hWndMenuBar, 
        g_hInstExe, IDR_MENUBAR, 0))
    {
		HTRACE(TG_MessageBox,
            _T("Warning: CommandBar_InsertMenubar failed. ")
            _T("Error = %d\r\n"),
			GetLastError());
    }
    CommandBar_AddAdornments(g_hWndMenuBar, 0, 0);
    CommandBar_Show(g_hWndMenuBar, TRUE);

    g_hOptionsMenu = CommandBar_GetMenu(g_hWndMenuBar, 0);
    g_hOptionsMenu = GetSubMenu(g_hOptionsMenu, g_iOptionsMenuPos);
    
    DWORD l_dwStyle = GetWindowLong(p_hWnd, GWL_STYLE);
    SetWindowLong(p_hWnd, GWL_STYLE, l_dwStyle & ~WS_CAPTION);
    ShowWindow(p_hWnd, SW_SHOWMAXIMIZED);
    HMENU l_hMenu = g_hOptionsMenu;
#endif
    DWORD l_dwGroupsDebug = 
        TraceGetAssignedGroupsToStream(TO_DebugMonitor);
    DWORD l_dwGroupsMem = 
        TraceGetAssignedGroupsToStream(TO_MemoryBuffer);
    g_dwCheckedGroups = (l_dwGroupsDebug | l_dwGroupsMem) &
        (TG_InterceptedInfo | TG_DebugSpyBrief | 
        TG_DebugSpyDetailed);
    CheckMenuItem(l_hMenu, IDC_TRACE_INTERCEPTED, MF_BYCOMMAND | 
        (g_dwCheckedGroups & TG_InterceptedInfo)? 
        MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(l_hMenu, IDC_TRACE_SPY_BRIEF, MF_BYCOMMAND | 
        (g_dwCheckedGroups & TG_DebugSpyBrief)? 
        MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(l_hMenu, IDC_TRACE_SPY_DETAILED, MF_BYCOMMAND | 
        (g_dwCheckedGroups & TG_DebugSpyDetailed)? 
        MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(l_hMenu, IDC_OUTPUT_MEM, MF_BYCOMMAND | 
        (l_dwGroupsMem & TG_InterceptedInfo)? 
        MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(l_hMenu, IDC_OUTPUT_DEBUG, MF_BYCOMMAND | 
        (l_dwGroupsDebug & TG_InterceptedInfo)? 
        MF_CHECKED : MF_UNCHECKED);
    return TRUE;
}//BOOL OnCreateWindow

/*-------------------------------------------------------------------
   FUNCTION: OnSize
   PURPOSE: Handle WM_SIZE message. Resize menu bar and trace view
-------------------------------------------------------------------*/
void OnSize
(
    int p_iWidth,
    int p_iHeight
)
{
#ifdef WIN32_PLATFORM_PSPC
    int l_iMenuHeight = 0;
#else
    if(!g_hWndMenuBar)
        return;
    int l_iMenuHeight = CommandBar_Height(g_hWndMenuBar);
    SetWindowPos(g_hWndMenuBar, NULL, 0, 0, 
        p_iWidth, l_iMenuHeight, SWP_NOZORDER);
#endif
    SetWindowPos(g_hWndTrace, NULL, 0, l_iMenuHeight, 
        p_iWidth, p_iHeight - l_iMenuHeight, SWP_NOZORDER);
}

/*-------------------------------------------------------------------
   FUNCTION: CheckTraceOption
   PURPOSE: Handle check boxes in the Options menu to enable/disable
    different trace groups and outputs.
   PARAMETERS:
    int     p_iID - menu item ID
    BOOL    p_bChangeGroups - TRUE to change groups, FALSE - outputs
    DWORD   p_dwTraceOutputs - which outputs to change
    DWORD   p_dwTraceGroups  - which trace groups to toggle
-------------------------------------------------------------------*/
void CheckTraceOption
(
    int     p_iID, 
    BOOL    p_bChangeGroups,
    DWORD   p_dwTraceOutputs, 
    DWORD   p_dwTraceGroups
)
{
#ifdef WIN32_PLATFORM_PSPC
    HMENU l_hMenu = (HMENU)
      SendMessage(g_hWndMenuBar, SHCMBM_GETSUBMENU, 0, IDC_OPTIONS);
#else
    HMENU l_hMenu = g_hOptionsMenu;
#endif
    DWORD l_dwState = CheckMenuItem(l_hMenu, p_iID, MF_BYCOMMAND);
    if(l_dwState & MF_CHECKED)
    {
        CheckMenuItem(l_hMenu, p_iID, MF_BYCOMMAND | MF_UNCHECKED);
        TraceAssignGroupsToStream(p_dwTraceOutputs,
            0, p_dwTraceGroups);//Clear groups
        if(p_bChangeGroups)
        {
            g_dwCheckedGroups &= ~p_dwTraceGroups;
        }
    }
    else
    {
        CheckMenuItem(l_hMenu, p_iID, MF_BYCOMMAND | MF_CHECKED);
        TraceAssignGroupsToStream(p_dwTraceOutputs,
            p_dwTraceGroups, p_dwTraceGroups);//set groups
        if(p_bChangeGroups)
        {
            g_dwCheckedGroups |= p_dwTraceGroups;
        }
    }
    //Save changes
    TraceReadWriteSettings(HKEY_CURRENT_USER, s_szSettingsRegPath,
        true, p_dwTraceOutputs);
}

/*-------------------------------------------------------------------
   FUNCTION: OnCommand
   PURPOSE: Handle WM_COMMAND message.
-------------------------------------------------------------------*/
void OnCommand
(
    HWND p_hWnd, 
    UINT p_uiMsg, 
    WPARAM p_wParam, 
    LPARAM p_lParam
)
{
	WORD l_wNotifyCode = HIWORD(p_wParam);
	WORD l_wID = LOWORD(p_wParam);

    switch(l_wID)
    {
    case IDOK:
    case IDCANCEL:
        if(!StopSpy())
        {
            HTRACE(TG_MessageBox,
                _T("Cannot exit because hook(s) are still in use.")
                _T("Try terminating running applications to ")
                _T("release hooks."));
            return;
        }
		DestroyWindow(p_hWnd);
        break;
    case IDC_START:
        StopSpy();
        StartSpy();
        break;
	case IDC_OPTIONS_EXIT:
		StopSpy();
		PostQuitMessage(1);
		break;
    case IDC_STOP:
        StopSpy();
        break;
    case IDC_DUMP_APIS:
        DumpApis();
        break;
    case IDC_SAVE:
        OnSave(p_hWnd);
        break;
    case IDC_TRACE_INTERCEPTED:
        CheckTraceOption(IDC_TRACE_INTERCEPTED, TRUE,
            TO_DebugMonitor | TO_MemoryBuffer, TG_InterceptedInfo);
        break;
    case IDC_TRACE_SPY_BRIEF:
        CheckTraceOption(IDC_TRACE_SPY_BRIEF, TRUE,
            TO_DebugMonitor | TO_MemoryBuffer, TG_DebugSpyBrief);
        break;
    case IDC_TRACE_SPY_DETAILED:
        CheckTraceOption(IDC_TRACE_SPY_DETAILED, TRUE,
            TO_DebugMonitor | TO_MemoryBuffer, TG_DebugSpyDetailed);
        break;
    case IDC_OUTPUT_DEBUG:
        CheckTraceOption(IDC_OUTPUT_DEBUG, FALSE,
            TO_DebugMonitor, g_dwCheckedGroups);
        break;
	}
}//void OnCommand

/*-------------------------------------------------------------------
   FUNCTION: WindowProc
   PURPOSE: Window procedure for the main GUI window
-------------------------------------------------------------------*/
LRESULT CALLBACK WindowProc
(
    HWND p_hWnd, 
    UINT p_uiMsg, 
    WPARAM p_wParam, 
    LPARAM p_lParam
)
{
	switch(p_uiMsg)
	{
		case WM_CREATE:
            OnCreateWindow(p_hWnd, p_uiMsg, p_wParam, p_lParam);
            break;
        case WM_SIZE:
            OnSize(LOWORD(p_lParam), HIWORD(p_lParam));
            break;
		case WM_COMMAND:
            OnCommand(p_hWnd, p_uiMsg, p_wParam, p_lParam);
            break;
        case WM_CLOSE:
            DestroyWindow(p_hWnd);
            break;
		case WM_DESTROY:
            PostQuitMessage(1);
			break;
	}
	return DefWindowProc(p_hWnd, p_uiMsg, p_wParam, p_lParam);
}

/*-------------------------------------------------------------------
   FUNCTION: WinMain
   PURPOSE: Application entry point
-------------------------------------------------------------------*/
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, 
                   LPWSTR lpCmdLine, int nShowCmd )
{
    HWND l_hWnd = FindWindow(s_szClass, NULL);
    if(l_hWnd)
    {
        SetForegroundWindow(l_hWnd);
        return 0;
    }
    TraceInitialize();

    //Show errors and warnings on the
    //debugger's log window and in message boxes.
    TraceAssignGroupsToStream(TO_DebugMonitor | TO_MessageBox,
        TG_MessageBox | TG_Error, (DWORD)-1);

    //Print important messages to the debug monitor
    TraceAssignGroupsToStream(TO_DebugMonitor,
        TG_PrintAlways, TG_PrintAlways);

    //Allocate a memory buffer and print intercepted trace to it
    TraceUseMemMapFileBuffer(s_szTraceMapFile, TRACE_SIZE, 
        TG_MessageBox | TG_Error |
        TG_InterceptedInfo | TG_PrintAlways, (DWORD)-1);

    //After we assigned default values, read current settings from 
    //registry
    TraceReadWriteSettings(HKEY_CURRENT_USER, s_szSettingsRegPath, 
        false, TO_DebugMonitor | TO_MemoryBuffer);
    
    g_hInstExe = hInstance;

    //Register trace viewer window
    RegisterTraceView(s_szTraceViewClass);

    WNDCLASS l_Class;
    memset(&l_Class, 0, sizeof(l_Class));
    l_Class.lpfnWndProc = WindowProc;
    l_Class.lpszClassName = s_szClass;
    RegisterClass(&l_Class);

    CreateWindowEx(0, s_szClass, s_szTitle, WS_VISIBLE, 
        CW_DEFAULT, CW_DEFAULT, CW_DEFAULT, CW_DEFAULT, 
        NULL, NULL, NULL, NULL);

    MSG l_Msg;
    while(GetMessage(&l_Msg, NULL, 0, 0))
    {
        TranslateMessage(&l_Msg);
        DispatchMessage(&l_Msg);
    }
    
    StopSpy();

    HTRACE(TG_DebugSpyBrief, _T("Before TraceUnInitialize"));
    TraceUnInitialize();
    return 0;
}//int WINAPI WinMain
