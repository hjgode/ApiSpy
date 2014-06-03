/********************************************************************
Module : SpyEngine.cpp - part of ApiSpyDll implementation 
             Written 2003 by Dmitri Leman
             for an article about CE Api Spy.
Purpose: Contains the Spy engine - the main part of ApiSpyDll

  This file was compiled using eMbedded Visual C++ 3.0 
  with Pocket PC 2002 SDK and 4.0 with Standard SDK.
********************************************************************/

#include <windows.h>
#include <Tlhelp32.h>
//Redefinitions for some CE internal structures and undocumented API:
#include "SysDecls.h" 
#include "SpyEngine.h"
#include "SpyControl.h"
#include "HTrace.h"

#ifndef TH32CS_SNAPNOHEAPS
	#define TH32CS_SNAPNOHEAPS 0x40000000
#endif

#define NUM_OUR_API_METHODS 32

HINSTANCE g_hInst = NULL;//this is our DLL instance

#define TRACE_SIZE 0x100000

#define countof(array) (sizeof(array)/sizeof(array[0]))

#pragma data_seg("SH_DATA")
SpyEngine g_SpyEngine = {0};//The only static instance of SpyEngine.
BOOL      g_bStarted = FALSE;
#pragma data_seg()
#pragma comment( linker, "/SECTION:SH_DATA,SRW" )

/*****************   Beginning of Toolhelp replacement *************/
//Beginning of Toolhelp replacement. PocketPC emulation and retail 
//devices do have toolhelp support, but CE.NET emulation version 
//does not include toolhelp.dll.  Therefore, I have to reimplement 
//some toolhelp methods myself using public declarations of necessary
//structures in PUBLIC\COMMON\OAK\INC\TOOLHELP.H.

typedef HANDLE WINAPI T_CreateToolhelp32Snapshot
    (DWORD dwFlags, DWORD th32ProcessID);
typedef BOOL WINAPI T_CloseToolhelp32Snapshot(HANDLE hSnapshot);
typedef BOOL WINAPI T_Module32First
    (HANDLE hSnapshot, LPMODULEENTRY32 lpme);
typedef BOOL WINAPI T_Module32Next
    (HANDLE hSnapshot, LPMODULEENTRY32 lpme);
typedef BOOL WINAPI T_Process32First
    (HANDLE hSnapshot, LPPROCESSENTRY32 lppe);
typedef BOOL WINAPI T_Process32Next
    (HANDLE hSnapshot, LPPROCESSENTRY32 lppe);

T_CreateToolhelp32Snapshot * g_pCreateToolhelp32Snapshot = NULL;
T_CloseToolhelp32Snapshot  * g_pCloseToolhelp32Snapshot = NULL;
T_Module32First * g_pModule32First = NULL;
T_Module32Next  * g_pModule32Next  = NULL;
T_Process32First* g_pProcess32First = NULL;
T_Process32Next * g_pProcess32Next  = NULL;


//From PUBLIC\COMMON\OAK\INC\TOOLHELP.H:
typedef struct TH32PROC {
	PROCESSENTRY32 procentry;
	void *pMainHeapEntry;
	struct TH32PROC *pNext;
} TH32PROC;
typedef struct TH32MOD {
	MODULEENTRY32 modentry;
	struct TH32MOD *pNext;
} TH32MOD;
typedef struct THSNAP {
	LPBYTE pNextFree;
	LPBYTE pHighCommit;
	LPBYTE pHighReserve;
	TH32PROC *pProc;
	TH32MOD *pMod;
	void *pThread;
	void *pHeap;
} THSNAP;

/*-------------------------------------------------------------------
   FUNCTION: MyCreateToolhelp32Snapshot
   PURPOSE:  Replacement to ToolHelp CreateToolhelp32Snapshot.
   Parameters and return value are identical to
   documented CreateToolhelp32Snapshot.
-------------------------------------------------------------------*/
HANDLE MyCreateToolhelp32Snapshot
(
    DWORD p_dwFlags, 
    DWORD p_th32ProcessID
)
{
    THSNAP * l_pSnapshot = (THSNAP *)
        THCreateSnapshot(p_dwFlags, p_th32ProcessID);
    HTRACE(TG_DebugSpyBrief, _T("THCreateSnapshot ret %x"), 
        l_pSnapshot);
    return (HANDLE)l_pSnapshot;
}

/*-------------------------------------------------------------------
   FUNCTION: MyCloseToolhelp32Snapshot
   PURPOSE:  Replacement to ToolHelp CloseToolhelp32Snapshot.
   Parameters and return value are identical to
   documented CloseToolhelp32Snapshot.
-------------------------------------------------------------------*/
BOOL MyCloseToolhelp32Snapshot(HANDLE p_hSnapshot)
{
    MEMORY_BASIC_INFORMATION l_MemInfo;
    if(!VirtualQuery((void*)p_hSnapshot, 
        &l_MemInfo, sizeof(l_MemInfo)))
        return FALSE;
    VirtualFree((void*)p_hSnapshot, 
        l_MemInfo.RegionSize, MEM_DECOMMIT);
    VirtualFree((void*)p_hSnapshot, 0, MEM_RELEASE);
    return TRUE;
}

/*-------------------------------------------------------------------
   FUNCTION: MyModule32First
   PURPOSE:  Replacement to ToolHelp Module32First.
   Parameters and return value are identical to
   documented Module32First.
-------------------------------------------------------------------*/
BOOL MyModule32First(HANDLE p_hSnapshot, LPMODULEENTRY32 p_lpme)
{
    THSNAP * l_pSnapshot = (THSNAP*)p_hSnapshot;
    if(!l_pSnapshot || !l_pSnapshot->pMod || !p_lpme  || 
        p_lpme->dwSize < sizeof(MODULEENTRY32))
        return FALSE;
    memcpy(p_lpme, &l_pSnapshot->pMod->modentry, 
        sizeof(MODULEENTRY32));
    p_lpme->dwFlags = (DWORD)l_pSnapshot->pMod->pNext;
    //Kludge. dwFlags is reserved, so reuse it.
    return TRUE;
}

/*-------------------------------------------------------------------
   FUNCTION: MyModule32Next
   PURPOSE:  Replacement to ToolHelp Module32Next.
   Parameters and return value are identical to
   documented Module32Next.
-------------------------------------------------------------------*/
BOOL MyModule32Next(HANDLE p_hSnapshot, LPMODULEENTRY32 p_lpme)
{
    THSNAP * l_pSnapshot = (THSNAP*)p_hSnapshot;
    if(!l_pSnapshot || !l_pSnapshot->pMod || !p_lpme)
        return FALSE;
    if(p_lpme->dwFlags < (DWORD)l_pSnapshot || 
        p_lpme->dwFlags >= (DWORD)l_pSnapshot->pHighCommit)
        return FALSE;
    TH32MOD * l_pNextMod = (TH32MOD*)p_lpme->dwFlags;
    //Kludge. dwFlags is reserved, so reuse it.
    memcpy(p_lpme, &l_pNextMod->modentry, sizeof(MODULEENTRY32));
    p_lpme->dwFlags = (DWORD)l_pNextMod->pNext;
    //Kludge. dwFlags is reserved, so reuse it.
    return TRUE;
}

/*-------------------------------------------------------------------
   FUNCTION: MyProcess32First
   PURPOSE:  Replacement to ToolHelp Process32First.
   Parameters and return value are identical to
   documented Process32First.
-------------------------------------------------------------------*/
BOOL MyProcess32First(HANDLE p_hSnapshot, LPPROCESSENTRY32 p_lppe)
{
    THSNAP * l_pSnapshot = (THSNAP*)p_hSnapshot;
    if(!l_pSnapshot || !l_pSnapshot->pProc || !p_lppe  || 
        p_lppe->dwSize < sizeof(PROCESSENTRY32))
        return FALSE;
    memcpy(p_lppe, &l_pSnapshot->pProc->procentry, 
        sizeof(PROCESSENTRY32));
    p_lppe->dwFlags = (DWORD)l_pSnapshot->pProc->pNext;
    //Kludge. dwFlags is reserved, so reuse it.
    return TRUE;
}

/*-------------------------------------------------------------------
   FUNCTION: MyProcess32Next
   PURPOSE:  Replacement to ToolHelp Process32Next.
   Parameters and return value are identical to
   documented Process32Next.
-------------------------------------------------------------------*/
BOOL MyProcess32Next(HANDLE p_hSnapshot, LPPROCESSENTRY32 p_lppe)
{
    THSNAP * l_pSnapshot = (THSNAP*)p_hSnapshot;
    if(!l_pSnapshot || !l_pSnapshot->pProc || !p_lppe)
        return FALSE;
    if(p_lppe->dwFlags < (DWORD)l_pSnapshot || 
        p_lppe->dwFlags >= (DWORD) l_pSnapshot->pHighCommit)
        return FALSE;
    TH32PROC * l_pNextProc = (TH32PROC*)p_lppe->dwFlags;
    //Kludge. dwFlags is reserved, so reuse it.
    memcpy(p_lppe, &l_pNextProc->procentry, 
        sizeof(PROCESSENTRY32));
    p_lppe->dwFlags = (DWORD)l_pNextProc->pNext;
    //Kludge. dwFlags is reserved, so reuse it.
    return TRUE;
}

/*-------------------------------------------------------------------
   FUNCTION: LoadToolHelp
   PURPOSE:  Loads ToolHelp API or, if it is missing, initialize
   pointers to our replacement routines.
   RETURNS: TRUE on success, FALSE on failure
-------------------------------------------------------------------*/
BOOL LoadToolHelp()
{
    if(g_pCreateToolhelp32Snapshot)
        return TRUE;
    HINSTANCE l_hInst = LoadLibrary(_T("TOOLHELP"));
    if(!l_hInst)
    {
        HTRACE(TG_DebugSpyBrief, 
            _T("Cannot find ToolHelp library. %d"), GetLastError());
    }
    else
    {
        g_pCreateToolhelp32Snapshot = (T_CreateToolhelp32Snapshot *)
            GetProcAddress(l_hInst, _T("CreateToolhelp32Snapshot"));
        g_pCloseToolhelp32Snapshot = (T_CloseToolhelp32Snapshot  *)
            GetProcAddress(l_hInst, _T("CloseToolhelp32Snapshot"));
        g_pModule32First = (T_Module32First * )
            GetProcAddress(l_hInst, _T("Module32First"));
        g_pModule32Next  = (T_Module32Next  * )
            GetProcAddress(l_hInst, _T("Module32Next"));
        g_pProcess32First = (T_Process32First* )
            GetProcAddress(l_hInst, _T("Process32First"));
        g_pProcess32Next  = (T_Process32Next * )
            GetProcAddress(l_hInst, _T("Process32Next"));

        if(g_pCreateToolhelp32Snapshot && g_pCloseToolhelp32Snapshot 
            && g_pModule32First && g_pModule32Next &&
            g_pProcess32First && g_pProcess32Next)
            return TRUE;
        HTRACE(TG_Error, 
            _T("Cannot find methods in ToolHelp library"));
    }
    if(l_hInst != NULL)
    {
        FreeLibrary(l_hInst);
    }
    g_pCreateToolhelp32Snapshot = MyCreateToolhelp32Snapshot;
    g_pCloseToolhelp32Snapshot = MyCloseToolhelp32Snapshot;
    g_pModule32First = MyModule32First;
    g_pModule32Next  = MyModule32Next;
    g_pProcess32First = MyProcess32First;
    g_pProcess32Next  = MyProcess32Next;
    HTRACE(TG_DebugSpyBrief, _T("Use our own toolhelp replacement"));
    return TRUE;
}
/*****************   End of Toolhelp replacement *******************/

/*-------------------------------------------------------------------
   FUNCTION: InitProcessList
   PURPOSE:  InitProcessList uses Toolhelp API to enumerate running 
   processes and store process information to our 
   g_SpyEngine.m_Processes array. This will allow accessing this 
   process information from our interceptor routines.
   RETURNS: TRUE on success, FALSE on failure
-------------------------------------------------------------------*/
BOOL InitProcessList()
{
    memset(g_SpyEngine.m_Processes, 0, 
        sizeof(g_SpyEngine.m_Processes));

    HANDLE l_hSnapShotProc = g_pCreateToolhelp32Snapshot(TH32CS_SNAPPROCESS | TH32CS_SNAPNOHEAPS, 0);
    if(l_hSnapShotProc == (HANDLE)-1)
        return FALSE;
    
    PROCESSENTRY32 l_ProcEntry;
    l_ProcEntry.dwSize = sizeof(l_ProcEntry);
    if(g_pProcess32First(l_hSnapShotProc, &l_ProcEntry))
    {
        HTRACE(TG_DebugSpyDetailed, 
            _T("Proc %x %s"), l_ProcEntry.th32AccessKey,
            l_ProcEntry.szExeFile);
        do
        {
            int l_iKey = l_ProcEntry.th32AccessKey;
            int l_iIndex = 0;
            l_iKey >>= 1;
            while(l_iKey)
            {
                l_iKey >>= 1;
                l_iIndex++;
            }
            memcpy(g_SpyEngine.m_Processes + l_iIndex, 
                &l_ProcEntry, l_ProcEntry.dwSize);
        } while(g_pProcess32Next(l_hSnapShotProc, &l_ProcEntry));
    }
    g_pCloseToolhelp32Snapshot(l_hSnapShotProc);
    return TRUE;
}//BOOL InitProcessList()

/*-------------------------------------------------------------------
   FUNCTION: HookCoredll
   PURPOSE:  HookCoredll replaces pointers to Win32 method table, 
   which coredll caches in it's data section when each process loads. 
   Later most of coredll exported methods will use this cached table 
   to call the kernel directly (bypassing the regular API dispatcher)
   Therefore, Spy has to replace this cached pointer in each process.
   RETURNS: TRUE on success, FALSE on failure
-------------------------------------------------------------------*/
BOOL HookCoredll(LPVOID p_pOldWin32Methods,LPVOID p_pNewWin32Methods)
{
    HTRACE(TG_DebugSpyBrief, 
        _T("HookCoredll(old methods=%x, new=%x)\r\n"),
        p_pOldWin32Methods, p_pNewWin32Methods);

    //Get address of any exported routine in coredll, which is known
    //to use the cached table. EventModify happened to be one.
    HINSTANCE l_hInst = LoadLibrary(_T("COREDLL"));
    LPCTSTR l_pszMethodName = _T("EventModify");
    LPBYTE l_pCoreDllFunc = (LPBYTE)GetProcAddress
        (l_hInst, l_pszMethodName);
    if(!l_pCoreDllFunc)
    {
        HTRACE(TG_Error, 
         _T("ERROR: failed to get address of %s in coredll. Err %d"),
            l_pszMethodName, GetLastError());
        FreeLibrary(l_hInst);
        return FALSE;
    }

    //Now scan the body of the method and look for a pointer to the 
    //table. There is no way to know exactly where the method ends,
    //so use 256 as an upper limit.
    //This technique is fragile an may need adjustments for different
    //CPUs and newer versions of the OS.
    DWORD l_dwAddressInCoredll = 0;
    for(int i = 0; i < 256; i++)
    {
        __try
        {
            #if defined(ARM) || defined(SH3)
            //ARM don't like misaligned addresses
            if((DWORD)(l_pCoreDllFunc+i) & 3)
                continue;
            #endif

            HTRACE(TG_DebugSpyDetailed, 
                _T("Before reading from %x\r\n"),
                l_pCoreDllFunc+i);
            DWORD * l_pdwAddress = *((DWORD**)(l_pCoreDllFunc+i));

            MEMORY_BASIC_INFORMATION l_MemInfo;
            if(!VirtualQuery(l_pdwAddress, 
                &l_MemInfo, sizeof(l_MemInfo)))
                continue;
            if(l_MemInfo.Protect != PAGE_READWRITE && 
				l_MemInfo.Protect != PAGE_EXECUTE_READWRITE)
                continue;
            
            #if defined(ARM) || defined(SH3)
            if((DWORD)l_pdwAddress & 3)
                continue;
            #endif

            HTRACE(TG_DebugSpyDetailed, 
                _T("Before reading from %x\r\n"), l_pdwAddress);
            DWORD l_dwAddress2 = *l_pdwAddress;

            HTRACE(TG_DebugSpyDetailed, 
                _T("Comparing %x with %x\r\n"), 
                l_dwAddress2, p_pOldWin32Methods);
            if(l_dwAddress2 == (DWORD)p_pOldWin32Methods)
            {
                HTRACE(TG_DebugSpyDetailed, 
                    _T("Found pointer to table %x at %x\r\n"),
                    p_pOldWin32Methods, l_pdwAddress);
                l_dwAddressInCoredll = (DWORD)l_pdwAddress;
                break;
            }
        }
        __except(1)
        {//Exceptions in this code are not unusual
        }
    }//for(int i = 0; i < 256; i++)

    if(l_dwAddressInCoredll == 0)
    {
        HTRACE(TG_Error, 
        _T("ERROR: Failed to find cached table %x in method %s\r\n"),
            p_pOldWin32Methods, l_pszMethodName);
        FreeLibrary(l_hInst);
        return FALSE;
    }
    //OK, now we know where is cached table pointer is hiding.
    //Replace it in all running processes (each process has it's own
    //copy of coredll's data section). Here we rely on fact, that on 
    //CE a DLL is always loaded at the same address in all processes.
    for(int l_iProcID = 0; 
        l_iProcID < countof(g_SpyEngine.m_Processes); l_iProcID++)
    {
        PROCESSENTRY32 * l_pProcEntry = 
            g_SpyEngine.m_Processes + l_iProcID;
        if(!l_pProcEntry->dwSize)
            continue;//This slot is not used
        
        DWORD l_dwProcBaseAddr = 0x2000000 * (l_iProcID+1);
        DWORD * l_pdwAddress = (DWORD*)
            ((l_dwAddressInCoredll & 0x1ffffff) + 
            l_dwProcBaseAddr);
        __try
        {
            MEMORY_BASIC_INFORMATION l_MemInfo;
            if(!VirtualQuery(l_pdwAddress, 
                &l_MemInfo, sizeof(l_MemInfo)) ||
              (l_MemInfo.Protect != PAGE_READWRITE &&
               l_MemInfo.Protect != PAGE_EXECUTE_READWRITE))
            {
                //This seems to me normal for NK.EXE, so don't print
                //this as an error for NK.EXE.
                HTRACE(l_iProcID? TG_Error : TG_DebugSpyDetailed, 
                    _T("Failed to access address %x in process %s"),
                    l_pdwAddress, l_pProcEntry->szExeFile);
                continue;
            }

            if(*l_pdwAddress == (DWORD)p_pOldWin32Methods)
            {
                HTRACE(TG_DebugSpyDetailed, 
                    _T("replacing function pointer (%x by %x)")
                    _T(" at %x in process %x %s\r\n"),
                    *l_pdwAddress, p_pNewWin32Methods, l_pdwAddress, 
                    l_pProcEntry->th32ProcessID, 
                    l_pProcEntry->szExeFile);
                *l_pdwAddress = (DWORD)p_pNewWin32Methods;

                //Double check. This is not really necessary.
                FlushInstructionCache(GetCurrentProcess(), 0, 0);
                if(*l_pdwAddress != (DWORD)p_pNewWin32Methods)
                {
                    HTRACE(TG_Error, 
                        _T("ERROR: failed to replace function")
                        _T(" pointer (%x by %x) at %x\r\n"),
                        *l_pdwAddress, p_pNewWin32Methods, 
                        l_pdwAddress);
                }
                else
                {
                    HTRACE(TG_DebugSpyDetailed,
                        _T("replaced function pointer at %x\r\n"), 
                        l_pdwAddress);
                }
            }
            else
            {
                HTRACE(TG_DebugSpyDetailed, 
                    _T("Don't replace pointer (%x by %x)")
                    _T(" at %x in process %x %s\r\n"),
                    *l_pdwAddress, p_pNewWin32Methods, l_pdwAddress, 
                    l_pProcEntry->th32ProcessID, 
                    l_pProcEntry->szExeFile);
            }
        }
        __except(1)
        {
            HTRACE(TG_Error, 
                _T("ERROR: Exception trying to access address %x")
                _T(" in process %x %s\r\n"),
                l_pdwAddress, 
                l_pProcEntry->th32ProcessID,l_pProcEntry->szExeFile);
        }
    }//for(int l_iProcID = 0; l_iProcID < countof(m_Processes);)
    FreeLibrary(l_hInst);
    return TRUE;
}//BOOL HookCoredll

/*-------------------------------------------------------------------
   FUNCTION: ProcessAddress
   PURPOSE:  
   returns an address of memory slot for the given process index.
   PARAMETERS:
    BYTE p_byProcNum - process number (slot index) between 0 and 31
   RETURNS:
    Address of the memory slot.
-------------------------------------------------------------------*/
inline DWORD ProcessAddress(BYTE p_byProcNum)
{
    return 0x02000000 * (p_byProcNum+1);
}

/*-------------------------------------------------------------------
   FUNCTION: ConvertAddr
   PURPOSE:  
    ConvertAddr does the same as MapPtrToProcess - maps an address in
    slot 0 to the address in the slot of the given process. Unlike 
    MapPtrToProcess, which accepts process handle, ConvertAddr uses
    undocumented PROCESS structure.
   PARAMETERS:
    LPVOID p_pAddr - address to convert
    PPROCESS p_pProcess - internal kernel Process structure
   RETURNS:
    Address mapped to the slot of the given process
-------------------------------------------------------------------*/
LPVOID ConvertAddr(LPVOID p_pAddr, PPROCESS p_pProcess)
{
    if( ((DWORD)p_pAddr) < 0x2000000 && p_pProcess)
    {//Slot 0 and process is not the kernel
        LPVOID l_pOld = p_pAddr;
        BYTE l_byProcNum = 
            *(((LPBYTE)p_pProcess) + PROCESS_NUM_OFFSET);
        p_pAddr = (LPVOID) (((DWORD)p_pAddr) + 
            ProcessAddress(l_byProcNum));
        HTRACE(TG_DebugSpyDetailed, 
            _T("Converted address %x to %x for server %x\r\n"), 
            l_pOld, p_pAddr, l_byProcNum);
    }
    return p_pAddr;
}

/*-------------------------------------------------------------------
   FUNCTION: GetKernelProcessHandle
   PURPOSE:  returns handle to the kernel process (nk.exe)
   RETURNS: HANDLE of nk.exe
-------------------------------------------------------------------*/
HANDLE GetKernelProcessHandle()
{
//In CE 3.0 and PPC 2002 I used GetProcFromPtr((LPVOID)0x2000000)
//to get handle to process nk.exe, but in CE 4.0 this does not work.
//HANDLE hNKProc = GetProcFromPtr((LPVOID)0x2000000);
    
    //Use process #0 from the array of preloaded processes.
    HANDLE l_hNKProc = OpenProcess(0, FALSE, 
        g_SpyEngine.m_Processes[0].th32ProcessID);
    if(l_hNKProc == NULL)
    {
        HTRACE(TG_Error, 
            _T("ERROR: OpenProcess(%x) failed. Err %d\r\n"),
            g_SpyEngine.m_Processes[0].th32ProcessID,GetLastError());
        return FALSE;
    }
    return l_hNKProc;
}

/*-------------------------------------------------------------------
   FUNCTION: CallCoredllInProc
   PURPOSE:  CallCoredllInProc uses undocumented method 
    PerformCallBack4 to call exported methods from coredll.dll in 
    the specified process.
   PARAMETERS:
    HANDLE  p_hProcess - handle to the process, where the call should
        be made
    LPCTSTR p_pszMethodName - name of method exported from coredll, 
        such as VirtualAlloc, VirtualFree, etc.
    DWORD p_dwParam1, p_dwParam2, p_dwParam3, p_dwParam4 - arguments
    DWORD * p_pdwResult - pointer to the return value
   RETURNS:
    TRUE on success, FALSE on failure
-------------------------------------------------------------------*/
BOOL CallCoredllInProc
(
    HANDLE  p_hProcess,
    LPCTSTR p_pszMethodName,
    DWORD   p_dwParam1, DWORD p_dwParam2, 
    DWORD   p_dwParam3, DWORD p_dwParam4,
    DWORD * p_pdwResult)
{
    HINSTANCE l_hCoreDll = NULL;
    BOOL l_bReturn = FALSE;
    __try
    {
        //Use undocumented method PerformCallBack4 
        //to call method in NK.EXE.
        CALLBACKINFO CallbackInfo;
        CallbackInfo.m_hDestinationProcessHandle = p_hProcess;
        l_hCoreDll = LoadLibrary(_T("COREDLL"));
        CallbackInfo.m_pFunction = (FARPROC)GetProcAddress(l_hCoreDll, p_pszMethodName);
        if(!CallbackInfo.m_pFunction)
        {
            HTRACE(TG_Error, 
                _T("GetProcAddress(%x, %s) failed. Err %d"), 
                l_hCoreDll, p_pszMethodName, GetLastError());
        }
        else
        {
            CallbackInfo.m_pFirstArgument = (LPVOID)p_dwParam1;
            DWORD l_dwResult = PerformCallBack4
                (&CallbackInfo, p_dwParam2, p_dwParam3, p_dwParam4);
            if(p_pdwResult)
            {
                *p_pdwResult = l_dwResult;
            }
            l_bReturn = TRUE;
        }
    }
    __except(1)
    {
        HTRACE(TG_Error, _T("Exception in CallCoredllInProc(%s)"), 
            p_pszMethodName);
        l_bReturn = FALSE;
    }
    if(l_hCoreDll)
    {
        FreeLibrary(l_hCoreDll);
    }
    return l_bReturn;
}//BOOL CallCoredllInProc

/*-------------------------------------------------------------------
   FUNCTION: CallCoredllInKernelProc
   PURPOSE:  CallCoredllInKernelProc uses undocumented method 
    PerformCallBack4 to call exported methods from coredll.dll in the
    kernel process (nk.exe).
   PARAMETERS:
    LPCTSTR p_pszMethodName - name of method exported from coredll, 
        such as VirtualAlloc, VirtualFree, etc.
    DWORD p_dwParam1, p_dwParam2, p_dwParam3, p_dwParam4 - arguments
    DWORD * p_pdwResult - pointer to the return value
   RETURNS:
    TRUE on success, FALSE on failure
-------------------------------------------------------------------*/
BOOL CallCoredllInKernelProc(LPCTSTR p_pszMethodName,
    DWORD p_dwParam1, DWORD p_dwParam2, 
    DWORD p_dwParam3, DWORD p_dwParam4,
    DWORD * p_pdwResult)
{
    HANDLE l_hNKProc = GetKernelProcessHandle();
    if(!l_hNKProc)
        return FALSE;
    BOOL l_bReturn = CallCoredllInProc(l_hNKProc, p_pszMethodName, 
        p_dwParam1, p_dwParam2, p_dwParam3, p_dwParam4, p_pdwResult);
    CloseHandle(l_hNKProc);
    return l_bReturn;
}//BOOL CallCoredllInKernelProc

/*-------------------------------------------------------------------
   FUNCTION: FreeMemInKernelProc
   PURPOSE:  FreeMemInKernelProc is used to free memory, which was 
    allocated in the nk.exe process space by AllocateMemInKernelProc.
   PARAMETERS:
    LPVOID p_pMem - memory to free
   RETURNS:
    TRUE on success, FALSE on failure
-------------------------------------------------------------------*/
BOOL FreeMemInKernelProc(LPVOID p_pMem)
{
    if(!CallCoredllInKernelProc(_T("VirtualFree"),
        (DWORD)p_pMem, 0, MEM_RELEASE, 0, NULL))
    {
        HTRACE(TG_Error, _T("Failed to free %x in NK.EXE"), p_pMem);
        return FALSE;
    }
    return TRUE;
}

/*-------------------------------------------------------------------
   FUNCTION: AllocateMemInKernelProc
   PURPOSE:  AllocateMemInKernelProc is used to allocate memory in 
    the process #0 - nk.exe, which is accessible from all other 
    processes. We need this memory to create duplicate method tables, 
    which we will use to substitute original method tables provided 
    by the kernel or by server processes.
   PARAMETERS:
    int p_iSize - size to allocate
   RETURNS:
    Pointer to the allocated memory in the kernel process
-------------------------------------------------------------------*/
void * AllocateMemInKernelProc(int p_iSize)
{
    LPVOID l_pAllocated = NULL;
    if(!CallCoredllInKernelProc(_T("VirtualAlloc"),
        0, p_iSize, MEM_COMMIT, PAGE_READWRITE, 
        (LPDWORD)&l_pAllocated))
    {
        HTRACE(TG_Error, 
            _T("Failed to allocate %d bytes in NK.EXE"), p_iSize);
        return NULL;
    }
    HTRACE(TG_DebugSpyBrief, 
        _T("VirtualAlloc(%d) in NK.EXE returned %x"), 
        p_iSize, l_pAllocated);
    //Currently l_pAllocated is slot 0 address, so it is not valid
    //unless the current process is NK.EXE. So we need to map
    //it to slot 1, which is the home of NK.EXE.
    HANDLE l_hNKProc = GetKernelProcessHandle();
    LPVOID l_pRemoteMem = l_hNKProc? 
        MapPtrToProcess(l_pAllocated, l_hNKProc) : NULL;
    if(!l_pRemoteMem)
    {
        HTRACE(TG_Error, 
            _T("Failed to map ptr %x to NK.EXE. Err %d"), 
            l_pAllocated, GetLastError());
        FreeMemInKernelProc(l_pAllocated);
    }
    CloseHandle(l_hNKProc);
    HTRACE(TG_DebugSpyBrief, 
        _T("AllocateMemInKernelProc(%d) ret %x\r\n"), 
        p_iSize, l_pRemoteMem);
	return l_pRemoteMem;
}//void * AllocateMemInKernelProc

/*-------------------------------------------------------------------
   FUNCTION: LoadHookDllIntoProcess
   PURPOSE: In order to spy for APIs served by other processes, the 
    Spy interceptor routines must be accessible from these processes. 
    This can be achieved by loading the Spy DLL into these processes. 
    The way to do that is by using undocumented PerformCallBack4 
    routine to execute LoadLibrary in other processes. We cannot 
    simply call LoadLibrary exported from coredll, because it can be 
    intercepted by us, but the interceptor will crash because the 
    SpyEngine is not loaded yet into the process. Therefore, use 
    m_pOriginalLoadLibrary, which keeps a trap to the original 
    W32_LoadLibraryW handler.
   PARAMETERS:
    HANDLE p_hProcess - handle to the process where the DLL should
        be loaded.
   RETURNS:
    Instance handle of the loaded DLL in the target process.
-------------------------------------------------------------------*/
HINSTANCE SpyEngine::LoadHookDllIntoProcess(HANDLE p_hProcess)
{
	void *t=GetProcAddress(GetModuleHandle(L"coredll.dll"),L"LoadLibraryW");
	//ci.pfn=(FARPROC)MapPtrToProcess(t,Proc);

    CALLBACKINFO CallbackInfo;
    CallbackInfo.m_hDestinationProcessHandle = p_hProcess;
    if(m_pOriginalLoadLibrary==0)
		CallbackInfo.m_pFunction = (FARPROC)MapPtrToProcess(t,p_hProcess);// (FARPROC)m_pOriginalLoadLibrary;
	else
		CallbackInfo.m_pFunction = (FARPROC)m_pOriginalLoadLibrary;

    CallbackInfo.m_pFirstArgument = (LPVOID)m_pszSpyDllPathInKernelMemory;

    HTRACE(TG_DebugSpyBrief, _T("Load our DLL in proc %x using func %x\r\n"), p_hProcess, m_pOriginalLoadLibrary);

    DWORD l_dwResult = PerformCallBack4(&CallbackInfo, 0, 0, 0);
    
	HTRACE(TG_DebugSpyBrief, _T("Loaded %x\r\n"), l_dwResult);
    return (HINSTANCE)l_dwResult;
}

/*-------------------------------------------------------------------
   FUNCTION: UnloadHookDllInProcess
   PURPOSE: UnloadHookDllInProcess is used to unload SpyDll 
    from a process, where it was loaded by LoadHookDllIntoProcess.
   PARAMETERS:
    HANDLE p_hProcess - handle to process where the DLL was loaded
    HINSTANCE p_hInst - instance handle of the loaded DLL
   RETURNS:
    TRUE on success, FALSE on failure
-------------------------------------------------------------------*/
BOOL SpyEngine::UnloadHookDllInProcess
(
    HANDLE p_hProcess, 
    HINSTANCE p_hInst
)
{
    DWORD l_dwResult = 0;
    if(!CallCoredllInProc(p_hProcess, _T("FreeLibrary"), 
        (DWORD)p_hInst, 0,0,0, &l_dwResult))
        return FALSE;
    HTRACE(TG_DebugSpyBrief, 
        _T("FreeLibrary(%x) in process %x ret %x\r\n"), 
        p_hInst, p_hProcess, l_dwResult);
    return l_dwResult;
}

/*-------------------------------------------------------------------
   FUNCTION: LoadHookDllInAllProcesses
   PURPOSE: Load our Spy DLL in all running processes so that these 
    processes will be able to access our interceptor routines.
-------------------------------------------------------------------*/
void SpyEngine::LoadHookDllInAllProcesses()
{
    HTRACE(TG_DebugSpyBrief, _T("LoadHookDllInAllProcesses()\r\n"));

    //Don't load Spy DLL into NK.EXE (process #0) because NK.EXE 
    //already has access to all addresses and because it is difficult
    //to unload from there.
    for(int l_iProcID = 1; l_iProcID < countof(m_Processes); 
        l_iProcID++)
    {
        PROCESSENTRY32 * l_pProcEntry = m_Processes + l_iProcID;
        if(!l_pProcEntry->dwSize)
            continue;//This slot is not used
        if(l_pProcEntry->th32ProcessID == GetCurrentProcessId())
            continue;//This is Spy itself.

        HANDLE l_hProc = OpenProcess(0, FALSE, l_pProcEntry->th32ProcessID);
        if(l_hProc == NULL)
        {
            HTRACE(TG_Error, 
                _T("ERROR: Failed to open process %x %s\r\n"),
                l_pProcEntry->th32ProcessID,l_pProcEntry->szExeFile);
        }
        else
        {
            HTRACE(TG_DebugSpyBrief, _T("Load our DLL in process %x %s\r\n"), l_pProcEntry->th32ProcessID, l_pProcEntry->szExeFile);
            m_hSpyDllLoaded[l_iProcID] = LoadHookDllIntoProcess(l_hProc);
            CloseHandle(l_hProc);
        }
    }//for(int l_iProcID = 0; l_iProcID < countof(m_Processes);
    HTRACE(TG_DebugSpyBrief, _T("LoadHookDllInAllProcesses() ends\r\n"));
}//void SpyEngine::LoadHookDllInAllProcesses

/*-------------------------------------------------------------------
   FUNCTION: CloseAPISet
   PURPOSE: CloseAPISet closes the API handle returned by 
    CreateAPISet. For some reason CloseAPISet routine is not 
    exported from coredll. So, call it using API trap.
   PARAMETERS:
    HANDLE p_hSet - API handle returned by CreateAPISet
-------------------------------------------------------------------*/
void CloseAPISet(HANDLE p_hSet)
{
    __try
    {
        ((void (*)(HANDLE)) IMPLICIT_CALL(HT_APISET, 0)) (p_hSet);
    }
    __except(1)
    {
        HTRACE(TG_Error, _T("ERROR: exception in CloseAPISet"));
    }
}

/*-------------------------------------------------------------------
   FUNCTION: CreateDuplicateMethodTable
   PURPOSE: CreateDuplicateMethodTable used to allocate memory in 
    the kernel process space and copy the given method and signature 
    table to this memory.
   PARAMETERS:
    int        p_iNumMethods - number of methods
    PFNVOID *  p_pMethods    - method table
    DWORD   *  p_pdwSignatures - signature table (May be NULL)
    PFNVOID ** p_ppReturnHookMthds- return the allocated method table
    DWORD   ** p_ppdwReturnHookSignatures - return the signatures
   RETURNS:
    TRUE on success, FALSE on failure
-------------------------------------------------------------------*/
BOOL CreateDuplicateMethodTable
(
    int        p_iNumMethods, 
    PFNVOID *  p_pMethods,
    DWORD   *  p_pdwSignatures,//May be NULL
    PFNVOID ** p_ppReturnHookMthds,
    DWORD   ** p_ppdwReturnHookSignatures
)
{
    int l_iSize = (sizeof(PFNVOID)*p_iNumMethods + 
                   sizeof(DWORD)*p_iNumMethods);
    LPBYTE l_pRemoteMem = (LPBYTE)AllocateMemInKernelProc(l_iSize);
    HTRACE(TG_DebugSpyBrief, 
        _T("Create method table at %x\r\n"), l_pRemoteMem);

    PFNVOID * l_pHookMthds = (PFNVOID *)l_pRemoteMem;
    DWORD   * l_pdwHookSignatures = 
        (DWORD*)(l_pRemoteMem + sizeof(PFNVOID)*p_iNumMethods);

    int i;
    for(i = 0; i < p_iNumMethods; i++)
    {
        HTRACE(TG_DebugSpyDetailed, 
            _T("Copy meth #%x %x from %x to %x\r\n"), i, 
            p_pMethods[i], &p_pMethods[i], &l_pHookMthds[i]);
        l_pHookMthds[i] = p_pMethods[i];
        //Our replaced API will be served by our process and we 
        //always should provide  a signature table. But the original
        //signature table pointer will be NULL if the original API 
        //is served by the kernel (therefore no pointer mapping is 
        //required). In that case fill our table by FNSIG0, which 
        //means that no mapping is required.
        l_pdwHookSignatures[i] = p_pdwSignatures? 
            p_pdwSignatures[i] : FNSIG0();
    }
    *p_ppReturnHookMthds = l_pHookMthds;
    *p_ppdwReturnHookSignatures = l_pdwHookSignatures;
    return TRUE;
}//CreateDuplicateMethodTable

/*-------------------------------------------------------------------
   FUNCTION: CreateAndRegisterApi
   PURPOSE: CreateAndRegisterApi calls undocumented methods 
    CreateAPISet, RegisterAPISet and QueryAPISetID to create, 
    register a new API and return it's ID
   PARAMETERS:
    char    * p_pszName       - API name
    int       p_iNumMethods   - number of methods
    PFNVOID * p_ppMethods     - method table
    DWORD   * p_pdwSignatures - method signatures
    HANDLE  * p_phReturnApiHandle - return API handle
   RETURNS:
    Positive API ID (index in the system API table)
    -1 on error
-------------------------------------------------------------------*/
int CreateAndRegisterApi
(
    char    * p_pszName,
    int       p_iNumMethods,
    PFNVOID * p_ppMethods,
    DWORD   * p_pdwSignatures,
    HANDLE  * p_phReturnApiHandle
)
{
    HANDLE l_hApiHandle = CreateAPISet(p_pszName, 
        p_iNumMethods, p_ppMethods, p_pdwSignatures);
    if(!l_hApiHandle)
    {
        HTRACE(TG_Error, 
            _T("ERROR: CreateAPISet(%c%c%c%c, %x, %x, %x) failed.")
            _T("Err %d"),
            p_pszName[0], p_pszName[1], p_pszName[2], p_pszName[3],
            p_iNumMethods, p_ppMethods, p_pdwSignatures,
            GetLastError());
        return -1;
    }
    if(!RegisterAPISet(l_hApiHandle, 0))
    {
        HTRACE(TG_Error,
            _T("ERROR: RegisterAPISet(%x) for API %c%c%c%c failed.")
            _T("Err %d"),
            l_hApiHandle, 
            p_pszName[0], p_pszName[1], p_pszName[2], p_pszName[3],
            GetLastError());
        CloseAPISet(l_hApiHandle);
        return -1;
    }
    int l_iAPISetID = QueryAPISetID(p_pszName);
    if(l_iAPISetID < 0)
    {
        HTRACE(TG_Error,
            _T("ERROR: QueryAPISetID(%c%c%c%c) failed. Err %d"),
            p_pszName[0], p_pszName[1], p_pszName[2], p_pszName[3],
            GetLastError());
        CloseAPISet(l_hApiHandle);//it also unregisters the API
        return -1;
    }
    HTRACE(TG_DebugSpyBrief, 
        _T("CreateAndRegisterApi(%c%c%c%c) NumM %x Mthds %x ")
        _T("Sign %x ret ID %d"),
        p_pszName[0], p_pszName[1], p_pszName[2], p_pszName[3],
        p_iNumMethods, p_ppMethods, p_pdwSignatures, l_iAPISetID);
    *p_phReturnApiHandle = l_hApiHandle;
    return l_iAPISetID;
}//int CreateAndRegisterApi

/*-------------------------------------------------------------------
   FUNCTION: CreateDuplicateApi
   PURPOSE: CreateDuplicateApi is used to create a copy of the
    system API (CINFO structure, method and signature tables).
    Then replace a pointer to the original CINFO by the new one.
    We keep a table m_HookedAPI of such replacement APIs, 
    so CreateDuplicateApi will not allocate a new API for the same
    original more than once.
   PARAMETERS:
    int p_iAPISetId - original API set ID to replace
   RETURNS:
    TRUE on success, FALSE on failure
-------------------------------------------------------------------*/
BOOL SpyEngine::CreateDuplicateApi(int p_iAPISetId)
{
    if(p_iAPISetId < 0 || p_iAPISetId >= countof(m_HookedAPI))
    {
        HTRACE(TG_Error, 
         _T("ERROR: CreateDuplicateApi argument %d is out of range"),
            p_iAPISetId);
        return FALSE;
    }
    HookedAPI * l_pHookedAPI = m_HookedAPI + p_iAPISetId;
    if(l_pHookedAPI->m_bUsed)
        return TRUE;
    HANDLE l_hCurProc = GetCurrentProcess();
    CINFO * l_pOrigApiSet = m_pSystemAPISets[p_iAPISetId];
    
    PFNVOID * l_pHookMthds = NULL;
    DWORD   * l_pdwHookSignatures = NULL;
    PFNVOID * l_pOrigMethods = (PFNVOID *)ConvertAddr
        (l_pOrigApiSet->m_ppMethods,l_pOrigApiSet->m_pProcessServer);
    DWORD   * l_pOrigSignatures = (DWORD *)ConvertAddr
      (l_pOrigApiSet->m_pdwMethodSignatures, 
      l_pOrigApiSet->m_pProcessServer);

    CreateDuplicateMethodTable(l_pOrigApiSet->m_wNumMethods,
        l_pOrigMethods, l_pOrigSignatures,
        &l_pHookMthds, &l_pdwHookSignatures);
    if(p_iAPISetId == SH_WIN32)
    {
        m_ppHookWin32Methods = l_pHookMthds;
        m_pdwHookWin32Signatures = l_pdwHookSignatures;
    }
    CINFO * l_pOurApiSet = (CINFO *)
        AllocateMemInKernelProc(sizeof(CINFO));
    //Use the same name, dispatch type, API ID and server 
    //as the original API:
    memcpy(l_pOurApiSet, l_pOrigApiSet, sizeof(CINFO));

    //But replace the method and signature tables:
    l_pOurApiSet->m_ppMethods = l_pHookMthds;
    l_pOurApiSet->m_pdwMethodSignatures = l_pdwHookSignatures;

    HTRACE(TG_DebugSpyBrief, 
        _T("Created duplicate CINFO at %x for API %x ")
        _T("dup mthds %x %x\r\n"),
        l_pOurApiSet, p_iAPISetId, 
        l_pHookMthds, l_pdwHookSignatures);

    //Now fill our HookedAPI structure so we can undo the 
    //API replacement later.
    l_pHookedAPI->m_bUsed = TRUE;
    l_pHookedAPI->m_bSwapped = FALSE;
    l_pHookedAPI->m_iOrigApiSetId = p_iAPISetId;
    l_pHookedAPI->m_pOrigApiSet = l_pOrigApiSet;
    l_pHookedAPI->m_pOurApiSet = l_pOurApiSet;
    
    //And finally, replace the pointer to the original CINFO
    //by the pointer to ours.
    HTRACE(TG_DebugSpyBrief, 
        _T("Before replace original API %x (CINFO %x by %x) ")
        _T("at %x\r\n"),
        p_iAPISetId,
        m_pSystemAPISets[p_iAPISetId], l_pOurApiSet,
        &m_pSystemAPISets[p_iAPISetId]);

    m_pSystemAPISets[p_iAPISetId] = l_pOurApiSet;
    l_pHookedAPI->m_bSwapped = TRUE;

    HTRACE(TG_DebugSpyBrief, 
        _T("After replace original API %x\r\n"), p_iAPISetId);
    return TRUE;
}//BOOL SpyEngine::CreateDuplicateApi

void DummyRoutine()
{
    //HTRACE(TG_DebugSpyBrief, _T("DummyRoutine"));
}

/*-------------------------------------------------------------------
   FUNCTION: SetPrivateApiMethod
   PURPOSE: SetPrivateApiMethod finds an unused slot in our private
    API method table and stores the given method to it.
   PARAMETERS:
    PFNVOID p_pHookMethod - pointer to the new method
   RETURNS:
    Trap to invoke the given method in our private API 
    or NULL on failure
-------------------------------------------------------------------*/
PFNVOID SpyEngine::SetPrivateApiMethod
(
    PFNVOID p_pHookMethod
)
{
    //We need to find an unused slot in our private API.
    //Start searching from slot #2 because slots #0 and #1 are
    //used for special purposes by the system.
    int i;
    for(i = 2; i < m_pHookAPISet->m_wNumMethods; i++)
    {
        if(m_pHookAPISet->m_ppMethods[i] == DummyRoutine)
        {
            break;
        }
    }
    if(i == m_pHookAPISet->m_wNumMethods)
    {
        HTRACE(TG_Error, 
            _T("ERROR: not enough free slots in hook table"));
        return NULL;
    }
    //Replace the DummyRoutine in our API table by the target method.
    m_pHookAPISet->m_ppMethods[i] = p_pHookMethod;

    //Now create a special "trap" address for this method:
    PFNVOID l_pHookTrap = (PFNVOID)GetAPIAddress(m_iHookAPISetID, i);
    return l_pHookTrap;
}

/*-------------------------------------------------------------------
   FUNCTION: HookMethod
   PURPOSE: HookMethod is the main routine of the SpyEngine.
   It is used to intercept the specified method in the specified API.
   PARAMETERS:
    int p_iAPISetId - the API set ID (index) to intercept
    int p_iMethodIndex - method index in the given API to intercept
    PFNVOID p_pHookMethod - interceptor method in the Spy DLL.
   RETURNS:
    Pointer to the original method. This pointer must be stored
    by the caller and used by the interceptor routine to call the 
    original method.
    NULL is returned on error.
-------------------------------------------------------------------*/
PFNVOID SpyEngine::HookMethod
(
    int p_iAPISetId, 
    int p_iMethodIndex, 
    PFNVOID p_pHookMethod
)
{
    HTRACE(TG_DebugSpyBrief, _T("HookMethod %x %x %x\r\n"),
        p_iAPISetId, p_iMethodIndex, p_pHookMethod);

    //First call CreateDuplicateApi to create a copy of the original 
    //API and swap pointers, so the copy will immediately be used.
    //Note that CreateDuplicateApi can be called many
    //times for the same API - it will only create a copy once, the
    //following calls will simply return the copy created earlier.
    //This allows calling HookMethod for several method from the 
    //same API.
    if(!CreateDuplicateApi(p_iAPISetId))
        return NULL;
    HookedAPI & l_rAPI = m_HookedAPI[p_iAPISetId];

    //Then prepare an address to the method table of the target API:
    PFNVOID * l_pOrigMethods = (PFNVOID *)ConvertAddr(
        l_rAPI.m_pOrigApiSet->m_ppMethods, 
        l_rAPI.m_pOrigApiSet->m_pProcessServer);

    HTRACE(TG_DebugSpyBrief, 
        _T("Replace method #%x(%x) in API %x by hook %x"),
        p_iMethodIndex, 
        ((PFNVOID*)l_rAPI.m_pOurApiSet->m_ppMethods)[p_iMethodIndex],
        l_rAPI.m_iOrigApiSetId,
        p_pHookMethod);

    PFNVOID l_pOrig = ((PFNVOID *)l_pOrigMethods)[p_iMethodIndex];

    //Finally, put our interceptor to the table of the target API:
    ((PFNVOID *)l_rAPI.m_pOurApiSet->m_ppMethods)[p_iMethodIndex] = 
        p_pHookMethod;

    //Check whether the method is duplicated in the "extra" table
    //and hook it there.
    for(int i = 0; i < SIZE_EXTRA_TABLE; i++)
    {
        if(m_ppHookExtraMethods[i] == l_pOrig)
        {
            HTRACE(TG_DebugSpyDetailed, 
               _T("Replace method #%x(%x) in Extra table by %x\r\n"),
                i, m_ppHookExtraMethods[i], p_pHookMethod);
            m_ppHookExtraMethods[i] = p_pHookMethod;
            break;
        }
    }
    return l_pOrig;
}//PFNVOID SpyEngine::HookMethod

/*-------------------------------------------------------------------
   FUNCTION: HookGetRomFileInfo
   PURPOSE: HookGetRomFileInfo is an interceptor for undocumented
    function GetRomFileInfo, which is called by coredll when a new
    process started to retrieve pointers to 2 system API method
    tables: Win32Methods and ExtraMethods. We must always intercept
    GetRomFileInfo to substitute these 2 pointers and also to load
    the Spy DLL into the new process.
   PARAMETERS:
    DWORD p_dwType - type of information. Value 3 is used to
        retrieve a pointer to Win32Methods, 4 - ExtraMethods.
        Other values are used to enumerate ROM files.
    LPWIN32_FIND_DATA p_pFindData - pointer to a structure to fill 
    DWORD p_dwSize - size of structure pointed by p_pFindData
   RETURNS:
    TRUE on success, FALSE on failure
-------------------------------------------------------------------*/
PFNVOID g_pOrigGetRomFileInfo = NULL;
BOOL HookGetRomFileInfo
(
    DWORD p_dwType, 
    LPWIN32_FIND_DATA p_pFindData, 
    DWORD p_dwSize
)
{
    InterlockedIncrement(&g_SpyEngine.m_lNumCalls);
    InterlockedIncrement(&g_SpyEngine.m_lNumHooksActive);

    HANDLE l_hCaller = GetCallerProcess();
    
    HTRACE(TG_DebugSpyDetailed, 
        _T("HookGetRomFileInfo enter type = %d\r\n"), p_dwType);

    BOOL l_bResult = FALSE;
    if(g_pOrigGetRomFileInfo)
    {
        CALLBACKINFO l_CallbackInfo;
        l_CallbackInfo.m_hDestinationProcessHandle = l_hCaller;
        l_CallbackInfo.m_pFunction = (FARPROC)g_pOrigGetRomFileInfo;
        l_CallbackInfo.m_pFirstArgument = (LPVOID)p_dwType;
    
        l_bResult = (BOOL)PerformCallBack4(&l_CallbackInfo, 
            (DWORD)p_pFindData, p_dwSize, 0);

        //Type 3 and 4 are special backdoors used by coredll to 
        //retrieve pointers to the system method tables.
        //We need to replace these system tables by our copy.
        //Also we know that coredll calls this routine when a 
        //new process starts, so it is a good opportunity to load 
        //the Spy dll into the new process.
        if(p_dwType == 3)
        {
            LPVOID * l_pRetVal = (LPVOID *)MapPtrToProcess
                ((LPVOID)p_pFindData, l_hCaller);
			
            HTRACE(TG_DebugSpyBrief, 
                _T("Replace SH_WIN32 methods %x by %x at %x ")
                _T("AllKMode %d\r\n"),
                *l_pRetVal, g_SpyEngine.m_ppHookWin32Methods, 
                l_pRetVal, *(BOOL*)p_dwSize);
            *l_pRetVal = g_SpyEngine.m_ppHookWin32Methods;
            HTRACE(TG_DebugSpyBrief, _T("Replaced\r\n"));

            DWORD l_dwIdx = GetProcessIndexFromID(l_hCaller);
            HTRACE(TG_InterceptedInfo, 
                _T("Process %x #%d started\r\n"),
                l_hCaller, l_dwIdx);
        }
        if(p_dwType == 4)
        {
            LPVOID * l_pRetVal = (LPVOID *)MapPtrToProcess
                ((LPVOID)p_pFindData, l_hCaller);
			
            HTRACE(TG_DebugSpyBrief, 
                _T("Replace Extra methods %x by %x at %x\r\n"),
                *l_pRetVal, 
                g_SpyEngine.m_ppHookExtraMethods, l_pRetVal);
            *l_pRetVal = g_SpyEngine.m_ppHookExtraMethods;
            HTRACE(TG_DebugSpyBrief, _T("Replaced\r\n"));

            //Pull ourselves to this new process
            DWORD l_dwIdx = GetProcessIndexFromID(l_hCaller);

            g_SpyEngine.m_hSpyDllLoaded[l_dwIdx] = 
                g_SpyEngine.LoadHookDllIntoProcess(l_hCaller);
        }
    }//if(g_pOrigGetRomFileInfo)
    
    HTRACE(TG_DebugSpyDetailed, 
        _T("HookGetRomFileInfo(%x,%x,%x) ret %x\r\n"),
        p_dwType, p_pFindData, p_dwSize, l_bResult);

    InterlockedDecrement(&g_SpyEngine.m_lNumHooksActive);
    return l_bResult;
}//BOOL HookGetRomFileInfo

/*-------------------------------------------------------------------
   FUNCTION: DoStart 
   PURPOSE: Spy engine startup routine. Called by StartSpy
    under an exception handler.
   RETURNS:
    TRUE on success, FALSE on failure
-------------------------------------------------------------------*/
BOOL SpyEngine::DoStart()
{
    m_dwOrigThreadId = GetCurrentThreadId();

    //Load toolhelp library and initialize pointers to it's functions
    if(!LoadToolHelp())
    {
        return FALSE;
    }

    //Use toolhelp to enumerate running processes and keep the info.
    InitProcessList();

    //Calculate the beginning of the current process's memory slot.
    //Use undocumented function GetProcessIndexFromID.
    //There is a confusion: name of the function suggests, that it 
    //expects process ID, but argument is declared as HANDLE.
    //In fact, CE process handles and IDs are identical.
    //Also rely on fact that each process's memory slot is 32 MB.
    //To get the base address shift 0x2000000 left by the process
    //index.
    //WARNING: using 3 undocumented features here.
    HANDLE hCurrent = GetCurrentProcess();
    m_dwCurrentProcBase = 0x2000000 * 
        (1 + GetProcessIndexFromID(hCurrent));

    //Now get access to the kernel data structures.
    m_pSystemAPISets = (CINFO**)(UserKInfo[KINX_APISETS]);
    DWORD dwMask = UserKInfo[KINX_API_MASK];

    HTRACE(TG_DebugSpyBrief, 
        _T("KernelInfo: UserKInfo = %x SystemAPISets = %x Mask =%x"),
        UserKInfo, m_pSystemAPISets, dwMask);

    //Need to keep a trap to the original LoadLibrary.
    m_pOriginalLoadLibrary = m_pSystemAPISets[SH_WIN32]->m_ppMethods[W32_LoadLibraryW];
    HTRACE(TG_DebugSpyBrief,_T("m_pOriginalLoadLibrary=0x%08x"), m_pOriginalLoadLibrary);
	//BETTER JUST USE THE DLL POINTER and do not look at mem, see LoadHookDllIntoProcess
	//void *t=GetProcAddress(GetModuleHandle(L"coredll.dll"),L"LoadLibraryW");

    //Allocate memory in the kernel (NK.EXE), which will be 
    //accessible from all processes. We will use this memory as 
    //argument to LoadLibrary to load the Spy DLL into all processes.
    m_pszSpyDllPathInKernelMemory = (LPTSTR)
        AllocateMemInKernelProc(_MAX_PATH * sizeof(TCHAR));
    
    //g_hInst is a instance of the Spy DLL.
    //Put the path of Spy DLL to the shared memory:
    GetModuleFileName(g_hInst, 
        m_pszSpyDllPathInKernelMemory, _MAX_PATH);

    //Finally, pull the Spy DLL into all running processes.
    LoadHookDllInAllProcesses();

    //Do the same as coredll itself is doing when each new process 
    //is loaded: get direct pointers to a table of Win32 methods
    //and extra methods.
    BOOL bKernelMode;
    GetRomFileInfo(3, (LPWIN32_FIND_DATA)&m_ppWin32Methods, 
        (DWORD)&bKernelMode);
    GetRomFileInfo(4, (LPWIN32_FIND_DATA)&m_ppExtraMethods, 
        NULL);

    //Substitute the system API by our own. 
    //Later custom interceptors may hook other APIs as well,
    //but we always need to intercept the SH_WIN32 because:
    //1. we must intercept W32_GetRomFileInfo
    //2. pointer to SH_WIN32 is cached by coredll and we need to
    //   replace this cached pointer.
    CreateDuplicateApi(SH_WIN32);

    //Also create a copy of special "Extra methods" table.
    //This table is kept in the kernel and a pointer to is is
    //cached by coredll. Coredll uses it as a backdoor to some
    //system services. We, obviously, need to substitute this 
    //pointer as well.
    CreateDuplicateMethodTable(SIZE_EXTRA_TABLE,
        (PFNVOID *)m_ppExtraMethods, NULL,
        &m_ppHookExtraMethods, 
        &m_pdwHookExtraSignatures);

    //Replace pointers to SH_WIN32 table and "Extra Methods" table,
    //which are cached by coredll, by our copies.
    if(!HookCoredll(m_ppWin32Methods, m_ppHookWin32Methods) ||
       !HookCoredll(m_ppExtraMethods, m_ppHookExtraMethods))
    {
        //Undo the hooking:
        HookCoredll(m_ppHookWin32Methods, m_ppWin32Methods);
        HookCoredll(m_ppHookExtraMethods, m_ppExtraMethods);
        return FALSE;
    }

    m_bCoredllHooked = TRUE;

    //Now create our own private API. We will use it to wrap a 
    //pointer to some interceptor routines in an API trap.
    m_iNumOurApiMethods = NUM_OUR_API_METHODS;
    int l_iSize = (sizeof(PFNVOID) + sizeof(DWORD)) * 
        m_iNumOurApiMethods;
    //Allocate a table of pointers to methods and signatures.
    m_ppOurApiMethods = (PFNVOID *)malloc(l_iSize);
    if(!m_ppOurApiMethods)
    {
        HTRACE(TG_Error,
          _T("ERROR: failed to allocate %d bytes for our API table"),
            l_iSize);
        return FALSE;
    }
    m_pdwOurApiSignatures = (DWORD*)
        (LPBYTE(m_ppOurApiMethods) + 
        sizeof(PFNVOID)*m_iNumOurApiMethods);
    //Initially fill all entries with a pointer to DummyRoutine,
    //which does nothing. Later this pointer will be replaced by 
    //an actual interceptor routine.
    for(int i = 0; i < m_iNumOurApiMethods; i++)
    {
        m_ppOurApiMethods[i] = DummyRoutine;
        m_pdwOurApiSignatures[i] = FNSIG0();
    }
    //Finally, register our API
    m_iHookAPISetID = CreateAndRegisterApi("Hook",
        m_iNumOurApiMethods, 
        m_ppOurApiMethods, 
        m_pdwOurApiSignatures,
        &m_hOurApiHandle);
    if(m_iHookAPISetID < 0)
    {
        free(m_ppOurApiMethods);
        m_ppOurApiMethods = NULL;
        return FALSE;
    }
    //Get a pointer to our private API stored in the system table.
    m_pHookAPISet = m_pSystemAPISets[m_iHookAPISetID];

    //GetRomFileInfo must always be hooked to substitute pointers to 
    //system method tables retrieved by coredll when a new process 
    //starts.
    PFNVOID l_pTrapToHookGetRomFileInfo = SetPrivateApiMethod((PFNVOID)HookGetRomFileInfo);
    g_pOrigGetRomFileInfo = HookMethod(SH_WIN32, W32_GetRomFileInfo, l_pTrapToHookGetRomFileInfo);


    //Install custom interceptors.
    InstallInterceptors();

    HTRACE(TG_PrintAlways, _T("StartSpy successful"));
    g_bStarted = TRUE;
    return TRUE;
}//BOOL SpyEngine::DoStart()

/*-------------------------------------------------------------------
   FUNCTION: DoStop 
   PURPOSE: Stop spy engine. Called by StopSpy 
    under an exception handler.
   RETURNS:
    TRUE on success, FALSE on failure
-------------------------------------------------------------------*/
BOOL SpyEngine::DoStop()
{
    HTRACE(TG_DebugSpyBrief, _T("Unhook hooked APIs"));
    int i;
    if(m_bCoredllHooked)
    {
        HTRACE(TG_DebugSpyBrief, _T("UnHook core DLL"));
        m_bCoredllHooked = FALSE;
        HookCoredll(m_ppHookWin32Methods, 
            m_ppWin32Methods);
        HookCoredll(m_ppHookExtraMethods, 
            m_ppExtraMethods);
    }
    for(i = 0; i < countof(m_HookedAPI); i++)
    {
        HookedAPI * l_pHookedAPI = m_HookedAPI + i;
        if(!l_pHookedAPI->m_bUsed || !l_pHookedAPI->m_bSwapped)
            continue;
        HTRACE(TG_DebugSpyDetailed, 
            _T("Restore old api set %x orig CINFO %x, ours %x"),
            l_pHookedAPI->m_iOrigApiSetId,
            l_pHookedAPI->m_pOrigApiSet,
            l_pHookedAPI->m_pOurApiSet);

        //Copy the original table into our (eliminating all hooks)
		PFNVOID * l_pOrigMethods = (PFNVOID *)ConvertAddr(
			l_pHookedAPI->m_pOrigApiSet->m_ppMethods, 
			l_pHookedAPI->m_pOrigApiSet->m_pProcessServer);

        for(int j = 0; j < 
            l_pHookedAPI->m_pOrigApiSet->m_wNumMethods; j++)
        {
            l_pHookedAPI->m_pOurApiSet->m_ppMethods[j] =
                l_pOrigMethods[j];
        }

        //Swap back
        m_pSystemAPISets[l_pHookedAPI->m_iOrigApiSetId] = 
            l_pHookedAPI->m_pOrigApiSet;
        l_pHookedAPI->m_bSwapped = FALSE;

        //Free duplicate tables and CINFO.
        //WARNING: There may be a chance that they are still used
        //by a dispatcher or Coredll on another thread.
        //So, if crashes will happen after Stop - delay or
        //remove this memory freeing
        HTRACE(TG_DebugSpyBrief, 
            _T("Free duplicate tables at %x and CINFO at %x"),
            l_pHookedAPI->m_pOurApiSet->m_ppMethods,
            l_pHookedAPI->m_pOurApiSet);

        FreeMemInKernelProc(l_pHookedAPI->m_pOurApiSet->m_ppMethods);
        FreeMemInKernelProc(l_pHookedAPI->m_pOurApiSet);
        l_pHookedAPI->m_pOurApiSet = NULL;
    
        HTRACE(TG_DebugSpyDetailed, 
            _T("Restored old api set %x orig CINFO %x, ours %x"),
            l_pHookedAPI->m_iOrigApiSetId,
            l_pHookedAPI->m_pOrigApiSet,
            l_pHookedAPI->m_pOurApiSet);
    }//for(i = 0; i < countof(m_HookedAPI); i++)
    if(m_hOurApiHandle)
    {
        CloseAPISet(m_hOurApiHandle);
        HTRACE(TG_DebugSpyDetailed, 
            _T("After closed \"Hook\" API %x"),m_hOurApiHandle);
        m_hOurApiHandle = NULL;
        free(m_ppOurApiMethods);
        m_ppOurApiMethods = NULL;
    }
    
    for(int l_iProcID = countof(m_hSpyDllLoaded)-1; 
        l_iProcID > 0; l_iProcID--)
    {//Skip NK.EXE
        PROCESSENTRY32 * l_pProcEntry = m_Processes + l_iProcID;
		if(l_pProcEntry->dwSize == 0)
			continue;
        HANDLE l_hProc = OpenProcess(0, FALSE, 
            l_pProcEntry->th32ProcessID);
        if(l_hProc == NULL)
        {
            HTRACE(TG_DebugSpyDetailed, 
                _T("WARNING: Failed to open process %x %s\r\n"),
                l_pProcEntry->th32ProcessID,l_pProcEntry->szExeFile);
            continue;
        }
        HTRACE(TG_DebugSpyDetailed, 
            _T("Unload SpyDll from proc %x %s"),
            l_pProcEntry->th32ProcessID,
            l_pProcEntry->szExeFile);

        UnloadHookDllInProcess(l_hProc, 
            m_hSpyDllLoaded[l_iProcID]);
        CloseHandle(l_hProc);
        m_hSpyDllLoaded[l_iProcID] = NULL;
    }
    return TRUE;
}

/*-------------------------------------------------------------------
   FUNCTION: StartSpy
   PURPOSE: Exported routine called by Spy GUI to start monitoring
   RETURNS:
    TRUE on success, FALSE on failure
-------------------------------------------------------------------*/
extern "C" __declspec(dllexport) 
BOOL StartSpy()
{
    if(g_bStarted)
    {
        HTRACE(TG_DebugSpyBrief, _T("Spy already started"));
        return TRUE;
    }
    HTRACE(TG_DebugSpyBrief, _T("->StartSpy"));
    BOOL l_bResult = TRUE;
    DWORD l_dwOldPermissions = 0;
    __try
    {
        //Switch to kernel mode to get access to kernel memory 
        //(needed only on CE 4.0)
        SetKMode(TRUE);

        //Get access to memory slots of other processes
        l_dwOldPermissions = SetProcPermissions(-1);

        //Init global data
        memset(&g_SpyEngine, 0, sizeof(g_SpyEngine));
        l_bResult = g_SpyEngine.DoStart();
    }
    __except(1)
    {
        HTRACE(TG_Error, _T("Exception in StartSpy()"));
        l_bResult = FALSE;
    }
    if(l_dwOldPermissions)
    {
        SetProcPermissions(l_dwOldPermissions);
    }
    SetKMode(FALSE);//Switch back to User mode(needed only on CE 4.0)
    HTRACE(TG_DebugSpyBrief, _T("<-StartSpy ret %d"), l_bResult);
    return l_bResult;
}//BOOL StartSpy()

/*-------------------------------------------------------------------
   FUNCTION: StopSpy
   PURPOSE: Exported routine called by Spy GUI to stop monitoring
   RETURNS:
    TRUE on success, FALSE on failure
-------------------------------------------------------------------*/
extern "C" __declspec(dllexport) 
BOOL StopSpy()
{
    HTRACE(TG_DebugSpyBrief, _T("->StopSpy"));
    DWORD l_dwOldPermissions = 0;
    BOOL l_bReturn = FALSE;
    __try
    {
        g_bStarted = FALSE;

        //Switch to kernel mode to get access to kernel memory 
        //(needed only on CE 4.0)
        SetKMode(TRUE);
        
        //Get access to memory slots of other processes
        l_dwOldPermissions = SetProcPermissions(-1);

        if(g_SpyEngine.DoStop())
        {
            l_bReturn = TRUE;
        }
    }
    __except(1)
    {
        HTRACE(TG_Error, _T("Exception in StopSpy"));
        l_bReturn = FALSE;
    }
    if(l_dwOldPermissions)
    {
        SetProcPermissions(l_dwOldPermissions);
    }
    //Switch back to User mode (needed only on CE 4.0)
    SetKMode(FALSE);

    HTRACE(TG_PrintAlways, 
        _T("StopSpy Ends. %d calls intercepted. ")
        _T("%d calls still in progress"), 
        g_SpyEngine.m_lNumCalls, g_SpyEngine.m_lNumHooksActive);
    if(g_SpyEngine.m_lNumHooksActive != 0)
    {
        l_bReturn = FALSE;
    }
    return l_bReturn;
}//BOOL StopSpy()

/*-------------------------------------------------------------------
   FUNCTION: DumpApis
   PURPOSE: DumpApis prints all registered APIs.
    DumpApis is not needed for api spying. 
    It is used for information only.
-------------------------------------------------------------------*/
extern "C" __declspec(dllexport) 
void DumpApis()
{
    HTRACE(TG_InterceptedInfo, _T("Dump APIs:"));
    DWORD l_dwOldPermissions = 0;
    __try
    {
        SetKMode(TRUE);
    
        //Get access to memory slots of other processes
        l_dwOldPermissions = SetProcPermissions(-1);

        CINFO ** l_pSystemAPISets = (CINFO**)(UserKInfo[KINX_APISETS]);
        for(int i = 0; i < NUM_SYSTEM_SETS; i++)
        {
            CINFO * l_pSet = l_pSystemAPISets[i];
            if(!l_pSet)
            {
                continue;
            }
            LPBYTE l_pServer = (LPBYTE)l_pSet->m_pProcessServer;
            HTRACE(TG_InterceptedInfo, 
                _T("#%x: %S disp %d type %d #meth %d ")
                _T("pMeth %x sig %x srv %x %s\n"),
                i,
                l_pSet->m_szApiName,
                l_pSet->m_byDispatchType,
                l_pSet->m_byApiHandle,
                l_pSet->m_wNumMethods,
                l_pSet->m_ppMethods,
                l_pSet->m_pdwMethodSignatures,
                l_pServer,
                l_pServer? (*(LPTSTR*)
                    (l_pServer + PROCESS_NAME_OFFSET)) : _T("") );

            //If this API is served by an application - get it's 
            //address, if it is served by the kernel - use address 0
            DWORD l_dwBaseAddress = 0;
            if(l_pServer)
            {
                l_dwBaseAddress = ProcessAddress(*(l_pServer + PROCESS_NUM_OFFSET));
            }
            //Add the base address to the method and signature 
            //tables pointers
            PFNVOID * l_ppMethods = l_pSet->m_ppMethods;
            if(l_ppMethods  && (DWORD)l_ppMethods < 0x2000000)
            {
                l_ppMethods = (PFNVOID *)((DWORD)l_ppMethods + l_dwBaseAddress);
            }
            DWORD * l_pdwMethodSignatures = l_pSet->m_pdwMethodSignatures;
            if(l_pdwMethodSignatures && 
                (DWORD)l_pdwMethodSignatures < 0x2000000)
            {
                l_pdwMethodSignatures = (DWORD *)((DWORD)l_pdwMethodSignatures + l_dwBaseAddress); 
            }
            if(l_ppMethods)
            {
                for(int j = 0; j < l_pSet->m_wNumMethods; j++)
                {
                    PFNVOID l_pMethod = l_ppMethods? 
                        l_ppMethods[j] : 0;
                    if(l_pMethod && (DWORD)l_pMethod < 0x2000000)
                    {
                        l_pMethod = (PFNVOID)
                            ((DWORD)l_pMethod + l_dwBaseAddress);
                    }
                    DWORD l_dwSign = l_pdwMethodSignatures? 
                        l_pdwMethodSignatures[j] : 0;
                    HTRACE(TG_InterceptedInfo, 
                        _T("meth #%x: %x sign %x\n"),
                        j,
                        l_pMethod,
                        l_dwSign);
                }
            }        
        }//for(int i = 0; i < NUM_SYSTEM_SETS; i++)
    }
    __except(1)
    {
        HTRACE(TG_Error, _T("Exception in DumpApis"));
    }
    SetKMode(FALSE);
    if(l_dwOldPermissions)
    {
        SetProcPermissions(l_dwOldPermissions);
    }
}//void DumpApis()

/*-------------------------------------------------------------------
   FUNCTION: DllMain
   PURPOSE: Regular DLL initialization point. Used to initialize
    per-process data structures, such as the tracing engine
-------------------------------------------------------------------*/
BOOL WINAPI DllMain(HANDLE hInstance, DWORD dwReason, 
                    LPVOID /*lpReserved*/)
{
    static long s_lLoadCount = 0;
    DWORD l_dwProcessId = GetCurrentProcessId();
    DWORD l_dwIdx = GetProcessIndexFromID((HANDLE)l_dwProcessId);
    PROCESSENTRY32 * l_pEntry = &g_SpyEngine.m_Processes[l_dwIdx];
    TCHAR l_szPath[_MAX_PATH];
    GetModuleFileName(NULL, l_szPath, _MAX_PATH);
	switch (dwReason)
	{
		case DLL_PROCESS_ATTACH:
        {
            g_hInst = (HINSTANCE)hInstance;
            if(InterlockedIncrement(&s_lLoadCount) == 1)
            {
                TraceInitialize();

                //Show errors and warnings on the
                //debugger's log window and in message boxes.
                TraceAssignGroupsToStream(
                    TO_DebugMonitor | TO_MessageBox,
                    TG_MessageBox | TG_Error, (DWORD)-1);

                //Print important messages to the debug monitor
                TraceAssignGroupsToStream(TO_DebugMonitor,
                    TG_PrintAlways, TG_PrintAlways);

                //Allocate a memory buffer and print 
                //intercepted trace to it
                TraceUseMemMapFileBuffer(s_szTraceMapFile, 
                    TRACE_SIZE,
                    TG_MessageBox | TG_Error | 
                    TG_InterceptedInfo | TG_PrintAlways, (DWORD)-1);

                //After we assigned default values, 
                //read current settings from registry
                TraceReadWriteSettings(HKEY_CURRENT_USER, s_szSettingsRegPath, false, TO_DebugMonitor | TO_MemoryBuffer);
				HTRACE(TG_DebugSpyBrief, _T("\xFEFF"));//Unicode BOM 
                HTRACE(TG_DebugSpyBrief, _T("Initialize trace"));
            }
            //Fill new process info. We only use name and ID
            memset(l_pEntry, 0, sizeof(PROCESSENTRY32));
            l_pEntry->dwSize = sizeof(PROCESSENTRY32);
            _tcscpy(l_pEntry->szExeFile, l_szPath);
            l_pEntry->th32ProcessID = l_dwProcessId;

            HTRACE(TG_DebugSpyBrief, 
                _T("SpyDLL DLL_PROCESS_ATTACH proc %x #%d %s %d"),
                l_dwProcessId, l_dwIdx, l_szPath, s_lLoadCount);
        }
        break;
        case DLL_PROCESS_DETACH:
        {
            HTRACE(TG_DebugSpyBrief, 
                _T("SpyDLL DLL_PROCESS_DETACH proc %x #%d %s %d"),
                l_dwProcessId, l_dwIdx, l_szPath, s_lLoadCount);

            g_SpyEngine.m_hSpyDllLoaded[l_dwIdx] = NULL;
            memset(l_pEntry, 0, sizeof(PROCESSENTRY32));//not used

            if(InterlockedDecrement(&s_lLoadCount) == 0)
            {
                HTRACE(TG_DebugSpyBrief, _T("Uninitialize trace"));
                TraceUnInitialize();
            }
        }
        break;
    }
    return TRUE;
}//BOOL WINAPI DllMain
