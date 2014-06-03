/********************************************************************
Module : HTrace.cpp  Implementation of tracing routines.
             Written 1998-2003 by Dmitri Leman
         for an article in C/C++ journal about a tracing framework.
Purpose: Tracing framework allows inserting HTRACE and HTRACEK
  tracing statements to applications and drivers.
  Tracing messages may redirected to various output streams 
  without rebuilding the whole application.
  To enable the trace, define TRACE_ in compiler settings.
  To compile for NT kernel mode driver, add TRACER_NTDRIVER 
  definition. To compile with Java JNI support, add TRACE_JNI.
  This file was compiled using Visual C++ 6.0 for WIN32 and NT
  kernel mode, 
  g++ version egcs-2.91 for Red Hat Linux 6.1  on Pentium
********************************************************************/
#if defined(TRACER_NTDRIVER)
extern "C"
{
#include "ntddk.h"
//If the compiler cannot find ntddk.h, 
//please download NT or 2000 DDK from www.microsoft.com/ddk
//and modify compiler settings to include ddk\inc directory

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#define HAVE_EXCEPTIONS
#define HAVE_FILE_OUTPUT
}
#elif defined(CE_KERNEL_DRV)

#include <windows.h>
#include "CeApiTraps.h"

#elif defined(_WIN32_WCE)

#include <windows.h>
#define WINAPP
#define HAVE_FILE_OUTPUT

#elif defined(WIN32)

#include <windows.h>
#include <stdio.h>
#include <conio.h>
#include <time.h>
#define WINAPP
#define HAVE_EXCEPTIONS
#define HAVE_FILE_OUTPUT

#elif defined(__linux__)

#if !defined(__i386__)
#error only Pentium supported
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unistd.h"
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sem.h>
#include <syslog.h>
#include <stdarg.h>
#define _vsnprintf vsnprintf

#define HAVE_FILE_OUTPUT

#else
#error "unknown platform"
#endif

#define TRACE_CPP
#include "HTrace.h"

#ifdef TRACE_JNI
#include "jni.h"
extern "C" void JNISetTotalMask
    (ULONG p_dwTotal, ULONG p_ulConsole);
#endif

#ifdef TRACE_ //{

const TCHAR g_szMemMapFileName[] = _T("TraceMemMapFile");
const int  g_iDefaultMemMapFileSize = 1024*1024;

#if !defined(_WIN32_WCE)
#pragma intrinsic(memset,memcpy,strlen,_tcscpy)
#endif

#if defined(__linux__)
long InterlockedExchangeAdd(long * p_plValue, long p_lAdd)
{
    long l_lResult;
    __asm__ __volatile__ (
    "push %%EBX;"    
    "movl %1, %%EBX;" 
    "movl %2, %%EAX;"
    "lock; xadd %%EAX, (%%EBX);"
    "inc %%EAX;"
    "mov %%EAX, %0;"
    "pop %%EBX"    
    : "=g"(l_lResult) : "g"(p_plValue), "g"(p_lAdd) );
    return l_lResult;
}

long InterlockedIncrement(long * p_plValue)
{
    long l_lResult;
    __asm__ __volatile__ (
    "push %%EBX;"    
    "movl %1, %%EBX;" 
    "movl $1, %%EAX;"
    "lock; xadd %%EAX, (%%EBX);"
    "inc %%EAX;"
    "mov %%EAX, %0;"
    "pop %%EBX"    
    : "=g"(l_lResult) : "g"(p_plValue) );
    return l_lResult;
}
long InterlockedDecrement(long * p_plValue)
{
    long l_lResult;
    __asm__ __volatile__ (
    "push %%EBX;"    
    "movl %1, %%EBX;" 
    "movl $-1, %%EAX;"
    "lock; xadd %%EAX, (%%EBX);"
    "inc %%EAX;"
    "mov %%EAX, %0;"
    "pop %%EBX"    
    : "=g"(l_lResult) : "g"(p_plValue) );
    return l_lResult;
}
static bool InterlockedExchangeAddUsingSemaphore(int p_iSemKey,
 int & p_riSemID, int   p_iAdd, int & p_riReturnPreviousValue);
#define __int64 long long
static ULONG GetTickCount()
{
    clock_t l_Clock = clock();
    HTRACE(16, "clock %d per sec %d", l_Clock, CLOCKS_PER_SEC);
    return (ULONG)(((__int64)l_Clock) * 1000 / CLOCKS_PER_SEC);
}
static void _itoa(int p_iValue, char * p_pStr, int p_iRadix)
{
    sprintf(p_pStr, (p_iRadix == 16)? "%x" : "%d", p_iValue);
}
#endif

#define countof(array) (sizeof(array)/sizeof(array[0]))

#if defined(_WIN32_WCE) && UNDER_CE < 400

long InterlockedExchangeAdd(long * p_plValue, long p_lAdd)
{
    while(1)
    {
        long lOld1 = *p_plValue;
        long lOld2 = InterlockedTestExchange
            (p_plValue, lOld1, lOld1 + p_lAdd);
        if(lOld1 == lOld2)
            return lOld1;
        //Some other thread interrupted us. Try again.
    }
}
#endif

#ifdef HAVE_FILE_OUTPUT //{
/*
class HTraceFileImpl is a wrapper around platform-specific 
routines to open,close, read and write a file
*/
class HTraceFileImpl
{
public:
    void Cleanup();
    
    ULONG OpenFile(LPCTSTR p_pszFileName, bool  p_bWrite,
        bool p_bPrependQuestionMarks,
        #if defined(TRACER_NTDRIVER)
            PUNICODE_STRING p_FileName
        #else
            void *
        #endif
        );

    ULONG CloseFile();

    bool  IsOpened();

    ULONG GetFileSize(OUT PLARGE_INTEGER  p_pReturnSize);

    ULONG ReadWriteFile(IN bool p_bWrite, 
        OUT     void *  p_lpBuffer,
        IN      ULONG   p_dwNumberOfBytes,
        IN  OUT PLARGE_INTEGER p_pByteOffset,
        OUT     ULONG*  p_pdwNumberOfBytesProcessed);

    ULONG FlushBuffers();
protected:
    HANDLE  m_hFileHandle;
};//class HTraceFileImpl

/*
class HTraceFileLocal is useful as a local object in functions,
which need to open a file and close it before exiting.
*/
class HTraceFileLocal : public HTraceFileImpl
{
public:
    HTraceFileLocal(){Cleanup();}
    ~HTraceFileLocal() {CloseFile();}
};//class HTraceFileLocal : public HTraceFileImpl

#endif //#ifdef HAVE_FILE_OUTPUT }

//T_TraceOutputStream is definition for trace output routines.
typedef bool T_TraceOutputStream
    (ULONG p_dwMask, LPCTSTR p_pszMessage, int p_iLenChars);
//TraceOutput keeps necessary data for a trace output stream
struct TraceOutput
{
    ULONG   m_dwOutputID;
    TCHAR * m_pszName;
    ULONG   m_dwEnabledGroups;
    char    m_szEnabledGroupsKeywords[256];
    ULONG   m_dwKeyWordModificationCounter;// equal to
    //s_dwKeyWordModifCounter at modification time
    T_TraceOutputStream * m_pOutputFunction;
};

//The following are various trace output functions.
//To add a new method of trace output just add a new constant 
//to enum TraceOutputs in htrace.h, add a new routine here of
//type T_TraceOutputStream and add new line in s_Outputs array
bool OutputDebugMonitor
    (ULONG p_dwMask, LPCTSTR p_pszMessage, int p_iLenChars);
bool OutputFile
    (ULONG p_dwMask, LPCTSTR p_pszMessage, int p_iLenChars);
bool AddToTraceBuffer
    (ULONG p_dwMask, LPCTSTR p_pszBuff, int p_iLenChars);

#if defined(TRACER_NTDRIVER)
bool AddToOutputLog
    (ULONG p_dwMask, LPCTSTR p_pszString, int p_iLenChars);
#else
bool TraceMessageBox
    (ULONG p_dwMask, LPCTSTR p_pszMessage,int p_iLenChars);
#if !defined(_WIN32_WCE)
bool OutputConsole
    (ULONG p_dwMask, LPCTSTR p_pszBuff, int p_iLenChars);
#endif
#endif

TraceOutput s_Outputs[] = {
{TO_DebugMonitor, _T("DebugMonitor"),0, "",-1, OutputDebugMonitor},
{TO_File        , _T("File")        ,0, "",-1, OutputFile        },
{TO_MemoryBuffer, _T("MemoryBuffer"),0, "",-1, AddToTraceBuffer  },
#if defined(TRACER_NTDRIVER)
{TO_OutputLog   , _T("OutputLog")   ,0, "",-1, AddToOutputLog    },
#else
{TO_MessageBox  , _T("MessageBox")  ,0, "",-1, TraceMessageBox   },
#if !defined(_WIN32_WCE)
{TO_Console     , _T("Console")     ,0, "",-1, OutputConsole     },
#endif
#endif
};//TraceOutput s_Outputs[]
#define NUM_STREAMS (countof(s_Outputs))
ULONG s_dwKeyWordModifCounter = 0;

static bool      s_bInitialized = false;
//struct TraceImpl is a holder for various trace settings, 
//current pointers to memory buffer, etc. Only one static 
//instance will be created called s_Impl.
struct TraceImpl
{
    void Clean()
    {
        m_BufferPointers.m_pGlobalHeader = NULL;
        m_BufferPointers.m_pTextArea = NULL;
        m_BufferPointers.m_dwTextAreaSize = 0;
        m_BufferPointers.m_pGlobalFooter = NULL;
        m_bBufferAllocated = false;

        #if defined(WINAPP)
        m_hFileMapping = NULL;
        m_bBufferMapped = false;

        m_hDeviceWithBuffer = INVALID_HANDLE_VALUE;
        m_bMappedDriverBuffer = false;
        m_iIOCTLToUnmapDriverBuffer = 0;
        #endif
        #if defined(__linux__)
        m_iMemMapFile = -1;
        m_iMappingSize = 0;
        m_iSemKey = *((long*)"HTMF");
        m_iSemID = 0;
        m_bBufferMapped = false;
        #endif
        
        #ifdef TRACER_NTDRIVER
        m_pDriverObject = NULL;
        #else
        #endif
        m_dwFlushFileMask         = 0;
        m_pdwTotalMask = &m_dwTotalMask;
        m_dwTotalMask = TG_Error;
        m_dwConsoleMask = 0;
        m_dwDebugMask = 0;

#ifdef HAVE_FILE_OUTPUT
        m_File.Cleanup();
        *m_szFileName = 0;
#endif

        m_bAddTime = false;
        m_bAddThread = false;
        s_bInitialized = false;
    }
    void Free()
    {
#ifdef HAVE_FILE_OUTPUT
        m_File.CloseFile();
#endif
        TraceFreeBuffer();
        Clean();
    }

    bool TraceAssignGroupsToStream
    (
        ULONG   p_dwTraceOutputs,
        ULONG   p_dwNewGroupFlags,
        ULONG   p_dwGroupFlagsToModify,
        char  * p_pszGroupKeyWord
    );

    bool TraceAllocateBuffer(int p_iSize);
    bool TraceSetExternalBuffer
        (GlobalTraceBufferHeader * p_pBuffer);
    void AssignTraceBufferPtr
        (GlobalTraceBufferHeader * p_pBuffer)
    {
        m_BufferPointers.m_dwTextAreaSize = (ULONG)
            ReadSize(p_pBuffer->m_cSizeTextArea);
        m_BufferPointers.m_pTextArea = (LPTSTR)(p_pBuffer+1);
        int l_iFooterAddr = (int)
            (((int)m_BufferPointers.m_pTextArea) + 
             m_BufferPointers.m_dwTextAreaSize);
        //Align on 4 byte boundary
        l_iFooterAddr += 3;
        l_iFooterAddr &= ~3;
        m_BufferPointers.m_pGlobalFooter = 
            (GlobalTraceBufferFooter *)l_iFooterAddr;
        m_BufferPointers.m_pGlobalHeader = p_pBuffer;
        
        //Start using shared mask:
        m_pdwTotalMask = &m_BufferPointers.m_pGlobalFooter->
            m_dwEnabledGroupsTotal;
    }
    static void SetTextAreaSizeHex
        (char p_pszString[12], int p_iSize)
    {
        p_pszString[0] = ' ';
        p_pszString[11] = '\n';
        for(int i = 0; i < 10; i++)
        {
            p_pszString[10-i] = (p_iSize % 10) + '0';
            p_iSize /= 10;
        }
    }

    /*
    ReadSize is a simplified version of atoi, which is only used 
    to read the size of the trace buffer stored in it's header.
    It is always positive decimal number. We don't use a regular 
    atoi, because it may not be available on some platforms.
    */
    static int ReadSize(char * p_pszString)
    {
	    int l_iNumber = 0;
        if(*p_pszString == ' ')
            p_pszString++;
	    while(1)
        {
            char c = *(p_pszString++);
            if(c < '0' || c > '9')
                break;
		    l_iNumber = 10 * l_iNumber + (c - '0');
	    }
        return l_iNumber;
    }


    #if defined(WINAPP)
    bool CreateAndMapFile(LPCTSTR  p_pszMemMapFilePath, 
        int     p_iFileSize);
    bool TraceAttachToNTDriverBuffer(LPCTSTR  p_pszDeviceName,
      int p_iIOCTLMapTraceBuffer, int p_iIOCTLUnMapTraceBuffer,
        bool    p_bDontComplainIfDeviceAbsent);
    #endif
    #if defined(__linux__)
    bool CreateAndMapFile(const char *  p_pszMemMapFilePath, 
        int     p_iFileSize);
    #endif

    bool TraceFreeBuffer();
    bool TraceDumpBufferToFile(LPCTSTR p_pszFileName);

    LocalTraceBufferPointers m_BufferPointers;
    bool                m_bBufferAllocated;

    #if defined(WINAPP)
    HANDLE              m_hFileMapping;
    bool                m_bBufferMapped;

    HANDLE              m_hDeviceWithBuffer;
    bool                m_bMappedDriverBuffer;
    int                 m_iIOCTLToUnmapDriverBuffer;
    #endif
    #if defined(__linux__)
    int                 m_iMemMapFile;
    int                 m_iMappingSize;
    int                 m_iSemKey;
    int                 m_iSemID;
    bool                m_bBufferMapped;
    #endif
    
    #ifdef TRACER_NTDRIVER
    PDRIVER_OBJECT m_pDriverObject;
    FAST_MUTEX     m_FastMutexToProtectMemMap;
    #else
    #endif
    ULONG m_dwFlushFileMask;

    ULONG m_dwTotalMask;
    ULONG*m_pdwTotalMask;
    ULONG m_dwConsoleMask;
    ULONG m_dwDebugMask;

#ifdef HAVE_FILE_OUTPUT
    HTraceFileImpl m_File;
    TCHAR           m_szFileName[_MAX_PATH];
#endif
    
    bool            m_bAddTime;
    bool            m_bAddThread;
};//struct TraceImpl

static TraceImpl s_Impl;
static long      s_lChangeLock = -1;
/*
Current implementation does not allow simultaneous modification
of trace parameters by multiple threads. Ideally, a process 
should initialize trace from the beginning and, may be, 
update trace parameters from a single thread.
Functions, which generate trace, such as HTraceImpl and 
OutputTraceString can be called by any thread at any time(after
the trace is initialized by TraceInitialize.
*/

//We will use s_lChangeLock and class ChangeLock
//to protect all functions, which can modify trace settings,
//from simultaneous calls from several threads.
class ChangeLock
{
public:
    ChangeLock()
    {
        m_lChangeLock = InterlockedIncrement(&s_lChangeLock);
    }
    ~ChangeLock()
    {
        InterlockedDecrement(&s_lChangeLock);
    }
    bool isFirst() 
    {
        return (m_lChangeLock == 0)? true : false;
    }
    long m_lChangeLock;
};//class ChangeLock

/*-------------------------------------------------------------
   FUNCTION: TraceInitialize
   PURPOSE:  Initialize the trace. Must be called before any 
    other trace routine. We cannot use constructor in class 
    TraceImpl, because constructors in static classes require 
    presence of a C++ library, which is not always available.
-------------------------------------------------------------*/
bool TraceInitialize()
{
    ChangeLock l_ChangeLock;
    if(!l_ChangeLock.isFirst())
        return false;
    if(s_bInitialized)
        return true;
    s_Impl.Clean();
    s_bInitialized = true;
    #ifdef TRACER_NTDRIVER
    ExInitializeFastMutex(&s_Impl.m_FastMutexToProtectMemMap);
    #endif
    return true;
}

/*-------------------------------------------------------------
   FUNCTION: TraceUnInitialize
   PURPOSE:  Uninitialize the trace. Must be called before 
    application or driver terminates. We cannot use destructor 
    in class TraceImpl, because constructors in static classes 
    require presence of C++ library, which may not be available
-------------------------------------------------------------*/
bool TraceUnInitialize()
{
    ChangeLock l_ChangeLock;
    if(!l_ChangeLock.isFirst())
        return false;
    s_Impl.Free();
    return true;
}

#ifndef TRACER_NTDRIVER
#ifdef  TRACE_ADD_DLLMAIN
/*-------------------------------------------------------------
   FUNCTION: DllMain
   PURPOSE:  If the HTrace.cpp is built into a stand-alone DLL,
    such as HTrcJni.DLL, we will use standard Win32 DllMain to
    initialize and uninitialize the trace.
-------------------------------------------------------------*/
BOOL WINAPI DllMain
(
    HINSTANCE   p_hInstDLL, 
    DWORD       p_dwReason, 
    LPVOID      p_lpvReserved
)
{
    switch (p_dwReason)
    {
        case DLL_PROCESS_ATTACH:
        {
            TraceInitialize();

            //By default, show errors and warnings on the
            //debugger's log window and in message boxes.
            TraceAssignGroupsToStream(
                TO_DebugMonitor | TO_MessageBox,
                TG_MessageBox | TG_Error, (DWORD)-1);
    
            //Print bank account and thread swaps(if the 
            //driver is loaded) to the memory buffer
            TraceAssignGroupsToStream(TO_MemoryBuffer,
                TG_BankAccount | TG_ThreadSwaps, (DWORD)-1);

            char l_szFullINIPath[_MAX_PATH];
            GetFullPathName("HTrace.ini", 
                sizeof(l_szFullINIPath), l_szFullINIPath,NULL);

            //Read state of all streams. This can override the 
            //assignments made in the preceding calls.
            TraceReadWriteSettings(NULL, l_szFullINIPath,
                false, (DWORD)-1);
        }
        break;
        case DLL_PROCESS_DETACH:
            TraceUnInitialize();
        break;
    }
    return TRUE;
}
#endif//  TRACE_ADD_DLLMAIN
#endif

void SetOneTraceMask
(
    PULONG   p_pdwMask,
    ULONG    p_dwNewGroupFlags,
    ULONG    p_dwGroupFlagsToModify
)
{
    *p_pdwMask =  (*p_pdwMask  & ~p_dwGroupFlagsToModify) |
              (p_dwNewGroupFlags &  p_dwGroupFlagsToModify);
}

/*-------------------------------------------------------------

   FUNCTION: TraceImpl::TraceAssignGroupsToStream

   PURPOSE:  This function allows to enable/disable output
    of the specified groups of HTRACE operators to
    the specified output stream.
    Each HTRACE operator has a bit mask as a first parameter.
    This mask will be matched against a mask for a particular
    device to decide whether to allow output or not.
      
   PARAMETERS:                   .
    
   ULONG    p_dwTraceOutputs       flags from TraceOutputs.
   ULONG    p_dwNewGroupFlags      new bits to set or reset
   ULONG    p_dwGroupFlagsToModify which bits can be modified
   char  *  p_pszGroupKeyWord      new keyword mask
-------------------------------------------------------------*/
bool TraceImpl::TraceAssignGroupsToStream
(
    ULONG   p_dwTraceOutputs,
    ULONG   p_dwNewGroupFlags,
    ULONG   p_dwGroupFlagsToModify,
    char  * p_pszGroupKeyWord
)
{
    m_dwTotalMask = 0;
    m_dwConsoleMask = 0;
    m_dwDebugMask = 0;
    //Loop for all available output streams
    for(int i = 0; i < NUM_STREAMS; i++)
    {
        TraceOutput & l_rOutput = s_Outputs[i];
        //Check whether we are asked to modify this stream
        if(l_rOutput.m_dwOutputID & p_dwTraceOutputs)
        {
            //Update bits in the group mask of this stream
            SetOneTraceMask(&l_rOutput.m_dwEnabledGroups,
                p_dwNewGroupFlags, p_dwGroupFlagsToModify);
            if(p_pszGroupKeyWord)
            {
                //We are asked to update the stream keyword
                int l_iLen = strlen(p_pszGroupKeyWord);
                if(l_iLen >= 
                   sizeof(l_rOutput.m_szEnabledGroupsKeywords))
                {
                    HTRACE(TG_Error, 
                      _T("ERROR: not enough space for keyword %s"),
                        p_pszGroupKeyWord);
                }
                else if(memcmp(
                        l_rOutput.m_szEnabledGroupsKeywords,
                        p_pszGroupKeyWord, l_iLen+1))
                {//need to change the keyword
                    memcpy(l_rOutput.m_szEnabledGroupsKeywords,
                        p_pszGroupKeyWord, l_iLen+1);
                    //Increment modification counter, 
                    //store its value to force all HTRACEK 
                    //macros to check whether they match the
                    //new keyword on the next call.
                    l_rOutput.m_dwKeyWordModificationCounter = 
                        ++s_dwKeyWordModifCounter;
                }
            }

            if(l_rOutput.m_dwOutputID == TO_MemoryBuffer)
            {
                //Some additional processing for memory buffer
                if(l_rOutput.m_dwEnabledGroups ||
                  *l_rOutput.m_szEnabledGroupsKeywords)
                {
                    #if defined(WINAPP)
                    //If we have no buffer,try first to connect
                    //to our driver and if not successful, try
                    //to map to a mem-map file
                    if(!m_BufferPointers.m_pGlobalHeader)
                    {
                        TraceAttachToNTDriverBuffer(
                            NULL, -1, -1, true);
                    }
                    #endif
                    #if defined(WINAPP) || defined(__linux__)
                    if(!m_BufferPointers.m_pGlobalHeader)
                    {
                        CreateAndMapFile(NULL, -1);
                    }
                    #endif
                }
                if(m_BufferPointers.m_pGlobalFooter != NULL)
                {
                    //If the buffer is present, 
                    //copy the mask to it, so it will 
                    //immediately affect other applications,
                    //which share the buffer with us.
                    GlobalTraceBufferFooter * l_Footer = 
                        m_BufferPointers.m_pGlobalFooter;
                    l_Footer->m_dwEnabledGroups = 
                        l_rOutput.m_dwEnabledGroups;
                    strcpy(l_Footer->m_szKeyWordMask,
                        l_rOutput.m_szEnabledGroupsKeywords);
                    l_Footer->m_dwKeyWordModificationCounter = 
                       l_rOutput.m_dwKeyWordModificationCounter;
                    //Start using shared mask:
                    m_pdwTotalMask = 
                        &l_Footer->m_dwEnabledGroupsTotal;
                }
            }//if(l_rOutput.m_dwOutputID == TO_MemoryBuffer)

            #ifndef CE_KERNEL_DRV
            if(l_rOutput.m_dwOutputID == TO_File &&
               (l_rOutput.m_dwEnabledGroups ||
               *l_rOutput.m_szEnabledGroupsKeywords) &&
               !m_File.IsOpened())
            {//We need to open the file
                if(m_File.OpenFile
                    (m_szFileName, true, true, NULL))
                {
                    //Cannot open the file. Need to 
                    //disable the trace
                    l_rOutput.m_dwEnabledGroups = 0;
                    *l_rOutput.m_szEnabledGroupsKeywords = 0;
                }
            }
            #endif
            #if defined(WINAPP) && !defined(_WIN32_WCE)
            if(l_rOutput.m_dwOutputID == TO_Console  &&
              (l_rOutput.m_dwEnabledGroups ||
              *l_rOutput.m_szEnabledGroupsKeywords))
            {
                AllocConsole();
            }
            #endif
        }//if(l_rOutput.m_dwOutputID & p_dwTraceOutputs)
        m_dwTotalMask |= l_rOutput.m_dwEnabledGroups;
        if(l_rOutput.m_dwOutputID == TO_Console)
        {   //Keep the console mask separately for Java sake.
            m_dwConsoleMask = l_rOutput.m_dwEnabledGroups;
        }
        if(l_rOutput.m_dwOutputID == TO_DebugMonitor)
        {
            m_dwDebugMask = l_rOutput.m_dwEnabledGroups;
        }
    }//for(int i = 0; i < NUM_STREAMS;

    //Treat TO_FlushFile separately, because it is not a 
    //separate output stream, but modification of TO_File
    if(p_dwTraceOutputs  & TO_FlushFile)
    {
        SetOneTraceMask(&m_dwFlushFileMask,
            p_dwNewGroupFlags, p_dwGroupFlagsToModify);
    }
    *m_pdwTotalMask = m_dwTotalMask;
    #ifdef TRACE_JNI
    //Inform Java class about new masks
    JNISetTotalMask(*m_pdwTotalMask, m_dwConsoleMask);
    #endif
    return true;
}//bool TraceImpl::TraceAssignGroupsToStream

/*-------------------------------------------------------------
   FUNCTION: TraceAssignGroupsToStream
   PURPOSE:  The same as TraceImpl::TraceAssignGroupsToStream.
    It accepts only integer group bitmap.
    Use TraceAssignGroupKeyWordToStream to change keyword masks
    Increment lock counter to prevent multiple threads from 
    simultaneous access.
-------------------------------------------------------------*/
bool TraceAssignGroupsToStream
(
    ULONG   p_dwTraceOutputs,
    ULONG   p_dwNewGroupFlags,
    ULONG   p_dwGroupFlagsToModify
)
{
    ChangeLock l_ChangeLock;
    if(!l_ChangeLock.isFirst())
        return false;
    return s_Impl.TraceAssignGroupsToStream(p_dwTraceOutputs, 
            p_dwNewGroupFlags, p_dwGroupFlagsToModify, NULL);
}//bool TraceAssignGroupsToStream

/*-------------------------------------------------------------
   FUNCTION: TraceAssignGroupKeyWordToStream
   PURPOSE:  The same as TraceImpl::TraceAssignGroupsToStream.
    It accepts only string keyword masks.
    Use TraceAssignGroupsToStream to change integer group mask.
    Increment lock counter to prevent multiple threads from 
    simultaneous access.
-------------------------------------------------------------*/
bool TraceAssignGroupKeyWordToStream
(
    ULONG   p_dwTraceOutputs,
    char  * p_pszGroupKeyWord
)
{
    ChangeLock l_ChangeLock;
    if(!l_ChangeLock.isFirst())
        return false;
    return s_Impl.TraceAssignGroupsToStream(p_dwTraceOutputs, 
            0, 0, p_pszGroupKeyWord);
}//bool TraceAssignGroupsToStream

/*-------------------------------------------------------------
   FUNCTION: TraceGetAssignedGroupsToStream
   PURPOSE:  Return all group flags, which are enabled for the 
    specified output stream(s)
-------------------------------------------------------------*/
ULONG TraceGetAssignedGroupsToStream
(
    ULONG   p_dwTraceOutputs
)
{
    ULONG l_ulResult = 0;
    for(int i = 0; i < NUM_STREAMS;i++)
    {
        TraceOutput & l_rOutput = s_Outputs[i];
        if(l_rOutput.m_dwOutputID & p_dwTraceOutputs)
        {
            l_ulResult |= l_rOutput.m_dwEnabledGroups;
        }
    }
    if(p_dwTraceOutputs  & TO_FlushFile)
    {
        l_ulResult |= s_Impl.m_dwFlushFileMask;
    }
    return l_ulResult;
}//ULONG TraceGetAssignedGroupsToStream

/*-------------------------------------------------------------
   FUNCTION: TraceGetAssignedGroupKeyWordsToStream
   PURPOSE:  Return keyword mask, which is assigned to the 
    specified output stream
-------------------------------------------------------------*/
LPCSTR TraceGetAssignedGroupKeyWordsToStream
(
    ULONG   p_dwTraceOutput
)
{
    ULONG l_ulResult = 0;
    for(int i = 0; i < NUM_STREAMS;i++)
    {
        TraceOutput & l_rOutput = s_Outputs[i];
        if(l_rOutput.m_dwOutputID == p_dwTraceOutput)
        {
            return l_rOutput.m_szEnabledGroupsKeywords;
        }
    }
    return NULL;
}//LPCSTR TraceGetAssignedGroupKeyWordsToStream

void SetTraceTimeAndThreadID
(
    bool            p_bAddTime,
    bool            p_bAddThread
)
{
    s_Impl.m_bAddTime = p_bAddTime;
    s_Impl.m_bAddThread = p_bAddThread;
}

#if defined(WINAPP) //{

/*-------------------------------------------------------------
   FUNCTION: ReadWriteSetting
   PURPOSE:  Read or write a single value (string or DWORD)
     from/to a registry value or an INI file field.
     Used by TraceReadWriteSettings.
   PARAMETERS:                   .
    BOOL    p_bRegistry - TRUE for registry, FALSE - for INI
    BOOL    p_bWrite    - TRUE to write, FALSE - read
    BOOL    p_bString   - TRUE for string, FALSE - DWORD
    HKEY    p_hKey      - registry key. Used only if 
        p_bRegistry == TRUE. May be NULL for Read operation -
        in that case 0 or an empty string will be returned.
    LPCTSTR p_pszPath   - INI file path. Used if 
        p_bRegistry == FALSE.
    LPCTSTR p_pszEntryName - registry entry or INI field.
    LPVOID  p_pExistingValue - pointer to the value to write.
        Used only if p_bWrite == TRUE. Must not be NULL.
    LPVOID  p_pReadValue   - pointer to the value to read
        Used only if p_bWrite == FALSE. Must not be NULL.
    DWORD   p_dwSizeBytes  - size of the string (in bytes,
        not characters) or sizeof(DWORD)
   
   RETURN:
      TRUE if write successful
      FALSE if write failed, if read failed (including
        missing entry) or if the string does not fit the buffer.
-------------------------------------------------------------*/
BOOL ReadWriteSetting
(
    BOOL    p_bRegistry,
    BOOL    p_bWrite, 
    BOOL    p_bString,
    HKEY    p_hKey,
    LPCTSTR p_pszPath,
    LPCTSTR p_pszEntryName,
    LPVOID  p_pExistingValue,
    LPVOID  p_pReadValue,
    DWORD   p_dwSizeBytes
)
{
    BOOL l_bResult = FALSE;
    TCHAR l_szEntryValue[512], * l_pszReadTo;
    DWORD l_dwReadSize;
    if(p_bRegistry)
    {
        if(!p_bWrite)
        {//Read string directly to user supplied buffer
            l_pszReadTo = (LPTSTR)p_pReadValue;
            l_dwReadSize = p_dwSizeBytes;
        }
        else
        {
            l_pszReadTo = l_szEntryValue;
            l_dwReadSize = sizeof(l_szEntryValue);
        }
        DWORD l_dwType = 0;
        long l_lResult = RegQueryValueEx(p_hKey, p_pszEntryName, 0, 
            &l_dwType, (LPBYTE)l_pszReadTo, &l_dwReadSize);
        if(l_lResult == ERROR_SUCCESS)
        {
            l_bResult = TRUE;
        }
        else
        {
            if(p_bString)
            {
                *(LPTSTR)l_pszReadTo = 0;
                l_dwReadSize = sizeof(TCHAR);
            }
            else
            {
                *(LPDWORD)l_pszReadTo = 0;
            }
        }
        if(p_bWrite)
        {
            l_bResult = FALSE;
            if(p_bString)
            {
                p_dwSizeBytes = (_tcslen
                    ((LPTSTR)p_pExistingValue)+1)*sizeof(TCHAR);
            }
            if(l_dwReadSize != p_dwSizeBytes ||
                memcmp(p_pExistingValue, l_pszReadTo, p_dwSizeBytes))
            {
                long l_lResult = RegSetValueEx(p_hKey, 
                    p_pszEntryName, 0, p_bString? REG_SZ : REG_DWORD,
                    (LPBYTE)p_pExistingValue, p_dwSizeBytes);
                if(l_lResult != ERROR_SUCCESS)
                {
                    HTRACE(TG_Error, 
                        _T("ERROR: RegSetValueEx(%s)")
                        _T(" failed - %s"), 
                        p_pszEntryName, ERR_EXPL(l_lResult));
                }
                else
                {
                    l_bResult = TRUE;
                }
            }
        }
    }
    #if !defined(_WIN32_WCE)
    else
    {
        DWORD l_dwReadBuffNumChars;
        if(!p_bWrite && p_bString)
        {//Read string directly to user supplied buffer
            l_pszReadTo = (LPTSTR)p_pReadValue;
            l_dwReadBuffNumChars = p_dwSizeBytes/sizeof(TCHAR);
        }
        else
        {
            l_pszReadTo = l_szEntryValue;
            l_dwReadBuffNumChars = countof(l_szEntryValue);
        }
        GetPrivateProfileString("Trace", p_pszEntryName, "~", 
            l_pszReadTo, l_dwReadBuffNumChars, p_pszPath);
        if(p_bString)
        {
            if(p_bWrite)
            {
                if(_tcscmp(l_szEntryValue, (LPTSTR)p_pExistingValue))
                {
                    WritePrivateProfileString("Trace", l_szEntryName,
                        (LPTSTR)p_pExistingValue, p_pszPath);
                }
                l_bResult = TRUE;
            }
            else
            {
                if(*p_pReadValue != '~')
                {
                    l_bResult = TRUE;
                }
            }
        }
        else
        {
            DWORD l_dwCurValue = 0;
            DWORD l_dwNewValue = *(LPDWORD)p_pExistingValue;
            if(*l_szEntryValue != '~')
            {
                l_dwCurValue = strtoul(l_szEntryValue, NULL, 0);
                l_bResult = TRUE;
            }
            if(p_bWrite)
            {
                if(l_dwNewValue != l_dwCurValue)
                {
                    sprintf(l_szEntryValue, "0x%x", l_dwNewValue);
                    WritePrivateProfileString("Trace",p_pszEntryName,
                        l_szEntryValue, p_pszPath);
                }
                l_bResult = TRUE;
            }
            else
            {
                *(LPDWORD)p_pReadValue = l_dwCurValue;
            }
        }
    }
    #endif//!_WIN32_WCE
    if(!p_bWrite && !l_bResult)
    {//Set default
        if(p_bString)
        {
            *(LPTSTR)p_pReadValue = 0;
        }
        else
        {
            *(LPDWORD)p_pReadValue = 0;
        }
    }
    return l_bResult;
}//BOOL ReadWriteSetting

/*-------------------------------------------------------------
   FUNCTION: TraceReadWriteSettings
   PURPOSE:  Read or write group masks and keywords for some or
    all output streams to/from either registry or .INI file.
   PARAMETERS:                   .
    HKEY    p_hKeyRoot            root registry key (for example
        HKEY_CURRENT_USER). May be NULL - in that case p_pszPath
        should be a path to .INI file (if platform supports it).
    LPCSTR  p_pszPath    if p_hKeyRoot != NULL - registry path
                         if p_hKeyRoot == NULL - INI file path
    bool    p_bWrite              true to write, false to read
    ULONG   p_ulOutputsToProcess  flags from TraceOutputs - 
        chose which stream settings to read/write
-------------------------------------------------------------*/
bool TraceReadWriteSettings
(
    HKEY    p_hKeyRoot,
    LPCTSTR p_pszPath,
    bool    p_bWrite,
    ULONG   p_ulOutputsToProcess
)
{
#if defined(_WIN32_WCE)
    if(p_hKeyRoot == NULL)
        return false;
#endif
    ChangeLock l_ChangeLock;
    if(!l_ChangeLock.isFirst())
        return false;

    HKEY l_hKey = NULL;
    LRESULT l_lResult;
    BOOL l_bRegistry = FALSE;
    if(p_hKeyRoot)
    {
        l_bRegistry = TRUE;
        if(p_bWrite)
        {
            l_lResult = RegCreateKeyEx(p_hKeyRoot, p_pszPath, 
                0, NULL, 0, NULL, NULL, &l_hKey, NULL);
            if(l_lResult != ERROR_SUCCESS)
            {
                HTRACE(TG_Error, 
                    _T("ERROR: RegCreateKeyEx(%s)")
                    _T(" failed - %s"), 
                    p_pszPath, ERR_EXPL(l_lResult));
                return false;
            }
        }
        else
        {
            l_lResult = RegOpenKeyEx(p_hKeyRoot, p_pszPath, 
                0, NULL, &l_hKey);
            //Don't report this as an error since the key 
            //may be absent until we create it later.
            //Also don't return on error - continue and 
            //use defaults instead of registry values.
        }
    }

    ULONG l_ulResult = 0;
    for(int i = 0; i < NUM_STREAMS;i++)
    {
        TraceOutput & l_rOutput = s_Outputs[i];
        if(!(l_rOutput.m_dwOutputID & p_ulOutputsToProcess))
            continue;

        TCHAR l_szEntryName[512];
        DWORD l_dwNewGroupMask = l_rOutput.m_dwEnabledGroups;
        DWORD l_dwModifyGroupMask = 0;
        TCHAR l_szKeywordMask[256] = _T("");
        char *l_pszNewKeywordMask = NULL;

        _stprintf(l_szEntryName, _T("%s_GroupMask"), 
            l_rOutput.m_pszName);
        if(ReadWriteSetting(l_bRegistry, p_bWrite, FALSE, 
            l_hKey, p_pszPath, l_szEntryName, 
            &l_rOutput.m_dwEnabledGroups,
            &l_dwNewGroupMask, sizeof(l_dwNewGroupMask)))
        {
            l_dwModifyGroupMask = -1;
        }

        _stprintf(l_szEntryName, 
            _T("%s_GroupKeyWords"), l_rOutput.m_pszName);
        if(ReadWriteSetting(l_bRegistry, p_bWrite, TRUE, 
            l_hKey, p_pszPath, l_szEntryName, 
            l_rOutput.m_szEnabledGroupsKeywords,
            l_szKeywordMask, sizeof(l_szKeywordMask)))
        {
            l_pszNewKeywordMask = (char*)l_szKeywordMask;
#ifdef UNICODE
            //simple conversion Unicode to single byte
            for(int j = 0; ; j++)
            {
                l_pszNewKeywordMask[j] = (char)l_szKeywordMask[j];
                if(l_szKeywordMask[j])
                    break;
            }
#endif
        }

        if(l_rOutput.m_dwOutputID == TO_File)
        {
            _stprintf(l_szEntryName, _T("%s_Path"), 
                l_rOutput.m_pszName);
            TCHAR l_szFilePath[_MAX_PATH] = _T("");

            ReadWriteSetting(l_bRegistry, p_bWrite, TRUE, 
                l_hKey, p_pszPath, l_szEntryName, 
                s_Impl.m_szFileName,
                l_szFilePath, sizeof(l_szFilePath));
            if(!p_bWrite)
            {
                if(*l_szFilePath == 0)
                {
                    s_Impl.m_File.CloseFile();
                }
                else
                {
                    TraceSetOutputToFile(l_szFilePath,
                        0,0,0,0);
                }
            }
        }//if(l_rOutput.m_dwOutputID == TO_File)
        if(!p_bWrite)
        {
            s_Impl.TraceAssignGroupsToStream(
                l_rOutput.m_dwOutputID, 
                l_dwNewGroupMask, l_dwModifyGroupMask, 
                l_pszNewKeywordMask);
        }
    }//for(int i = 0; i < NUM_STREAMS;i++)
    if(l_hKey)
    {
        RegCloseKey(l_hKey);
    }
    return true;
}//ULONG TraceReadWriteSettings
#endif //#if defined(WINAPP) }

#if defined(WINAPP) || defined(__linux__) //{
/*-------------------------------------------------------------
   FUNCTION: TraceSetOutputToFile
   PURPOSE:  Enable tracing to a file. Unlike memory-mapped 
    file, output to a file will directly print trace messages
    to a file, which allows an unlimited size, but may be 
    slower. This function accepts 2 masks: p_dwNewGroupFlags
    to decide which trace statements should be printed to file
    and p_dwNewFlushBits to determine which trace statements 
    should also flush the file. Flushing the file allows to 
    preserve the trace if an application crashes, but is much
    slower.
    
   PARAMETERS:                   .
    LPCTSTR p_pszFileName       - file name to print traces to
    ULONG   p_dwNewGroupFlags     -new trace groups flags
    ULONG   p_dwGroupFlagsToModify-trace groups flags to change
    ULONG   p_dwNewFlushBits - new trace flags for flushing
    ULONG   p_dwFlushBitsToModify  - trace flags to change
-------------------------------------------------------------*/
bool TraceSetOutputToFile
(
    LPCTSTR p_pszFileName,
    ULONG   p_dwNewGroupFlags   ,
    ULONG   p_dwGroupFlagsToModify,
    ULONG   p_dwNewFlushBits   ,
    ULONG   p_dwFlushBitsToModify
)
{
    int iLen = _tcslen(p_pszFileName);
    if(iLen >= countof(s_Impl.m_szFileName))
        return false;//too long name
    iLen = (iLen+1)*sizeof(TCHAR);
    ChangeLock l_ChangeLock;
    if(!l_ChangeLock.isFirst())
        return false;
    if(p_dwNewGroupFlags && (
        memcmp(s_Impl.m_szFileName, p_pszFileName, iLen) ||
       !s_Impl.m_File.IsOpened()))
    {
        s_Impl.m_File.CloseFile();
        if(s_Impl.m_File.OpenFile(p_pszFileName, 
            true, true, NULL))
        {
            p_dwNewGroupFlags = 0;
            p_dwGroupFlagsToModify = (ULONG)-1;
        }
    }
    memcpy(s_Impl.m_szFileName, p_pszFileName, iLen);

    s_Impl.TraceAssignGroupsToStream(TO_File, 
        p_dwNewGroupFlags, p_dwGroupFlagsToModify, NULL);
    s_Impl.TraceAssignGroupsToStream(TO_FlushFile, 
        p_dwNewFlushBits, p_dwFlushBitsToModify, NULL);

    return true;
}//void TraceSetOutputToFile

LPCTSTR TraceGetCurTraceFileName()
{
    return s_Impl.m_szFileName;
}
#endif

/*-------------------------------------------------------------
   FUNCTION: TraceImpl::TraceSetExternalBuffer
   PURPOSE:  Start using an externally allocated trace buffer.
-------------------------------------------------------------*/
bool TraceImpl::TraceSetExternalBuffer
(
    GlobalTraceBufferHeader * p_pBufferHeader
)
{
    if(m_BufferPointers.m_pGlobalHeader)
        return false;//Another buffer is already in use
    #ifdef HAVE_EXCEPTIONS
    __try
    {
    #endif    
        if(!p_pBufferHeader || p_pBufferHeader->m_dwSignature!=
            TRACE_BUFFER_SIGNATURE)
        {
            HTRACE(TG_Error, 
            _T("ERROR: TraceSetExternalBuffer(%x) invalid buffer"),
                    p_pBufferHeader);
            return false;
        }
        AssignTraceBufferPtr(p_pBufferHeader);
        m_bBufferAllocated = false;
        return true;
    #ifdef HAVE_EXCEPTIONS
    }
    __except(1)
    {
        HTRACE(TG_Error, 
            _T("ERROR: Exception in TraceSetExternalBuffer(%x)"),
                p_pBufferHeader);
        return false;
    }
    #endif
}//bool TraceImpl::TraceSetExternalBuffer

/*-------------------------------------------------------------
   FUNCTION: TraceSetExternalBuffer
   PURPOSE:  Start using an externally allocated trace buffer
    and enable trace groups for that output.
-------------------------------------------------------------*/
bool TraceSetExternalBuffer(
    GlobalTraceBufferHeader * p_pBufferHeader,
    ULONG   p_dwNewGroupFlags, ULONG   p_dwGroupFlagsToModify)
{
    ChangeLock l_ChangeLock;
    if(!l_ChangeLock.isFirst())
        return false;

    if(!s_Impl.TraceSetExternalBuffer(p_pBufferHeader))
    {
        s_Impl.TraceAssignGroupsToStream
            (TO_MemoryBuffer, 0, (ULONG)-1, NULL);
        return false;
    }
    s_Impl.TraceAssignGroupsToStream
        (TO_MemoryBuffer, p_dwNewGroupFlags, 
        p_dwGroupFlagsToModify, NULL);
    return true;
}

/*-------------------------------------------------------------
   FUNCTION: TraceImpl::TraceAllocateBuffer
   PURPOSE:  Allocate trace buffer of the given size
-------------------------------------------------------------*/
bool TraceImpl::TraceAllocateBuffer
(
    int p_iSizeTextArea
)
{
    if(m_BufferPointers.m_pGlobalHeader)
    {
        HTRACE(TG_Error, 
            _T("ERROR: cannot allocate a new buffer because ")
            _T("another one is in use"));
        return false;//Another buffer is already in use
    }

    int l_iTotalSize =TRACE_BUFFER_EXTRA_SIZE+p_iSizeTextArea;

    GlobalTraceBufferHeader * l_pBufferHeader = NULL;
    #ifdef TRACER_NTDRIVER 
        l_pBufferHeader = (GlobalTraceBufferHeader*) 
            ExAllocatePool(NonPagedPool, l_iTotalSize);
    #elif defined(CE_KERNEL_DRV)
        l_pBufferHeader = (GlobalTraceBufferHeader*) VirtualAlloc
            (NULL, l_iTotalSize, MEM_COMMIT, PAGE_READWRITE);
    #else 
        l_pBufferHeader = 
            (GlobalTraceBufferHeader*) malloc(l_iTotalSize);
    #endif
    if(!l_pBufferHeader)
    {
        HTRACE(TG_Error, 
            _T("ERROR: cannot allocate trace buffer of size %d"),
            l_iTotalSize);
        return false;
    }
    l_pBufferHeader->m_dwSignature = TRACE_BUFFER_SIGNATURE;

    SetTextAreaSizeHex(l_pBufferHeader->m_cSizeTextArea, 
        p_iSizeTextArea);

    AssignTraceBufferPtr(l_pBufferHeader);
    m_BufferPointers.m_pGlobalFooter->m_dwNumBytesWritten = 0;
    m_BufferPointers.m_pGlobalFooter->m_dwStopAfterThreshold 
        = (ULONG)-1;
    m_BufferPointers.m_pGlobalFooter->m_dwFrozen = 0;
    m_bBufferAllocated = true;
    return true;
}//bool TraceAllocateBuffer

/*-------------------------------------------------------------
   FUNCTION: TraceAllocateBuffer
   PURPOSE:  Allocate trace buffer of the given size
    and enable trace groups for that output.
-------------------------------------------------------------*/
bool TraceAllocateBuffer(int p_iSize,
    ULONG   p_dwNewGroupFlags, ULONG   p_dwGroupFlagsToModify)
{
    ChangeLock l_ChangeLock;
    if(!l_ChangeLock.isFirst())
        return false;

    if(!s_Impl.TraceAllocateBuffer(p_iSize))
    {
        if(!s_Impl.m_BufferPointers.m_pGlobalHeader)
        {
            s_Impl.TraceAssignGroupsToStream
                (TO_MemoryBuffer, 0, (ULONG)-1, NULL);
        }
        return false;
    }
    s_Impl.TraceAssignGroupsToStream
        (TO_MemoryBuffer, p_dwNewGroupFlags, 
        p_dwGroupFlagsToModify, NULL);
    return true;
}

/*-------------------------------------------------------------
   FUNCTION: TraceImpl::TraceFreeBuffer
   PURPOSE:  Free the trace buffer if allocated or mapped
-------------------------------------------------------------*/
bool TraceImpl::TraceFreeBuffer()
{
    if(m_BufferPointers.m_pGlobalHeader)
    {
        m_pdwTotalMask = &m_dwTotalMask;

        GlobalTraceBufferHeader * l_pBufferHeader = 
            m_BufferPointers.m_pGlobalHeader;
        m_BufferPointers.m_pGlobalHeader = NULL;

        if(m_bBufferAllocated)
        {
            #ifdef TRACER_NTDRIVER 
                ExFreePool(l_pBufferHeader);
            #elif defined(CE_KERNEL_DRV)
                VirtualFree(l_pBufferHeader, 0, MEM_RELEASE);
            #else
                free(l_pBufferHeader);
            #endif
            m_bBufferAllocated = false;
        }

        #if defined(WINAPP)
        if(m_bBufferMapped)
        {
            UnmapViewOfFile(l_pBufferHeader);
            m_bBufferMapped = false;
            l_pBufferHeader = NULL;
        }

        if(m_bMappedDriverBuffer &&
           m_hDeviceWithBuffer != INVALID_HANDLE_VALUE)
        {
            DWORD l_dwBytesReturned = 0;
            if(!DeviceIoControl(m_hDeviceWithBuffer, 
                m_iIOCTLToUnmapDriverBuffer, 
                &l_pBufferHeader, sizeof(l_pBufferHeader), 
                NULL, 0, &l_dwBytesReturned, NULL))
            {
                HTRACE(TG_Error, 
                    _T("ERROR: DeviceIoControl")
                    _T("(IOCTL_TRACER_UNMAP_BUFFER) failed - %s"), 
                    ERR_EXPL(GetLastError()));
            }
            m_bMappedDriverBuffer = false;
        }
        #endif//defined(WINAPP)
        #if defined(__linux__)
        if(m_bBufferMapped)
        {
            if(munmap(l_pBufferHeader, m_iMappingSize) != 0)
            {
                HTRACE(TG_Error, 
                 "ERROR: munmap failed. %s", strerror(errno));
            }
            m_bBufferMapped = false;
            l_pBufferHeader = NULL;
        }
        #endif
    }//if(m_pTraceBuffer)
    #if defined(WINAPP)
    if(m_hFileMapping != NULL)
    {
        CloseHandle(m_hFileMapping);
        m_hFileMapping = NULL;
    }

    if(m_hDeviceWithBuffer != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_hDeviceWithBuffer);
        m_hDeviceWithBuffer = INVALID_HANDLE_VALUE;
    }
    #endif
    #if defined(__linux__)
    if(m_iMemMapFile >= 0)
    {
        close(m_iMemMapFile);
        m_iMemMapFile = -1;
        int l_iPrevValue = 0;
        if(!InterlockedExchangeAddUsingSemaphore(m_iSemKey, 
            m_iSemID, -1, l_iPrevValue))
        {
             HTRACE(TG_Error, 
               "InterlockedExchangeAddUsingSemaphore failed");
        }
    }
    #endif
    return true;
}//bool TraceFreeBuffer()

bool TraceFreeBuffer()
{
    ChangeLock l_ChangeLock;
    if(!l_ChangeLock.isFirst())
        return false;
    return s_Impl.TraceFreeBuffer();
}

/*-------------------------------------------------------------

   FUNCTION: WriteCircularBufferToFile

   PURPOSE:  Used to dump trace memory buffer to a file.
    The memory buffer has a circular structure. The oldest data
    starts right after the most recent data. This position can
    be calculated by dividing the total number of bytes written
    by the size of the buffer. This function starts dumping the
    oldest data up to the end of the buffer and then continues
    from the beginning of the buffer.
    This function is available for applications and drivers.
   PARAMETERS:
   const char * p_pszFileName     name of the file
   LocalTraceBufferPointers * p_pBufferPointers - pointers to
                                  the trace buffer
   ULONG * p_pdwReturnStatus      in case of error return Win32
            error (result of GetLastError) or NT kernel status.

   RETURN VALUE:
      bool      true on success, false on failure
-------------------------------------------------------------*/
bool WriteCircularBufferToFile
(
    LPCTSTR      p_pszFileName,
    LocalTraceBufferPointers * p_pBufferPointers,
    ULONG      * p_pdwReturnStatus
)
{
#ifdef HAVE_FILE_OUTPUT
    HTraceFileLocal l_File;
    if(l_File.OpenFile(p_pszFileName, true, true, NULL))
        return false;

    ULONG l_dwWritten = 0;
    
    LARGE_INTEGER l_FileOffset;
    LARGE_INTEGER * l_pOffset = NULL;
    l_FileOffset.QuadPart = 0;
    #if defined(TRACER_NTDRIVER)
    l_pOffset = &l_FileOffset;
    #endif

    if(!p_pBufferPointers->m_pGlobalHeader ||
       !p_pBufferPointers->m_pGlobalFooter)
    {
        char * l_pszString = "Debug memory is NULL";
        l_File.ReadWriteFile(true, 
            l_pszString, strlen(l_pszString)+1,
            l_pOffset, NULL);
        l_File.CloseFile();
        #if defined(TRACER_NTDRIVER)
        if(p_pdwReturnStatus)
            *p_pdwReturnStatus = STATUS_UNSUCCESSFUL;
        #endif
        return false;
    }
    //Our buffer is circular. 
    //We need to write the tail first, then the beginning
    ULONG l_dwTextAreaSize = p_pBufferPointers->m_dwTextAreaSize;
    ULONG l_dwNumBytesWritten = p_pBufferPointers->m_pGlobalFooter->m_dwNumBytesWritten;
    char * l_pszTextArea = (char*)p_pBufferPointers->m_pTextArea;

    int l_iLastAddedByte = l_dwNumBytesWritten % l_dwTextAreaSize;

    if(l_dwNumBytesWritten > l_dwTextAreaSize)
        l_File.ReadWriteFile(true, l_pszTextArea + l_iLastAddedByte, l_dwTextAreaSize - l_iLastAddedByte, l_pOffset, NULL);

    l_File.ReadWriteFile(true, l_pszTextArea, l_iLastAddedByte, l_pOffset, NULL);
    
    l_File.CloseFile();
    return true;
#else
    return false;
#endif
}//bool WriteCircularBufferToFile

/*-------------------------------------------------------------

   FUNCTION: TraceImpl::TraceDumpBufferToFile

   PURPOSE:  Print the accumulated messages from the 
    trace memory buffer to a file.
    This function is available for applications and drivers.
      
   PARAMETERS:                   .
    const char * p_pszFileName   File name.
-------------------------------------------------------------*/
bool TraceImpl::TraceDumpBufferToFile(LPCTSTR p_pszFileName)
{
    if(!WriteCircularBufferToFile(p_pszFileName, 
        &m_BufferPointers, NULL))
        return false;
    return true;
}//bool TraceDumpBufferToFile

bool TraceDumpBufferToFile(LPCTSTR p_pszFileName)
{
    ChangeLock l_ChangeLock;
    if(!l_ChangeLock.isFirst())
        return false;
    return s_Impl.TraceDumpBufferToFile(p_pszFileName);
}

/*-------------------------------------------------------------

   FUNCTION: AddToTraceBuffer

   PURPOSE:  Add the given message to the trace memory buffer.
       This function is available for applications and drivers.
      
   PARAMETERS:                   .

    LPCTSTR p_pszMessage,  Message - 0-terminated string
    int     p_iLen         Number of characters in the string
-------------------------------------------------------------*/
bool AddToTraceBuffer
(
    ULONG,
    LPCTSTR p_pszString, 
    int     p_iLenChars
)
{
    int l_iLenBytes = p_iLenChars * sizeof(TCHAR);
    if(!s_Impl.m_BufferPointers.m_pGlobalHeader)
    {
        return false;
    }
    ULONG l_lTextAreaSize = 
        s_Impl.m_BufferPointers.m_dwTextAreaSize;
    if(!l_lTextAreaSize)
    {
        return false;
    }
    ULONG * l_pdwBytesWritten = &s_Impl.
        m_BufferPointers.m_pGlobalFooter->m_dwNumBytesWritten;
    if(s_Impl.m_BufferPointers.m_pGlobalFooter->m_dwFrozen)
    {
        return false;
    }
    char* l_pszTextArea = (char*)s_Impl.m_BufferPointers.m_pTextArea;

    if((ULONG)l_iLenBytes > l_lTextAreaSize)
       l_iLenBytes = l_lTextAreaSize;

    //To ensure thread safety, we access the m_iWriteTo 
    //variable only from the single atomic operation
    ULONG l_dwWriteAt = InterlockedExchangeAdd
        ((long*)l_pdwBytesWritten,l_iLenBytes);

    if(l_dwWriteAt + l_iLenBytes > s_Impl.m_BufferPointers.
        m_pGlobalFooter->m_dwStopAfterThreshold)
    {
        s_Impl.m_BufferPointers.m_pGlobalFooter->m_dwFrozen = 1;
        return false;
    }


    //Now we reserved a space in the buffer,which other threads
    //will not touch, unless the buffer is filled too fast. In 
    //that case we will have meaningless garbage in the buffer.

    //Wrap around (circular buffer):
    l_dwWriteAt %= l_lTextAreaSize;

    if(l_dwWriteAt + l_iLenBytes <= l_lTextAreaSize)
    {
       memcpy(l_pszTextArea + l_dwWriteAt, p_pszString, l_iLenBytes);
    }
    else
    {
        ULONG l_dwWriteAtTheTail = l_lTextAreaSize - l_dwWriteAt;

        memcpy(l_pszTextArea + l_dwWriteAt, p_pszString, 
            l_dwWriteAtTheTail);

        ULONG l_dwWriteAtTheBeginning = 
            l_iLenBytes -l_dwWriteAtTheTail;

        memcpy(l_pszTextArea, p_pszString + l_dwWriteAtTheTail,
            l_dwWriteAtTheBeginning);
    }
    return true;
}//bool AddToTraceBuffer

#if defined(WINAPP) //{
/*-------------------------------------------------------------
   FUNCTION: CreateOpenFileMapping
   PURPOSE:  Create file mapping for use as a shared memory 
    buffer by WIN32 applications
-------------------------------------------------------------*/
HANDLE CreateOpenFileMapping
(
    LPCTSTR p_pszMemMapFilePath, 
    int     p_iFileSize,
    bool   *p_pbReturnIsCreated
)
{
    *p_pbReturnIsCreated = false;

#if !defined(_WIN32_WCE)
    HANDLE l_hFileMapping = OpenFileMapping(
        FILE_MAP_ALL_ACCESS, FALSE, g_szMemMapFileName);
    if(l_hFileMapping)
        return l_hFileMapping;
    HANDLE l_hFileHandle = CreateFile(p_pszMemMapFilePath, 
        GENERIC_READ | GENERIC_WRITE, 
        FILE_SHARE_WRITE | FILE_SHARE_READ, 0,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
#else
    HANDLE l_hFileHandle = CreateFileForMapping(p_pszMemMapFilePath, 
        GENERIC_READ | GENERIC_WRITE, 
        FILE_SHARE_WRITE | FILE_SHARE_READ, 0,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
#endif
    if(GetLastError() != ERROR_SHARING_VIOLATION)
    {
        *p_pbReturnIsCreated = true;
        if(l_hFileHandle == INVALID_HANDLE_VALUE) 
        {
            HTRACE(TG_Error, 
                _T("ERROR: Cannot open file %s for mapping (%s)"), 
                 p_pszMemMapFilePath, ERR_EXPL(GetLastError()));
            return NULL;
        }//if(l_hFileHandle ==  INVALID_HANDLE_VALUE)
    }
    HANDLE l_hFileMapping = CreateFileMapping(l_hFileHandle, 
        NULL, PAGE_READWRITE, 0, p_iFileSize, g_szMemMapFileName);
    CloseHandle(l_hFileHandle);
    if(!l_hFileMapping)
    {//May be somebody else created it?
#if !defined(_WIN32_WCE)
        l_hFileMapping = OpenFileMapping(
            FILE_MAP_ALL_ACCESS, FALSE, 
            g_szMemMapFileName);
#endif
        if(!l_hFileMapping)
        {
            HTRACE(TG_Error, 
                _T("Failed to create file mapping(%s)"),
                ERR_EXPL(GetLastError()));
            return false;
        }
    }
    return l_hFileMapping;
}//HANDLE CreateNewFileMapping

/*-------------------------------------------------------------
   FUNCTION: TraceImpl::CreateAndMapFile
   PURPOSE:  Open an existing memory mapped file or create new
    one for use as shared memory buffer by WIN32 applications
-------------------------------------------------------------*/
bool TraceImpl::CreateAndMapFile
(
    LPCTSTR       p_pszMemMapFilePath, 
    int           p_iFileSize
)
{
    if(m_BufferPointers.m_pGlobalHeader)
        return false;
    TCHAR l_szFileName[260];
    if(p_pszMemMapFilePath == NULL || 
        !_tcschr(p_pszMemMapFilePath, '\\'))
    {
#if defined(_WIN32_WCE)
        //Just put it in the root
        int l_iLen = 1;
        l_szFileName[0] = '\\';
#else
        //No directory specified. Try to create the file in the
        //current directory
        GetCurrentDirectory(
            sizeof(l_szFileName)/sizeof(l_szFileName[0]), 
            l_szFileName);
        int l_iLen = strlen(l_szFileName);
        if(l_iLen > 0 && l_szFileName[l_iLen-1] != '\\')
            l_szFileName[l_iLen++] = '\\';
#endif
        if(p_pszMemMapFilePath)
            _tcscpy(l_szFileName + l_iLen, p_pszMemMapFilePath);
        else
            _tcscpy(l_szFileName + l_iLen, g_szMemMapFileName);
        p_pszMemMapFilePath = l_szFileName;
    }

    bool l_bFileCreated = false;
    if(!m_hFileMapping) 
    {
        if(p_iFileSize <= 0)
           p_iFileSize = g_iDefaultMemMapFileSize;
        p_iFileSize += TRACE_BUFFER_EXTRA_SIZE;

        m_hFileMapping = CreateOpenFileMapping(p_pszMemMapFilePath, 
            p_iFileSize, &l_bFileCreated);
    }//if(!m_hFileMapping)

    GlobalTraceBufferHeader * l_pBufferHeader = 
        (GlobalTraceBufferHeader * ) 
        MapViewOfFile(m_hFileMapping, FILE_MAP_ALL_ACCESS, 
        0,0,0);
    if(!l_pBufferHeader)
    {
        return false;
    }
    if(l_bFileCreated) 
    {
        memset(l_pBufferHeader, 0, p_iFileSize);
        int l_iSizeTextArea = 
            p_iFileSize - TRACE_BUFFER_EXTRA_SIZE;

        l_pBufferHeader->m_dwSignature = 
            TRACE_BUFFER_SIGNATURE;
        SetTextAreaSizeHex(l_pBufferHeader->m_cSizeTextArea, 
            l_iSizeTextArea);
    }
    AssignTraceBufferPtr(l_pBufferHeader);
    if(l_bFileCreated)
    {
        m_BufferPointers.m_pGlobalFooter->m_dwStopAfterThreshold = 
            (ULONG)-1;
        m_BufferPointers.m_pGlobalFooter->m_dwFrozen = 0; 
    }

    m_bBufferMapped = true;
    return true;
}//bool TraceImpl::CreateAndMapFile
#endif //#if defined(WINAPP) }

#if defined(__linux__)//{
//First we need to define semaphore routines for linux.
//define _TEST_SEM to trace and test semaphore routines 
//in a console app.
//#define _TEST_SEM
#ifdef _TEST_SEM
#define SEM_TRACE(format, param1, param2, param3) \
    {printf(format, param1, param2, param3);\
    printf("Press Enter key to continue\n");\
    getchar();}
#else
#define SEM_TRACE(format, param1, param2, param3)
#endif
static bool DoSemOp(int p_iSemID, int p_iNum, 
                    int p_iIncDec, bool p_bUndoable)
{
    SEM_TRACE("Before calling semop %d #%d Inc %d\n", 
        p_iSemID, p_iNum, p_iIncDec);
    sembuf l_SemBuf;
    l_SemBuf.sem_num = p_iNum;
    l_SemBuf.sem_op = p_iIncDec;
    l_SemBuf.sem_flg = p_bUndoable? SEM_UNDO : 0;
    if(semop(p_iSemID, &l_SemBuf, 1) != 0)
    {
        SEM_TRACE("semop failed. %s\n",
            strerror(errno),0,0);
        HTRACE(TG_Error, "semop failed. %s\n",
            strerror(errno));
        return false;
    }
    SEM_TRACE("After calling semop %d #%d Inc %d\n", 
        p_iSemID, p_iNum, p_iIncDec);
    return true;
}

//We will use a semaphore to achieve InterlockedIncrement/
//InterlockedDecrement behavior between processes. We cannot 
//use regular InterlockedIncrement on a shared memory variable 
//because a process may be killed as a result of a crash or by 
//quitting debugging session, which will leave the shared 
//variable in incremented state forever. Increment of a 
//semaphore, on the other hand, is automatically undone
//by the OS on the process death if SEM_UNDO flag is set.
//The problem with semaphores is that IPC semaphore API is too
//inflexible: it does not allow to increment the semaphore 
//value and get the previous value in the same atomic 
//operation. Therefore, we need to use two operations: 
//semctl(GETVAL) and semop(+1) and protect these operations 
//by a second semaphore to make them atomic. Despite using 
//2 semaphores, this function is not blocking: it may wait
//for the second semaphore very briefly until the another 
//process exits from it's call to 
//InterlockedExchangeAddUsingSemaphore.
static bool InterlockedExchangeAddUsingSemaphore
(
    int   p_iSemKey, 
    int & p_riSemID,
    int   p_iAdd, 
    int & p_riReturnPreviousValue
)
{
    bool l_bResult = true;
    bool l_bCreatedNew = false;
    if(p_riSemID <= 0)
    {
        //Need to create the semaphore.
        //We create a set of 2 semaphores: first is the 
        //interlocked counter. The second is to protect 
        //the modification of the first one.
        int l_iSemID = semget(p_iSemKey,2,
            IPC_CREAT|IPC_EXCL|0660);
        if(l_iSemID < 0)
        {
            SEM_TRACE(
               "Failed to create exclusive semaphore"
                 " %d. %s\n",
                p_iSemKey, strerror(errno), 0);
                //This is not an error
        
            //OK, get it non-exclusively
            l_iSemID = semget(p_iSemKey, 2, 
                IPC_CREAT|0660);
            if(l_iSemID < 0)
            {
                HTRACE(TG_Error, 
                    "Failed to get semaphore %d. %s\n",
                    p_iSemKey, strerror(errno));
                return false;
            }
            SEM_TRACE("Opened semaphore key %d id %d\n", 
                p_iSemKey, l_iSemID, 0);
            //wait and acquire the second semaphore
            if(!DoSemOp(l_iSemID,1,-1, true))
                return false;
        }
        else
        {
            //We just created the semaphores. Both of them 
            //should have value 0, which means that other 
            //processes will be blocked on the second
            //semaphore until we release it below.
            l_bCreatedNew = true;
            SEM_TRACE(
                "Exclusively created semaphore key %d id %d\n",
                p_iSemKey, l_iSemID, 0);            
        }
        p_riSemID = l_iSemID;    
    }//if(p_riSemID <= 0)
    else
    {
        //wait and acquire the second semaphore
        if(!DoSemOp(p_riSemID,1,-1, true))
            return false;
    }
    p_riReturnPreviousValue = semctl(p_riSemID, 0, GETVAL);
    SEM_TRACE("semctl(%d,0,GETVAL) returned %d"
        " going to increment by %d\n",
        p_riSemID, p_riReturnPreviousValue, p_iAdd);
    if(p_riReturnPreviousValue < 0)
    {
        HTRACE(TG_Error, 
            "ERROR: semctl(%d, GETVAL) failed. %s\n",
            p_riSemID, strerror(errno));
        l_bResult = false;
    }
    else if(p_riReturnPreviousValue + p_iAdd < 0)
    {
        HTRACE(TG_Error, 
            "ERROR: InterlockedExchangeAddUsingSemaphore"
            " called with semaphore value %d and add = %d."
            " Result will be negative", 
            p_riReturnPreviousValue, p_iAdd);
        l_bResult = false;
    }
    else
    {
        //increase/decrease value of the first semaphore.
        if(!DoSemOp(p_riSemID,0,p_iAdd, true))
            l_bResult = false;
    }
    //Release second semaphore.
    //We should use non-undoable release if we just created 
    //the semaphore, because it should be left in released 
    //state after the process terminates.
    if(!DoSemOp(p_riSemID,1,+1, l_bCreatedNew? false : true))
        return false;
    return l_bResult;
}//bool InterlockedExchangeAddUsingSemaphore

bool PrepareMemMapFile
(
    int   p_iMemMapFile,
    const char * p_pszMemMapFilePath,
    int   p_iTextAreaSize,
    bool  p_bInitialize,
    int * p_piReturnTotalSize,
    GlobalTraceBufferHeader ** p_ppReturnHeader
)
{
    int l_iTotalFileSize = 
        p_iTextAreaSize + TRACE_BUFFER_EXTRA_SIZE;
    if(p_bInitialize)
    {   //We are the first process, which created the 
        //file and must initialize it.
        //First, make sure the file is long enough
        int l_iSize = lseek(p_iMemMapFile, 0, SEEK_END);
        if(l_iSize < l_iTotalFileSize)
        {
            if(lseek(p_iMemMapFile,l_iTotalFileSize-1,SEEK_SET)
                != l_iTotalFileSize-1)
            {
                HTRACE(TG_Error, 
                    "ERROR: lseek(%s, %d) failed: %s",
                    p_pszMemMapFilePath, l_iTotalFileSize-1,
                    strerror(errno));
                return false;
            }
            if(write(p_iMemMapFile, "", 1) < 0)
            {
                HTRACE(TG_Error, 
                    "ERROR: write of 1 byte to %s failed: %s",
                    p_pszMemMapFilePath, strerror(errno));
                return false;
            }
        }
        else if(l_iSize > l_iTotalFileSize)
        {
            if(ftruncate(p_iMemMapFile, l_iTotalFileSize) < 0)
            {
                HTRACE(TG_Error, "ERROR: ftruncate failed. %s",
                    strerror(errno));
                return false;
            }
        }
        l_iSize = lseek(p_iMemMapFile, 0, SEEK_END);
    }//if(p_bInitialize)
    else
    {   //The file is already in use. We cannot change it's 
        //size and must use it as is.
        //Read file header and size
        GlobalTraceBufferHeader l_Header;
        int l_iHeaderSize = sizeof(l_Header);
        int l_iReadRes = 
            read(p_iMemMapFile, &l_Header, l_iHeaderSize);
        if(l_iReadRes != l_iHeaderSize)
        {
            HTRACE(TG_Error, 
               "ERROR: read(%s) returned %d instead of %d. %s",
                p_pszMemMapFilePath, l_iReadRes, 
                l_iHeaderSize, strerror(errno));
            return false;
                
        }
        int l_iAreaSize = ReadSize(l_Header.m_cSizeTextArea);
        if(l_iAreaSize != p_iTextAreaSize)
        {
            HTRACE(TG_Error, 
                "WARNING: use existing mem map file"
                " of size is %d instead of requested %d",
                l_iAreaSize, p_iTextAreaSize);
            p_iTextAreaSize = l_iAreaSize;
            l_iTotalFileSize = 
                p_iTextAreaSize + TRACE_BUFFER_EXTRA_SIZE;
        }
    }
    //Now perform actual mapping the file to memory
    GlobalTraceBufferHeader * l_pBufferHeader = 
        (GlobalTraceBufferHeader * )
        mmap(0, l_iTotalFileSize, PROT_READ | PROT_WRITE,
        MAP_SHARED, p_iMemMapFile, 0);
    if(l_pBufferHeader == MAP_FAILED)
    {
        HTRACE(TG_Error, 
            "ERROR: mmap of file %s size %d failed: %s",
            p_pszMemMapFilePath, l_iTotalFileSize, 
            strerror(errno));
        return false;
    }
    if(p_bInitialize) 
    {
        //Finish initialization
        memset(l_pBufferHeader, 0, l_iTotalFileSize);

        l_pBufferHeader->m_dwSignature =TRACE_BUFFER_SIGNATURE;
        TraceImpl::SetTextAreaSizeHex
          (l_pBufferHeader->m_cSizeTextArea,  p_iTextAreaSize);
    }
    *p_piReturnTotalSize = l_iTotalFileSize;
    *p_ppReturnHeader = l_pBufferHeader;  
    return true;
}//bool PrepareMemMapFile

bool TraceImpl::CreateAndMapFile
(
    const char *  p_pszMemMapFilePath, 
    int           p_iTextAreaSize
)
{
    if(m_BufferPointers.m_pGlobalHeader)
        return false;
    if(m_iMemMapFile >= 0)
    {//should not happen
        HTRACE(TG_Error, "ERROR: mem map file already opened");
        return false;
    }
   
    if(p_iTextAreaSize <= 0)
       p_iTextAreaSize = g_iDefaultMemMapFileSize;
    if(p_pszMemMapFilePath == NULL)
       p_pszMemMapFilePath = g_szMemMapFileName;

    int l_iMemMapFile = open(p_pszMemMapFilePath, 
        O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if(l_iMemMapFile < 0) 
    {
         HTRACE(TG_Error, 
             "ERROR: Failed to create file mapping %s: %s",
             p_pszMemMapFilePath, strerror(errno));
         return false;
    }//if(l_iMemMapFile < 0)
     
    //We must maintain a counter of processes, which 
    //currently use the shared memory mapped file. We increment
    //the counter when we open the file and decrement when 
    //close. This counter must be automatically
    //decremented when the process if killed prematurely.
    //We will use our InterlockedExchangeAddUsingSemaphore.
    
    int l_iPrevValue = 0;
    if(!InterlockedExchangeAddUsingSemaphore
        (m_iSemKey, m_iSemID, 1, l_iPrevValue))
    {
        HTRACE(TG_Error, 
            "InterlockedExchangeAddUsingSemaphore failed\n");
        return false;
    }   
    bool l_bInitialize = (l_iPrevValue == 0)? true : false;  
    int  l_iTotalSize = 0;
    GlobalTraceBufferHeader * l_pBufferHeader = NULL;    
    if(!PrepareMemMapFile(
        l_iMemMapFile, p_pszMemMapFilePath, p_iTextAreaSize,
        l_bInitialize, &l_iTotalSize, &l_pBufferHeader))
    {    //Failure: roll back
         close(l_iMemMapFile);
         if(!InterlockedExchangeAddUsingSemaphore
             (m_iSemKey, m_iSemID, -1, l_iPrevValue))
         {
             HTRACE(TG_Error, 
              "InterlockedExchangeAddUsingSemaphore failed\n");
         }
         return false;
    }
    AssignTraceBufferPtr(l_pBufferHeader);
    if(l_bInitialize)
    {
        m_BufferPointers.m_pGlobalFooter->m_dwStopAfterThreshold = 
            (ULONG)-1;
        m_BufferPointers.m_pGlobalFooter->m_dwFrozen = 0;   
    }     

    m_iMemMapFile = l_iMemMapFile;
    m_bBufferMapped = true;
    m_iMappingSize = l_iTotalSize; 
    
    return true;
}//bool TraceImpl::CreateAndMapFile
#endif//#if defined(__linux__) }

#if defined(WINAPP) || defined(__linux__) //{
/*-------------------------------------------------------------
   FUNCTION: TraceUseMemMapFileBuffer
   PURPOSE:  Start using memory mapped file,create it if needed
-------------------------------------------------------------*/
bool TraceUseMemMapFileBuffer
(
    LPCTSTR p_pszMemMapFilePath,
    int     p_iFileSize,
    ULONG   p_dwNewGroupFlags, 
    ULONG   p_dwGroupFlagsToModify
)
{
    ChangeLock l_ChangeLock;
    if(!l_ChangeLock.isFirst())
        return false;
    if(s_Impl.m_BufferPointers.m_pGlobalHeader)
    {
        HTRACE(TG_Error,
            _T("ERROR: Memory buffer is already in use"));
        return false;//Another buffer is already in use
    }
    if(!s_Impl.CreateAndMapFile
        (p_pszMemMapFilePath, p_iFileSize))
        return false;
    s_Impl.TraceAssignGroupsToStream(TO_MemoryBuffer, 
        p_dwNewGroupFlags, p_dwGroupFlagsToModify,
        NULL);
    return true;
}//bool TraceUseMemMapFileBuffer
#endif //#if defined(WINAPP) || defined(__linux__) //}

#if defined(WINAPP) //{
/*-------------------------------------------------------------
   FUNCTION: TraceImpl::TraceAttachToNTDriverBuffer
   PURPOSE:  Try to open NT driver, request it to map the trace
    buffer and start using it.
   PARAMETERS:
    LPCSTR  p_pszDeviceName - name of NT device. This parameter
      can be left NULL to use our default name
      TRACER_WIN32_FILE_NAME
    int     p_iIOCTLMapTraceBuffer - NT IOCTL code to map the
      buffer. This parameter can be left -1 to use our default
      IOCTL_TRACER_MAP_BUFFER
    int     p_iIOCTLUnMapTraceBuffer - NT IOCTL code to unmap 
      the buffer. This parameter can be left -1 to use our 
      default IOCTL_TRACER_MAP_BUFFER
    bool    p_bDontComplainIfDeviceAbsent - set to true if the 
      caller expects that the driver may be absent.
-------------------------------------------------------------*/
bool TraceImpl::TraceAttachToNTDriverBuffer
(
    LPCTSTR p_pszDeviceName,
    int     p_iIOCTLMapTraceBuffer,
    int     p_iIOCTLUnMapTraceBuffer,
    bool    p_bDontComplainIfDeviceAbsent
)
{
    if(!p_pszDeviceName)
        p_pszDeviceName = TRACER_WIN32_FILE_NAME;
    if(p_iIOCTLMapTraceBuffer == -1)
       p_iIOCTLMapTraceBuffer = IOCTL_TRACER_MAP_BUFFER;
    if(p_iIOCTLUnMapTraceBuffer == -1)
       p_iIOCTLUnMapTraceBuffer = IOCTL_TRACER_UNMAP_BUFFER;

    HANDLE   l_hDevice = CreateFile(p_pszDeviceName,
                          GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL,
                          OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL,
                          NULL
                          );
    if ( l_hDevice == INVALID_HANDLE_VALUE  )
    {
        if(!p_bDontComplainIfDeviceAbsent)
        {
            HTRACE(TG_Error, 
                _T("ERROR: CreateFile(%s) failed - %s"), 
                p_pszDeviceName, ERR_EXPL(GetLastError()));
        }
        return false;
    }
    DWORD l_dwBytesReturned = 0;
    void * l_pBuffer = NULL;
    if(!DeviceIoControl(l_hDevice, p_iIOCTLMapTraceBuffer, 
        NULL, 0, &l_pBuffer, sizeof(l_pBuffer),
        &l_dwBytesReturned, NULL))
    {
        CloseHandle(l_hDevice);
        HTRACE(TG_Error, 
          _T("ERROR: DeviceIoControl(%d) to map the trace buffer")
            _T(" failed - %s"), 
            p_iIOCTLMapTraceBuffer, ERR_EXPL(GetLastError()));
        return false;
    }
    else if(l_dwBytesReturned != sizeof(DWORD))
    {
        CloseHandle(l_hDevice);
        HTRACE(TG_Error, 
            _T("ERROR: DeviceIoControl to map trace buffer")
            _T("returned %d bytes instead of %d"), 
            l_dwBytesReturned, sizeof(DWORD));
        return false;
    }
    else if(!l_pBuffer || IsBadReadPtr(l_pBuffer,
         sizeof(GlobalTraceBufferHeader)) || 
        ((GlobalTraceBufferHeader*)l_pBuffer)->m_dwSignature !=
        TRACE_BUFFER_SIGNATURE)
    {
        CloseHandle(l_hDevice);
        HTRACE(TG_Error,_T("ERROR:Bad buffer returned by driver"));
        return false;
    }
    if(!TraceSetExternalBuffer
        ((GlobalTraceBufferHeader * )l_pBuffer))
    {
        CloseHandle(l_hDevice);
        return false;
    }
    m_hDeviceWithBuffer = l_hDevice;
    m_bMappedDriverBuffer = true;
    m_iIOCTLToUnmapDriverBuffer = p_iIOCTLUnMapTraceBuffer;

    return true;
}//bool TraceImpl::TraceAttachToNTDriverBuffer

/*-------------------------------------------------------------
   FUNCTION: TraceAttachToNTDriverBuffer
   PURPOSE:  Try to open NT driver, request it to map the trace
    buffer and start using it.
    See TraceImpl::TraceAttachToNTDriverBuffer for more info.
-------------------------------------------------------------*/
bool TraceAttachToNTDriverBuffer
(
    LPCTSTR p_pszDeviceName,
    int     p_iIOCTLMapTraceBuffer,
    int     p_iIOCTLUnMapTraceBuffer,
    bool    p_bDontComplainIfDeviceAbsent,
    ULONG   p_dwNewGroupFlags, 
    ULONG   p_dwGroupFlagsToModify
)
{
    ChangeLock l_ChangeLock;
    if(!l_ChangeLock.isFirst())
        return false;
    if(s_Impl.m_BufferPointers.m_pGlobalHeader)
    {
        HTRACE(TG_Error, 
            _T("ERROR: Memory buffer is already in use"));
        return false;//Another buffer is already in use
    }
    if(!s_Impl.TraceAttachToNTDriverBuffer(
        p_pszDeviceName,
        p_iIOCTLMapTraceBuffer, p_iIOCTLUnMapTraceBuffer,
        p_bDontComplainIfDeviceAbsent))
        return false;
    s_Impl.TraceAssignGroupsToStream
        (TO_MemoryBuffer, p_dwNewGroupFlags, 
        p_dwGroupFlagsToModify, NULL);
    return true;
}//bool TraceAttachToNTDriverBuffer
#endif //defined(WINAPP) }

LocalTraceBufferPointers * pGetTraceBuffer()
{
    return &s_Impl.m_BufferPointers;
}

void TraceFreezeBufferAfter(int p_iPercent)
{
    int l_iSize = p_iPercent * 
        s_Impl.m_BufferPointers.m_dwTextAreaSize / 100;
    s_Impl.m_BufferPointers.m_pGlobalFooter->
        m_dwStopAfterThreshold = s_Impl.m_BufferPointers.
        m_pGlobalFooter->m_dwNumBytesWritten + 
        l_iSize;
}

void TraceEraseBuffer()
{
    s_Impl.m_BufferPointers.m_pGlobalFooter->m_dwNumBytesWritten = 0;
}

#endif //TRACE_ }

#if defined(TRACE_) && defined(WINAPP) //{
/*-------------------------------------------------------------

   FUNCTION: LocalStringHolder strGetErrorExplanation.

   PURPOSE:  Obtain an explanation for the error.
    It is useful for printing Win32 error messages.
    For example : HTRACE(TG_Error, 
     "ERROR: CreateFile failed - %s", ERR_EXPL(GetLastError());
      
   PARAMETERS:                   
                                  
  ULONG p_dwErrCode WIN32 error code returned from GetLastError
-------------------------------------------------------------*/
LocalStringHolder strGetErrorExplanation(ULONG p_dwErrCode)
{
    if(!p_dwErrCode)
        return LocalStringHolder();

    TCHAR * l_pszBuffer = NULL;
    
    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | 
        FORMAT_MESSAGE_FROM_SYSTEM,
        0, p_dwErrCode, 0, (TCHAR*)&l_pszBuffer, 0, NULL);
  
    LocalStringHolder l_strReturn;

    if(l_pszBuffer) 
    {
        TCHAR * l_pNewBuffer = (TCHAR * )
            LocalAlloc(LPTR, 
            (_tcslen(l_pszBuffer)+40)*sizeof(TCHAR));
        if(l_pNewBuffer)
        {
            wsprintf(l_pNewBuffer, _T("Error %d(0x%x) - %s"), 
                p_dwErrCode, p_dwErrCode, l_pszBuffer);
            LocalFree(l_pszBuffer);
        }
        else
            l_pNewBuffer = l_pszBuffer;
        l_strReturn.AdoptStr(l_pNewBuffer);
    }//if(l_pszBuffer) 
  
    return l_strReturn;
}//LocalStringHolder strGetErrorExplanation
#endif //#if defined(TRACE_) && defined(WINAPP) //}

#ifdef TRACE_ //{
#ifdef HAVE_FILE_OUTPUT //{
/*-------------------------------------------------------------

   FUNCTION: HTraceFileImpl::Cleanup()

   PURPOSE: Initialize the file handle without freeing it
-------------------------------------------------------------*/
void HTraceFileImpl::Cleanup()
{
#if !defined(TRACER_NTDRIVER)
    m_hFileHandle = INVALID_HANDLE_VALUE;
#else
    m_hFileHandle = NULL;
#endif
}

/*-------------------------------------------------------------

   FUNCTION: ULONG HTraceFileImpl::OpenFile

   PURPOSE:  To open a file under WIN32 or NT kernel mode.
      
   PARAMETERS:                 .
                                  
   const char * p_pszFileName  name of the file (0 terminated)
   bool         p_bWrite       open for writing if true, 
                               reading otherwise
   bool         p_bPrependQuestionMarks - if working in kernel 
                           mode prepend "\??\" to the file name
   PUNICODE_STRING p_FileName     (for drivers only) If != NULL
                              this name will be used instead of
                                  p_pszFileName
   RETURN VALUE:
      ULONG  0 on success, in case of error return WIN32 error
        (result of GetLastError) or NT kernel status.
-------------------------------------------------------------*/
ULONG HTraceFileImpl::OpenFile
(
    LPCTSTR      p_pszFileName,
    bool         p_bWrite,
    bool         p_bPrependQuestionMarks,
#if defined(TRACER_NTDRIVER)
    PUNICODE_STRING p_FileName
#else
    void *
#endif
)
{
    CloseFile();//if a file is opened, close it
#if !defined(TRACER_NTDRIVER)
#ifdef WIN32
    DWORD l_dwDesiredAccess = 
        p_bWrite? GENERIC_WRITE:GENERIC_READ;
    DWORD l_dwCreationDisposition = p_bWrite? 
        CREATE_ALWAYS : OPEN_EXISTING;

    m_hFileHandle = CreateFile(p_pszFileName,l_dwDesiredAccess,
        FILE_SHARE_READ, NULL, l_dwCreationDisposition, 
        FILE_ATTRIBUTE_NORMAL,NULL);
#else
    if(m_hFileHandle != INVALID_HANDLE_VALUE)
        close(m_hFileHandle);
    m_hFileHandle = open(p_pszFileName,
        O_WRONLY|O_CREAT|O_TRUNC, 0664);
#endif
    if(m_hFileHandle == INVALID_HANDLE_VALUE)
    {
        #ifdef WIN32
            ULONG l_dwError = GetLastError();
        #else
            ULONG l_dwError = errno;
        #endif

        HTRACE(TG_Error,
            _T("ERROR:Cannot open file \"%s\" for %s.Err code %d"),
            p_pszFileName, p_bWrite? _T("writing") : _T("reading"), 
            l_dwError);
        return l_dwError;
    }
    return 0;
#else
    if(KeGetCurrentIrql() > PASSIVE_LEVEL)
        return STATUS_UNSUCCESSFUL;
    ANSI_STRING l_AnsiName;
    UNICODE_STRING l_UnicodeName;
    NTSTATUS l_Status;

    if(!p_FileName)
    {
        RtlInitAnsiString(&l_AnsiName, p_pszFileName);

        l_UnicodeName.Length = 0;
        l_UnicodeName.MaximumLength = 
                    (l_AnsiName.Length + 5) * sizeof(WCHAR);
                    
        if(NULL == ( l_UnicodeName.Buffer = (PWCHAR)
            ExAllocatePool(PagedPool,
            l_UnicodeName.MaximumLength)))
        {
            HTRACE(TG_Error,"ERROR: ExAllocatePool(%d) failed",
                l_UnicodeName.MaximumLength);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        if(p_bPrependQuestionMarks)
        {
            l_UnicodeName.Buffer[0] = '\\';
            l_UnicodeName.Buffer[1] = '?';
            l_UnicodeName.Buffer[2] = '?';
            l_UnicodeName.Buffer[3] = '\\';

            l_UnicodeName.Buffer += 4;
            l_UnicodeName.MaximumLength -= 4 * sizeof(WCHAR);
        }
        RtlAnsiStringToUnicodeString
            (&l_UnicodeName, &l_AnsiName, false);
        if(p_bPrependQuestionMarks)
        {
            l_UnicodeName.Buffer -= 4;
            l_UnicodeName.Length += 4 * sizeof(WCHAR);
            l_UnicodeName.MaximumLength += 4 * sizeof(WCHAR);
        }
        p_FileName = &l_UnicodeName;
    }//if(!p_FileName)
    OBJECT_ATTRIBUTES l_Attr;
    InitializeObjectAttributes(&l_Attr, p_FileName, 
        OBJ_CASE_INSENSITIVE, NULL, NULL);

    IO_STATUS_BLOCK l_IoStatus;
    
    l_IoStatus.Status = 0;
    l_IoStatus.Information = 0;

    ACCESS_MASK l_AccessMask = p_bWrite? 
        GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE :
        GENERIC_READ | SYNCHRONIZE;
    ULONG l_dwShare = p_bWrite? 0 : FILE_SHARE_READ;
    ULONG l_dwCreateDisposition = p_bWrite?
        FILE_OVERWRITE_IF : FILE_OPEN;

    l_Status = ZwCreateFile(&m_hFileHandle, l_AccessMask,
        &l_Attr, &l_IoStatus, 0, 
        FILE_ATTRIBUTE_NORMAL,l_dwShare,l_dwCreateDisposition,
        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
        NULL, 0);
    if(!NT_SUCCESS(l_Status))
    {
        HTRACE(TG_Error, 
            "ERROR : failed to open file %S for %s. Status %x",
            p_FileName->Buffer, 
            p_bWrite? "writing" : "reading", l_Status);
    }
    if(p_FileName == &l_UnicodeName)
        RtlFreeUnicodeString(p_FileName);

    if(!NT_SUCCESS(l_Status))
    {
        m_hFileHandle = NULL;
        return l_Status;
    }
    return STATUS_SUCCESS;
#endif
}//ULONG HTraceFileImpl::OpenFile

/*-------------------------------------------------------------

   FUNCTION: ULONG HTraceFileImpl::CloseFile()

   PURPOSE:  Close the file opened by OpenFile
   
   RETURN VALUE:
      ULONG  0 on success, in case of error return Win32 error
        (result of GetLastError) or NT kernel status.
-------------------------------------------------------------*/
ULONG HTraceFileImpl::CloseFile()
{
#if defined(TRACER_NTDRIVER)
    NTSTATUS l_Status = STATUS_SUCCESS;
    if(m_hFileHandle)
    {
        l_Status = ZwClose(m_hFileHandle);
        if(!NT_SUCCESS(l_Status))
        {
            HTRACE(TG_Error, 
                "ERROR : ZwClose failed. Status %x", l_Status);
        }
    }//if(m_hFileHandle)
    m_hFileHandle = NULL;
    return l_Status;
#elif defined(WIN32)
    DWORD l_dwError = 0;
    if(m_hFileHandle != INVALID_HANDLE_VALUE)
    {
        if(!CloseHandle(m_hFileHandle))
        {
            l_dwError = GetLastError();
            HTRACE(TG_Error, _T("ERROR: CloseHandle failed. %s"),
                ERR_EXPL(l_dwError));
        }
    }//if(m_hFileHandle != INVALID_HANDLE_VALUE)
    m_hFileHandle = INVALID_HANDLE_VALUE;
    return l_dwError;
#else
    if(m_hFileHandle != INVALID_HANDLE_VALUE)
    {
        long l_Result = close(m_hFileHandle);
        if(l_Result != 0)
        {
            HTRACE(TG_Error, 
                "ERROR: CloseHandle failed. Err code %d", 
                l_Result);
            return l_Result;
        }
    }//if(m_hFileHandle != INVALID_HANDLE_VALUE)
    m_hFileHandle = INVALID_HANDLE_VALUE;
    return 0;
#endif
}//ULONG HTraceFileImpl::CloseFile()

bool HTraceFileImpl::IsOpened()
{
#if defined(TRACER_NTDRIVER)
    return m_hFileHandle? true : false;
#else
    return (m_hFileHandle!=INVALID_HANDLE_VALUE)? true : false;
#endif
}

/*-------------------------------------------------------------

   FUNCTION: ULONG HTraceFileImpl::GetFileSize

   PURPOSE:  Get size of the file
   
   PARAMETERS:
    PLARGE_INTEGER p_pReturnSize-return 64 bit size of the file
   RETURN VALUE:
      ULONG   0 on success, in case of error return Win32 error
        (result of GetLastError) or NT kernel status.
-------------------------------------------------------------*/
ULONG HTraceFileImpl::GetFileSize
(
    OUT PLARGE_INTEGER  p_pReturnSize
)
{
#if defined(TRACER_NTDRIVER)
    IO_STATUS_BLOCK l_IOStatus;
    NTSTATUS        l_Status;
    FILE_STANDARD_INFORMATION   l_Info;

    l_Status =ZwQueryInformationFile(m_hFileHandle,&l_IOStatus,
        &l_Info, sizeof(l_Info), FileStandardInformation);
    if(!NT_SUCCESS(l_Status))
    {
        HTRACE(TG_Error,"ERROR: ZwQueryInformationFile ret %x",
            l_Status);
        if(p_pReturnSize)
           p_pReturnSize->QuadPart = 0;
        return l_Status;
    }
    if(p_pReturnSize)
        *p_pReturnSize = l_Info.EndOfFile;
    return STATUS_SUCCESS;
#elif defined(WIN32)
    DWORD l_dwSizeLow = 0, l_dwSizeHigh = 0;
    DWORD l_dwError = 0;
    l_dwSizeLow = ::GetFileSize(m_hFileHandle, &l_dwSizeHigh);
    if(l_dwSizeLow == (DWORD)-1)
    {
        l_dwError = GetLastError();
        if(l_dwError)
        {
            HTRACE(TG_Error, _T("ERROR: GetFileSize failed. %s"),
                ERR_EXPL(l_dwError));
        }
    }
    if(p_pReturnSize)
    {
        p_pReturnSize->LowPart = l_dwSizeLow;
        p_pReturnSize->HighPart = l_dwSizeHigh;
    }
    return l_dwError;
#else
    struct stat    l_Stat;
    long l_StatResult = fstat(m_hFileHandle, &l_Stat);
    if(l_StatResult != 0)
    {
        HTRACE(TG_Error, "ERROR: fstat(%d) failed. Code %d",
           m_hFileHandle, l_StatResult);
        p_pReturnSize->QuadPart = 0;
        return l_StatResult;
    }
    p_pReturnSize->QuadPart = l_Stat.st_size;
    return 0;
#endif
}//ULONG HTraceFileImpl::GetFileSize

/*-------------------------------------------------------------

   FUNCTION: ULONG HTraceFileImpl::ReadWriteFile

   PURPOSE:  Read or write from / to file
   
   PARAMETERS:
    IN  bool    p_bWrite   - true for write, false for read
    OUT void * p_lpBuffer  - buffer to read/write
    IN  ULONG p_dwNumberOfBytes - number of bytes to read/write
    IN  OUT PLARGE_INTEGER p_pByteOffset - if !=NULL,read/write
        starting at this offset, which will be incremented.
    OUT ULONG * p_pdwNumberOfBytesProcessed -if !=NULL, returns
        the number of bytes returned in p_lpBuffer. No error is
        returned if this number is smaller than requested.
        If == NULL and the number of bytes returned is smaller
        than requested, than an error will be returned.

   RETURN VALUE:
      ULONG  0 on success, in case of error return Win32 error
        (result of GetLastError) or NT kernel status.
-------------------------------------------------------------*/
ULONG HTraceFileImpl::ReadWriteFile
(
    IN      bool    p_bWrite,
    OUT     void *  p_lpBuffer,
    IN      ULONG   p_dwNumberOfBytes,
    IN  OUT PLARGE_INTEGER p_pByteOffset,
    OUT     ULONG*  p_pdwNumberOfBytesProcessed
)
{
    if(p_pdwNumberOfBytesProcessed)
      *p_pdwNumberOfBytesProcessed = 0;
#if defined(TRACER_NTDRIVER)
    if(KeGetCurrentIrql() > PASSIVE_LEVEL)
        return STATUS_UNSUCCESSFUL;

    IO_STATUS_BLOCK l_IoStatus;

    l_IoStatus.Status = 0;
    l_IoStatus.Information = 0;

    NTSTATUS l_Status = 
        p_bWrite? 
        ZwWriteFile(m_hFileHandle, NULL, NULL, NULL,
        &l_IoStatus, (void*)p_lpBuffer, p_dwNumberOfBytes, 
        p_pByteOffset, NULL):
        ZwReadFile(m_hFileHandle, NULL, NULL, NULL,
        &l_IoStatus, (void*)p_lpBuffer, p_dwNumberOfBytes, 
        p_pByteOffset, NULL);
    if(!NT_SUCCESS(l_Status))
    {
        HTRACE(TG_Error, "ERROR: %s with size %d failed (%x)",
            p_bWrite? "ZwWriteFile" : "ZwReadFile",
            p_dwNumberOfBytes, l_Status);
        return l_Status;
    }
    if(p_pByteOffset)
        p_pByteOffset->QuadPart += l_IoStatus.Information;

    if(p_pdwNumberOfBytesProcessed)
      *p_pdwNumberOfBytesProcessed = l_IoStatus.Information;
    else if(l_IoStatus.Information < p_dwNumberOfBytes)
    {
        HTRACE(TG_Error, 
            "%s with size %d returned only %d bytes",
            p_bWrite? "ZwWriteFile" : "ZwReadFile",
            p_dwNumberOfBytes, l_IoStatus.Information);
        return STATUS_END_OF_FILE;
    }
    return STATUS_SUCCESS;
#elif defined(WIN32)
    DWORD l_dwBytesProcessed = 0;
    DWORD l_dwError = 0;

    if(p_pByteOffset)
    {
        DWORD l_dwOffsetLow = p_pByteOffset->LowPart;
        LONG  l_dwOffsetHigh = p_pByteOffset->HighPart;
        l_dwOffsetLow = SetFilePointer(m_hFileHandle, l_dwOffsetLow, &l_dwOffsetHigh, FILE_BEGIN);
        if(l_dwOffsetLow == (DWORD)-1)
        {
            l_dwError = GetLastError();
            if(l_dwError)
            {
                HTRACE(TG_Error, _T("ERROR in SetFilePointer. %s"),
                    ERR_EXPL(l_dwError));
                return l_dwError;
            }
        }//if(l_dwOffsetLow == (DWORD)-1)
        if(l_dwOffsetLow != p_pByteOffset->LowPart ||
           l_dwOffsetHigh != p_pByteOffset->HighPart)
        {
            HTRACE(TG_Error, 
                _T("ERROR SetFilePointer(%8x%8x) ret %8x%8x"),
                p_pByteOffset->HighPart,p_pByteOffset->LowPart,
                l_dwOffsetHigh, l_dwOffsetLow);
            return ERROR_SEEK;
        }
    }//if(p_pByteOffset)

    if(!(p_bWrite?
        WriteFile(m_hFileHandle, p_lpBuffer, p_dwNumberOfBytes, &l_dwBytesProcessed, NULL):
        ReadFile(m_hFileHandle, p_lpBuffer, p_dwNumberOfBytes, &l_dwBytesProcessed, NULL)))
    {
        l_dwError = GetLastError();

        HTRACE(TG_Error, 
            _T("ERROR: %s with size %d failed (%s)"),
            p_bWrite? "WriteFile" : "ReadFile",
            p_dwNumberOfBytes, 
            ERR_EXPL(l_dwError));
        return l_dwError;
    }
    if(p_pByteOffset)
        p_pByteOffset->QuadPart += l_dwBytesProcessed;

    if(p_pdwNumberOfBytesProcessed)
      *p_pdwNumberOfBytesProcessed = l_dwBytesProcessed;
    else if(l_dwBytesProcessed < p_dwNumberOfBytes)
    {
        HTRACE(TG_Error, 
            _T("ERROR: %s with size %d returned only %d bytes"),
            p_bWrite? "WriteFile" : "ReadFile",
            p_dwNumberOfBytes, l_dwBytesProcessed);
        return ERROR_END_OF_MEDIA;
    }
    return l_dwError;
#else
    ULONG l_dwBytesProcessed = 0;
    ULONG l_dwError = 0;

    if(p_pByteOffset)
    {
        long l_lOffset = lseek(m_hFileHandle, 
            p_pByteOffset->QuadPart, SEEK_SET);
        if(l_lOffset == -1)
        {
            l_dwError = errno;
            HTRACE(TG_Error, "ERROR in lseek. %d", l_dwError);
            return l_dwError;
        }//if(l_lOffset == -1)
        if(l_lOffset != p_pByteOffset->QuadPart)
        {
            HTRACE(TG_Error, 
                "ERROR lseek(%x) ret %x",
                p_pByteOffset->QuadPart, l_lOffset);
            return (ULONG)-1;
        }
    }//if(p_pByteOffset)

    l_dwBytesProcessed = p_bWrite?
      write(m_hFileHandle,(char*)p_lpBuffer,p_dwNumberOfBytes):
      read (m_hFileHandle,(char*)p_lpBuffer,p_dwNumberOfBytes);
    if(l_dwBytesProcessed == (ULONG)-1)
    {
        l_dwError = errno;

        HTRACE(TG_Error, 
            "ERROR: %s with size %d failed Err %s",
            p_bWrite? "write" : "read",
            p_dwNumberOfBytes,
            strerror(errno));
        return l_dwError;
    }
    if(p_pByteOffset)
        p_pByteOffset->QuadPart += l_dwBytesProcessed;

    if(p_pdwNumberOfBytesProcessed)
      *p_pdwNumberOfBytesProcessed = l_dwBytesProcessed;
    else if(l_dwBytesProcessed < p_dwNumberOfBytes)
    {
        HTRACE(TG_Error, 
            "ERROR: %s with size %d returned only %d bytes",
            p_bWrite? "write" : "read",
            p_dwNumberOfBytes, l_dwBytesProcessed);
        return (ULONG)-1;
    }
    return l_dwError;
#endif
}//ULONG HTraceFileImpl::ReadWriteFile

ULONG HTraceFileImpl::FlushBuffers()
{
#if defined(TRACER_NTDRIVER)
    return STATUS_UNSUCCESSFUL;
#elif defined(WIN32)
    ULONG l_dwError = 0;
    if(!FlushFileBuffers(m_hFileHandle))
    {
        l_dwError = GetLastError();
        HTRACE(TG_Error, _T("ERROR: FlushFileBuffers failed. %s"),
            ERR_EXPL(l_dwError));
    }
    return l_dwError;
#elif defined(__linux__)
    int l_iResult = fsync(m_hFileHandle);
    if(l_iResult != 0)
    {
        HTRACE(TG_Error, 
            "ERROR: fsync failed. %s", strerror(errno));
        return l_iResult;
    }
    return 0;
#endif
}//NTSTATUS HTraceFileImpl::FlushBuffers
#endif //HAVE_FILE_OUTPUT }

#ifdef TRACER_NTDRIVER //{
/*-------------------------------------------------------------

   FUNCTION: EnableTraceToOutputLog

   PURPOSE: To enable printing some groups of trace messages to
             the system error log.
             This function available for NT drivers only.

   PARAMETERS:                   .

    PDRIVER_OBJECT p_pDriverObject - driver object associated
                                     with the all log entries
    ULONG   p_dwNewGroupFlags   - which groups of traces should
                                     be added to the log (from 
                                     enum TraceGroups).
    ULONG   p_dwGroupFlagsToModify - which bits to modify. If a
                        bit set to 1 in p_dwGroupFlagsToModify,
                                  the correspondent bit will be
                                  taken from p_dwNewGroupFlags.
                               Otherwise, it will be preserved.
-------------------------------------------------------------*/
bool EnableTraceToOutputLog
(
    PDRIVER_OBJECT p_pDriverObject,
    ULONG   p_dwNewGroupFlags, 
    ULONG   p_dwGroupFlagsToModify
)
{
    ChangeLock l_ChangeLock;
    if(!l_ChangeLock.isFirst())
        return false;
    s_Impl.TraceAssignGroupsToStream
        (TO_OutputLog, p_dwNewGroupFlags, 
        p_dwGroupFlagsToModify, NULL);
    s_Impl.m_pDriverObject = p_pDriverObject;
    return true;
}//bool EnableTraceToOutputLog

/*-------------------------------------------------------------

   FUNCTION: AddToOutputLog

   PURPOSE:  Add the given message to the system error log.
             This function available for NT drivers only.
      
   PARAMETERS:                   .

    LPCTSTR p_pszMessage,   Message - 0-terminated string
    int     p_iLenChars    Number of characters in the string
  
-------------------------------------------------------------*/
bool AddToOutputLog
(
    ULONG   p_dwMask, 
    LPCTSTR p_pszString,
    int     p_iLenChars
)
{
    if(!s_Impl.m_pDriverObject ||
        KeGetCurrentIrql() > DISPATCH_LEVEL)
        return false;

    int l_iStringSize = (p_iLen+1)*sizeof(WCHAR);
    UCHAR l_cPacketSize = 
        sizeof( IO_ERROR_LOG_PACKET ) + l_iStringSize;
    if(l_cPacketSize > ERROR_LOG_MAXIMUM_SIZE)
    {
        l_cPacketSize = ERROR_LOG_MAXIMUM_SIZE;
        l_iStringSize = ERROR_LOG_MAXIMUM_SIZE - 
            sizeof( IO_ERROR_LOG_PACKET );
        p_iLen = (l_iStringSize/sizeof(WCHAR))-1;
    }

    PIO_ERROR_LOG_PACKET l_pPacket = (PIO_ERROR_LOG_PACKET)
        IoAllocateErrorLogEntry( 
        s_Impl.m_pDriverObject, l_cPacketSize);

    if(!l_pPacket)
        return false;

    int l_iDumpDataCount = 0;//no data to dump

    //we have only string to report - so set 0 code and value
    l_pPacket->ErrorCode = 0;
    l_pPacket->UniqueErrorValue = 0;
    l_pPacket->MajorFunctionCode = 0;
    l_pPacket->RetryCount = 0;
    l_pPacket->FinalStatus = 0;
    l_pPacket->SequenceNumber = 0;
    l_pPacket->IoControlCode = 0;
    l_pPacket->DumpDataSize = 0;

    l_pPacket->NumberOfStrings = 1;

    l_pPacket->StringOffset = sizeof( IO_ERROR_LOG_PACKET ) +
            ( l_iDumpDataCount - 1 ) * sizeof( ULONG );

    UNICODE_STRING l_ResultString;
    l_ResultString.Length = 0;
    l_ResultString.MaximumLength = l_iStringSize;
    l_ResultString.Buffer = (PWSTR)
        ((PUCHAR)l_pPacket + l_pPacket->StringOffset);
#ifdef UNICODE
#error TODO
#else
    ANSI_STRING l_AnsiString;
    l_AnsiString.Length = p_iLenChars;
    l_AnsiString.MaximumLength = p_iLenChars;
    l_AnsiString.Buffer = (char*)p_pszString;
#endif
    RtlAnsiStringToUnicodeString
        (&l_ResultString, &l_AnsiString, FALSE);
    l_ResultString.Buffer[p_iLen] = 0;
    
    IoWriteErrorLogEntry(l_pPacket);// Log the message
    return true;
}//bool AddToOutputLog

/*
struct MapMem is used by NT drivers to keep information
about each mapping of the trace buffer to an application space
*/
struct MapMem
{
    NTSTATUS MapMemory(PVOID p_pMem, ULONG p_lSize);
    NTSTATUS UnmapMemory(ULONG p_ulUserAddress);
    ULONG ulGetUserAddress() {return m_ulUserAddress;}

    MapMem()
    {
        m_ulSignature = ms_ulSignature;
        m_pMDL = NULL;
        m_ulUserAddress = m_ulKernelAddress = 0;
        m_pProcessMapped = NULL;
    }
    
    ULONG           m_ulSignature;
    struct _MDL  *  m_pMDL;
    ULONG           m_ulUserAddress;
    ULONG           m_ulKernelAddress;
    PEPROCESS       m_pProcessMapped;//for comparison only

    static ULONG    ms_ulSignature;
};//struct MapMem
ULONG MapMem::ms_ulSignature = 
'M' << 24 | 'A' << 16 | 'P' << 8 | 'M';

/*-------------------------------------------------------------
   FUNCTION  MapMem::MapMemory

   PURPOSE: Map the given kernel memory to the address space of
    the current application.
   PARAMETERS:
    PVOID               p_pMem - kernel buffer address
    ULONG               p_lSize - size
   VALUE RETURNED:

      NTSTATUS
           STATUS_SUCCESS  if the view was mapped,
           STATUS_NO_MEMORY if we failed to allocate MDL or map
                the memory.
           STATUS_UNSUCCESSFUL in case of an error or exception
-------------------------------------------------------------*/
NTSTATUS MapMem::MapMemory
(
    PVOID               p_pMem,
    ULONG               p_lSize
)
{
    MDL * l_pMDL = NULL;
    NTSTATUS l_Status = STATUS_UNSUCCESSFUL;
    PVOID l_pUserMem = NULL;

    #ifdef HAVE_EXCEPTIONS
    __try
    {
    #endif
        if(m_pMDL)
        {
            HTRACE(TG_Error, 
                "ERROR : the MapMemory is already in use");
            return STATUS_UNSUCCESSFUL;
        }

        l_pMDL =IoAllocateMdl(p_pMem,p_lSize,FALSE,FALSE,NULL);
        if(!l_pMDL)
        {
            HTRACE(TG_Error, "ERROR : cannot allocate MDL");
            return STATUS_NO_MEMORY;
        }
        MmBuildMdlForNonPagedPool(l_pMDL);

        l_pUserMem = MmMapLockedPages(l_pMDL, UserMode);
        if(l_pUserMem)
        {
            m_pMDL = l_pMDL;
            m_ulUserAddress = (ULONG)l_pUserMem;
            m_ulKernelAddress = ULONG(p_pMem);
            m_pProcessMapped = IoGetCurrentProcess();

            HTRACEK((KeyWordDriverDebug, 
                "Mem Mapped. KrnAddr %x UserAddr %x Size %x",
                m_ulKernelAddress, m_ulUserAddress, p_lSize));

            return STATUS_SUCCESS;
        }
        HTRACE(TG_Error, 
            "ERROR : cannot map a buffer to the user memory");
        l_Status = STATUS_NO_MEMORY;
    #ifdef HAVE_EXCEPTIONS
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        l_Status = STATUS_UNSUCCESSFUL;
    }
    #endif
    if(l_pMDL)
    {
        if(l_pUserMem)
        {
            MmUnlockPages(l_pMDL);
        }
        IoFreeMdl(l_pMDL);
    }
    return l_Status;
}//NTSTATUS MapMem::MapMemory

/*-------------------------------------------------------------

   FUNCTION: MapMem::UnmapMemory

   PURPOSE:  unmap user memory mapping performed by MapMemory

   PARAMETERS:
   ULONG       p_ulUserAddress - for verification (may be NULL)
   VALUE RETURNED:

      NTSTATUS
            STATUS_SUCCESS if the view was unmapped,
            STATUS_NOT_MAPPED_VIEW otherwise,
            STATUS_UNSUCCESSFUL in case of exception
-------------------------------------------------------------*/
NTSTATUS MapMem::UnmapMemory
(
    ULONG   p_ulUserAddress //for verification (may be NULL)
)
{
    NTSTATUS l_Status = STATUS_NOT_MAPPED_VIEW;
    #ifdef HAVE_EXCEPTIONS
    __try
    {
    #endif
        if(m_pMDL)
        {
            if(p_ulUserAddress && 
                m_ulUserAddress != p_ulUserAddress)
            {
                HTRACE(TG_Error, "ERROR : request to unmap "
                    "different address", 0);
                return STATUS_NOT_MAPPED_VIEW;
            }
            if(m_pProcessMapped != IoGetCurrentProcess())
            {
                HTRACE(TG_Error, "ERROR : request to unmap "
                    "on different process");
                return STATUS_NOT_MAPPED_VIEW;
            }

            MmUnmapLockedPages((PVOID)m_ulUserAddress, m_pMDL);

            IoFreeMdl(m_pMDL);

            HTRACEK((KeyWordDriverDebug, 
                "Mem UnMapped. KrnAddr %x UserAddr %x",
                m_ulKernelAddress, m_ulUserAddress));

            m_pMDL = NULL;

            l_Status = STATUS_SUCCESS;
        }//if(m_pMDL)
    #ifdef HAVE_EXCEPTIONS
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        HTRACE(TG_Error,
            "ERROR : exception during memory unmapping");
        l_Status = STATUS_UNSUCCESSFUL;
    }
    #endif
    return l_Status;
}//NTSTATUS MapMem::UnmapMemory

/*-------------------------------------------------------------
   FUNCTION  MapTraceMemory

   PURPOSE:  Map the trace buffer to the address context of the
    current application and store the new MapMem object into
    the given FILE_OBJECT, so we will be able to unmap the 
    memory when the file is closed.
   PARAMETERS:
    PFILE_OBJECT p_pFileObject - file object, which was created 
        specifically for communication with the tracer driver.
        Don't try to call this function on other file objects,
        because it will overwrite FsContext field.
    ULONG   *p_pulReturnUserAddress - return the mapped address
   VALUE RETURNED:

      NTSTATUS
           STATUS_SUCCESS  if the view was mapped,
           STATUS_NO_MEMORY if we failed to allocate MDL or map
                the memory.
           STATUS_UNSUCCESSFUL in case of an error or exception
-------------------------------------------------------------*/
NTSTATUS MapTraceMemory
(
    PFILE_OBJECT p_pFileObject,
    ULONG       *p_pulReturnUserAddress
)
{
    HTRACEK((KeyWordDriverDebug, 
        "MapTraceMemory FileObject %x", p_pFileObject));

    LocalTraceBufferPointers * l_pBuff = pGetTraceBuffer();
    if(!l_pBuff || !l_pBuff->m_pGlobalHeader)
        return STATUS_UNSUCCESSFUL;

    NTSTATUS l_Status = STATUS_SUCCESS;
    ExAcquireFastMutex(&s_Impl.m_FastMutexToProtectMemMap);
    #ifdef HAVE_EXCEPTIONS
    __try
    {
    #endif
        MapMem * l_pMapMem = (MapMem*)p_pFileObject->FsContext;
        if(l_pMapMem == NULL)
        {
            l_pMapMem = (MapMem * )
                ExAllocatePool(PagedPool, sizeof(MapMem));
            l_pMapMem->MapMem::MapMem();//constructor
            p_pFileObject->FsContext = l_pMapMem;
        }
        if(l_pMapMem->m_ulSignature != MapMem::ms_ulSignature)
        {
            HTRACE(TG_Error, 
                "ERROR : bad signature %x in file %x",
                l_pMapMem->m_ulSignature, p_pFileObject);
            l_Status = STATUS_UNSUCCESSFUL;
        }
        else
        {
            l_Status = l_pMapMem->MapMemory(
                l_pBuff->m_pGlobalHeader,
                l_pBuff->m_dwTextAreaSize + 
                TRACE_BUFFER_EXTRA_SIZE);
            if(p_pulReturnUserAddress)
               *p_pulReturnUserAddress=
               l_pMapMem->ulGetUserAddress();
        }
    #ifdef HAVE_EXCEPTIONS
    }//__try
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        HTRACE(TG_Error, 
            "ERROR : exception in MapTraceMemory");
        l_Status = STATUS_UNSUCCESSFUL;
    }
    #endif

    ExReleaseFastMutex(&s_Impl.m_FastMutexToProtectMemMap);
    return l_Status;
}//NTSTATUS MapTraceMemory

/*-------------------------------------------------------------
   FUNCTION  UnmapTraceMemory

   PURPOSE:  Unmap the trace buffer, which was mapped by 
    MapTraceMemory.
   PARAMETERS:
    ULONG        p_ulUserAddress - the address, which was
        returned by MapTraceMemory
    PFILE_OBJECT p_pFileObject - must be the file object, which
       was created for communication with the tracer driver and
       was used in call to MapTraceMemory.
   VALUE RETURNED:
      NTSTATUS
           STATUS_SUCCESS  if the view was mapped,
           STATUS_NO_MEMORY if we failed to allocate MDL or map
                the memory.
           STATUS_UNSUCCESSFUL in case of an error or exception
-------------------------------------------------------------*/
NTSTATUS UnmapTraceMemory
(
    ULONG        p_ulUserAddress,
    PFILE_OBJECT p_pFileObject
)
{
    HTRACEK((KeyWordDriverDebug, 
        "UnmapTraceMemory FileObject %x UserAddr %x", 
            p_pFileObject, p_ulUserAddress));

    if(p_pFileObject->FsContext == NULL)
        return STATUS_SUCCESS;

    NTSTATUS l_Status = STATUS_SUCCESS;
    ExAcquireFastMutex(&s_Impl.m_FastMutexToProtectMemMap);
    #ifdef HAVE_EXCEPTIONS
    __try
    {
    #endif
        MapMem * l_pMapMem = (MapMem*)p_pFileObject->FsContext;
        if(l_pMapMem->m_ulSignature != MapMem::ms_ulSignature)
        {
            HTRACE(TG_Error, 
                "ERROR : bad signature %x in file %x",
                l_pMapMem->m_ulSignature, p_pFileObject);
            l_Status = STATUS_UNSUCCESSFUL;
        }
        else
        {
            l_Status = l_pMapMem->UnmapMemory(p_ulUserAddress);
            l_pMapMem->~MapMem();//destructor
            ExFreePool(l_pMapMem);
            p_pFileObject->FsContext = NULL;
        }
    #ifdef HAVE_EXCEPTIONS
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        HTRACE(TG_Error,"ERROR:exception in UnmapTraceMemory");
        l_Status = STATUS_UNSUCCESSFUL;
    }
    #endif
    ExReleaseFastMutex(&s_Impl.m_FastMutexToProtectMemMap);
    return l_Status;
}//NTSTATUS UnmapTraceMemory

/*-------------------------------------------------------------

   FUNCTION: TraceIoctl

   PURPOSE: This function should be called by driver's dispatch
    routine to allow the tracer to respond to request for 
    mapping and unmapping trace buffer and dumping it to a file
      
   PARAMETERS:                   .
                                  
   IN PDEVICE_OBJECT   p_pDeviceObject        our device object
   IN PIRP             p_pIrp                 request packet
          The following are IOCTL codes for tracer requests.
          Usually these are IOCTL_TRACER_* defined in HTrace.h,
          but the driver may assign different codes.
   IN ULONG p_dwIoctlCheckActive  - check presence of tracer.
              This code will be returned in the output buffer.
   IN ULONG p_dwIoctlDumpTrace    - dump the content of trace
              memory buffer to the file. IOCTL input buffer
              has the name of the file.
   IN ULONG p_dwIoctlMapBuffer    - map the trace memory buffer
              to the user space of the current application and
              return the address in the IOCTL output buffer.
   IN ULONG p_dwIoctlUnMapBuffer  - unmap the trace buffer.
           The IOCTL input buffer should have the user address,
           which was previously returned by p_iIoctlMapBuffer
   RETURN VALUE:
      NTSTATUS 0 on success, error code otherwise
-------------------------------------------------------------*/
NTSTATUS TraceIoctl
( 
    IN PDEVICE_OBJECT   p_pDeviceObject, 
    IN PIRP             p_pIrp,
    IN ULONG            p_dwIoctlCheckActive,
    IN ULONG            p_dwIoctlDumpTrace,
    IN ULONG            p_dwIoctlMapBuffer,
    IN ULONG            p_dwIoctlUnMapBuffer
)
{
    NTSTATUS l_Status;
    PIO_STACK_LOCATION l_pIrpStack = 
        IoGetCurrentIrpStackLocation (p_pIrp);
    PVOID l_pInputBuffer = p_pIrp->AssociatedIrp.SystemBuffer;
    PVOID l_pOutputBuffer = p_pIrp->AssociatedIrp.SystemBuffer;
    ULONG l_ulInputBufferLength = 
     l_pIrpStack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG l_ulOutputBufferLength = 
    l_pIrpStack->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG l_ulIoControlCode = 
        l_pIrpStack->Parameters.DeviceIoControl.IoControlCode;

    l_pIrpStack = IoGetCurrentIrpStackLocation (p_pIrp);

    l_Status = STATUS_NOT_IMPLEMENTED;
    if(l_ulIoControlCode == p_dwIoctlCheckActive)
    {
        if(l_ulOutputBufferLength < sizeof(ULONG))
            return STATUS_BUFFER_TOO_SMALL;

        *((ULONG*)l_pOutputBuffer) = p_dwIoctlCheckActive;
        p_pIrp->IoStatus.Information = sizeof(ULONG);
        l_Status = STATUS_SUCCESS;
    }
    else if(l_ulIoControlCode == p_dwIoctlDumpTrace)
    {
        l_Status = STATUS_UNSUCCESSFUL;
        
        if(l_ulInputBufferLength < sizeof(ULONG))
            return STATUS_BUFFER_TOO_SMALL;
        if(TraceDumpBufferToFile((char*)l_pInputBuffer))
            l_Status = STATUS_SUCCESS;
    }
    else if(l_ulIoControlCode == p_dwIoctlMapBuffer)
    {
        if(l_ulOutputBufferLength < sizeof(ULONG))
            return STATUS_BUFFER_TOO_SMALL;

        *((ULONG*)l_pOutputBuffer) = 0;

        l_Status = MapTraceMemory(l_pIrpStack->FileObject,
                (ULONG*)l_pOutputBuffer);
        if(!NT_SUCCESS(l_Status))
            return l_Status;
        p_pIrp->IoStatus.Information = sizeof(ULONG);
        l_Status = STATUS_SUCCESS;
    }
    else if(l_ulIoControlCode == p_dwIoctlUnMapBuffer)
    {
        if(l_ulInputBufferLength < sizeof(ULONG))
            return STATUS_BUFFER_TOO_SMALL;

        l_Status = UnmapTraceMemory(*((ULONG*)l_pInputBuffer), 
            l_pIrpStack->FileObject);
    }
    else
    {
        l_Status = STATUS_NOT_IMPLEMENTED;
    }
    return l_Status;
}//NTSTATUS TracerIoctl

#endif //#ifdef TRACER_NTDRIVER //}

#if defined(WINAPP) || defined(CE_KERNEL_DRV) //{
/*-------------------------------------------------------------

   FUNCTION: TraceMessageBox

   PURPOSE:  Display the given message in a message box.
          Allow the User to terminate the application, trigger
             a breakpoint or continue the execution.
             This function available for applications only.
      
   PARAMETERS:                   .

    LPCTSTR p_pszMessage,  Message - 0-terminated string
    int     p_iLenChars        Number of characters in the string
  
-------------------------------------------------------------*/
bool TraceMessageBox
(
    ULONG   p_dwMask, 
    LPCTSTR p_pszMessage,
    int     p_iLenChars
)
{
    static long s_lAssertRecursion = -1;
    if (InterlockedIncrement(&s_lAssertRecursion) > 0)
    {
        InterlockedDecrement(&s_lAssertRecursion);
        //don't forget to define 
        //(_WIN32_WINNT >= 0x0400) || (_WIN32_WINDOWS > 0x0400)
#if !defined(_WIN32_WCE)
        if(IsDebuggerPresent())
        {
            __asm int 3
        }
#endif
        return false;
    }//if (InterlockedIncrement(&s_lAssertRecursion) > 0)

#if !defined(_WIN32_WCE)
    TCHAR l_szText[1024];
    TCHAR l_szAddition[] = 
         _T("Press Abort to terminate application,")
         _T("Retry to trigger breakpoint, Ignore to continue");
    int l_iLenBytes = p_iLenChars*sizeof(TCHAR);
    if(l_iLenBytes + sizeof(l_szAddition) < sizeof(l_szText))
    {
        memcpy(l_szText, p_pszMessage, l_iLenBytes);
        memcpy(l_szText + p_iLenChars, l_szAddition, 
            sizeof(l_szAddition));
        p_pszMessage = l_szText;
    }
#endif

#ifdef WINAPP
    HWND l_hWndParent = GetActiveWindow();
#else
    HWND l_hWndParent = 0;
#endif
    DWORD l_dwStyles;
#if defined(_WIN32_WCE)
    l_dwStyles = MB_TOPMOST | MB_SETFOREGROUND;
#else
    if (l_hWndParent != NULL)
        l_hWndParent = GetLastActivePopup(l_hWndParent);
    l_dwStyles = MB_TASKMODAL|MB_ABORTRETRYIGNORE|MB_SETFOREGROUND;
#endif

    int l_iRet = MessageBox(l_hWndParent, p_pszMessage,
        _T("Trace Message"), l_dwStyles);
    InterlockedDecrement(&s_lAssertRecursion);
#ifdef WINAPP
    switch(l_iRet) {
    case IDABORT:
        TerminateProcess(GetCurrentProcess(),0);
        return true;
    case IDRETRY:
#if defined(_WIN32_WCE)
        DebugBreak();
#else
        __asm int 3
#endif
        return false;
    default:
    case IDIGNORE:
        return false;
    }
#else
    return true;
#endif
}//bool TraceMessageBox

#if !defined(_WIN32_WCE)
bool OutputConsole(ULONG p_dwMask,LPCSTR p_pszBuff,int p_iLen)
{
    //puts(p_pszBuff);
    _cputs(p_pszBuff);
    return true;
}
#endif

#endif //#if defined(WINAPP) || defined(CE_KERNEL_DRV) }

#ifndef WIN32
bool TraceMessageBox
    (ULONG p_dwMask,LPCSTR p_pszBuff,int p_iLen)
{//poor emulation of message box using console
    puts(p_pszBuff);
    puts("Press any key");
    getchar();
    return true;
}
bool OutputConsole
    (ULONG p_dwMask, LPCSTR p_pszBuff, int p_iLen)
{
    puts(p_pszBuff);
    return true;
}
#endif

#ifdef TRACE_JNI  //{
//These are JNI routines, defined by HTrace.java class.
extern "C" 
{
JNIEXPORT void JNICALL Java_HTrace_HTraceImpl
(JNIEnv * l_pEnv, jclass, jint p_iMask, jstring p_strMessage)
{
    const char * l_pszString = 
        l_pEnv->GetStringUTFChars(p_strMessage, NULL);
    if(!l_pszString)
        return;
    OutputTraceString(p_iMask, l_pszString, 
        strlen(l_pszString));
    l_pEnv->ReleaseStringUTFChars(p_strMessage, l_pszString);
}

JNIEXPORT void JNICALL Java_HTrace_TraceAssignGroupsToStream
  (JNIEnv *, jclass, 
  jint p_dwTraceOutputs, jint p_dwNewGroupFlags, 
  jint p_dwGroupFlagsToModify)
{
    TraceAssignGroupsToStream(p_dwTraceOutputs, 
        p_dwNewGroupFlags, p_dwGroupFlagsToModify);
}

static JNIEnv * s_pEnv = NULL;
static jobject  s_DebugClass = NULL;
JNIEXPORT void JNICALL Java_HTrace_InitTrace
  (JNIEnv * l_pEnv, jclass p_Class)
{
    s_pEnv = l_pEnv;
    s_DebugClass = s_pEnv->NewGlobalRef(p_Class);

    TraceInitialize();

    JNISetTotalMask(*s_Impl.m_pdwTotalMask, 
        s_Impl.m_dwConsoleMask);
}

void JNISetTotalMask(ULONG p_dwTotalMask, ULONG p_ulConsole)
{
    if(s_pEnv != NULL && s_DebugClass != NULL)
    {
        jfieldID l_FieldID = s_pEnv->GetStaticFieldID
            ((jclass)s_DebugClass, "ms_iTotalMask", "I");
        if(l_FieldID != NULL)
        {
            s_pEnv->SetStaticIntField(
                (jclass)s_DebugClass, l_FieldID, 
                (int)p_dwTotalMask);
        }
        l_FieldID = s_pEnv->GetStaticFieldID
            ((jclass)s_DebugClass, "ms_iConsoleMask", "I");
        if(l_FieldID != NULL)
        {
            s_pEnv->SetStaticIntField(
                (jclass)s_DebugClass, l_FieldID, 
                (int)p_ulConsole);
        }
    }
}//void JNISetGlobalMask


}//extern "C" 
#endif//#ifdef TRACE_JNI  }

bool OutputDebugMonitor
(
    ULONG p_dwMask,
    LPCTSTR p_pszMessage,
    int p_iLen
)
{
    #ifdef TRACER_NTDRIVER
        DbgPrint((char*)p_pszMessage);
    #elif defined(WIN32) && !defined(CE_KERNEL_DRV)
        OutputDebugString(p_pszMessage);
    #elif defined(__linux__)
        syslog(LOG_DEBUG, p_pszMessage);
    #endif
    return true;
}

bool OutputFile(ULONG p_dwMask,LPCTSTR p_pszMessage, int p_iLenChars)
{
#ifdef HAVE_FILE_OUTPUT //{
    s_Impl.m_File.ReadWriteFile(true, 
        (void*)p_pszMessage, p_iLenChars*sizeof(TCHAR), NULL, NULL);

    if(s_Impl.m_dwFlushFileMask & p_dwMask)
        s_Impl.m_File.FlushBuffers();
    return true;
#else
    return false;
#endif
}

/*-------------------------------------------------------------

   FUNCTION: OutputTraceStringUnconditional
   PURPOSE:Unlike OutputTraceString, it prints the given string
    to all outputs, whose IDs are included in p_dwMask.
   PARAMETERS:                   .
                        
    ULONG   p_dwOutputMask   flags from TraceOutputs
    LPCSTR  p_pszMessage,    Message - 0-terminated string
    int     p_iLenChars      Number of characters in the string
-------------------------------------------------------------*/
void OutputTraceStringUnconditional
(
    ULONG   p_dwOutputMask,
    LPCTSTR p_pszMessage,
    int     p_iLenChars
)
{
    for(int i = 0; i < NUM_STREAMS;i++)
    {
        TraceOutput & l_rOutput = s_Outputs[i];
        if(l_rOutput.m_dwOutputID & p_dwOutputMask)
        {
            l_rOutput.m_pOutputFunction(0,p_pszMessage, p_iLenChars);
        }
    }
}//void OutputTraceStringUnconditional

/*-------------------------------------------------------------

   FUNCTION: OutputTraceString

   PURPOSE: Print the given 0-terminated ANSI string to the one
    or several of the trace outputs (such as memory buffer, 
    debug monitor,etc.) 
    The destination is controlled by bit masks for the
    particular output and the given mask for this message.
    Do nothing if the specified mask does not match the
    enabled trace masks.
      
   PARAMETERS:                   .
                        
    ULONG p_dwMask          flags from TraceGroups
    LPCTSTR p_pszMessage,    string
    int     p_iLen          Number of characters in the string
-------------------------------------------------------------*/
void OutputTraceString
(
    ULONG   p_dwMask,
    LPCTSTR p_pszMessage,
    int     p_iLen
)
{
    for(int i = 0; i < NUM_STREAMS;i++)
    {
        TraceOutput & l_rOutput = s_Outputs[i];
        if(l_rOutput.m_dwOutputID == TO_MemoryBuffer &&
           s_Impl.m_BufferPointers.m_pGlobalFooter != NULL)
        {
            if(s_Impl.m_BufferPointers.m_pGlobalFooter->
                m_dwEnabledGroups & p_dwMask)
            {
                l_rOutput.m_pOutputFunction
                    (p_dwMask, p_pszMessage, p_iLen);
            }
        }
        else if(l_rOutput.m_dwEnabledGroups & p_dwMask)
        {
           l_rOutput.m_pOutputFunction
               (p_dwMask,p_pszMessage,p_iLen);
        }
    }
}//void OutputTraceString

/*-------------------------------------------------------------

   FUNCTION: TraceKeywordCheck::CompareKeyWords

   PURPOSE:Compare the given keyword with keyword masks of 
   streams, which were updated since we compared the last time.
-------------------------------------------------------------*/
void TraceKeywordCheck::CompareKeyWords
(
    const char * p_pszKeyWord
)
{
    for(int i = 0; i < NUM_STREAMS;i++)
    {
        TraceOutput & l_rOutput = s_Outputs[i];
        
        ULONG l_dwKeyWordModificationCounter = 
            l_rOutput.m_dwKeyWordModificationCounter;
        char * l_pszKeyWordMask =
            l_rOutput.m_szEnabledGroupsKeywords;

        if(l_rOutput.m_dwOutputID == TO_MemoryBuffer &&
           s_Impl.m_BufferPointers.m_pGlobalFooter != NULL)
        {
            GlobalTraceBufferFooter * l_Footer = 
                s_Impl.m_BufferPointers.m_pGlobalFooter;

            l_dwKeyWordModificationCounter = 
                l_Footer->m_dwKeyWordModificationCounter;
            l_pszKeyWordMask = l_Footer->m_szKeyWordMask;
        }

        if(l_dwKeyWordModificationCounter - 
            m_dwModificationCounter > 0)
        {//This stream has an updated keyword
            if(strstr(l_pszKeyWordMask, p_pszKeyWord))
            {//Enabled for this stream
                m_dwEnabledStreams |= l_rOutput.m_dwOutputID;
            }
            else
            {//Disabled for this stream
                m_dwEnabledStreams &= ~l_rOutput.m_dwOutputID;
            }
        }
    }
}//void TraceKeywordCheck::CompareKeyWords

/*-------------------------------------------------------------

   FUNCTION: TraceKeywordCheck::Output

   PURPOSE:  Perform printf style formatting and tracing.
    Do nothing if the specified string keyword does not match
    enabled trace keyword masks.
    This function is similar to HTraceImpl, but it has to be
    a member of TraceKeywordCheck, which keeps m_dwEnabled
    variable to allow us to avoid making expensive string match
  on every call, but only after global keyword mask is changed.
      
   PARAMETERS:                   .
                                  
   const char * p_pszKey         string keyword 
   LPCSTR p_pszFormat            printf style format
   ...                           parameters
-------------------------------------------------------------*/
void TraceKeywordCheck::Output
(
    char *  p_pszKey, 
    LPCTSTR p_pszFormat, ...
)
{
    if(s_dwKeyWordModifCounter
      != m_dwModificationCounter)
    {
        //At least one output 
        //stream has a new keyword
        //mask - need to compare
        //our keyword with all 
        //modified keyword masks 
        //of streams.
        long l_lCounter = 
           s_dwKeyWordModifCounter;
        CompareKeyWords(p_pszKey);
        m_dwModificationCounter = 
            l_lCounter;
    }
    if(!m_dwEnabledStreams)
        return;//This keyword is 
        //not enabled for any stream
    
    va_list l_List;
    va_start(l_List, p_pszFormat);
    TCHAR l_szBuff[512];
    
    *l_szBuff = 0;
    _vsntprintf(l_szBuff, sizeof(l_szBuff)/sizeof(l_szBuff[0]), 
        p_pszFormat, l_List);

    va_end(l_List);

    l_szBuff[sizeof(l_szBuff)/sizeof(l_szBuff[0])-2] = 0;

    int l_iLen = _tcslen(l_szBuff);
    if(l_iLen > 0 && l_szBuff[l_iLen-1] != '\n')
    {
        //Add the new line character if it is not already there
        l_szBuff[l_iLen++] = '\r';
        l_szBuff[l_iLen++] = '\n';
        l_szBuff[l_iLen] = 0;
    }
    OutputTraceStringUnconditional
        (m_dwEnabledStreams, l_szBuff, l_iLen);
}//void TraceKeywordCheck::Output

/*-------------------------------------------------------------

   FUNCTION: HTraceImpl

   PURPOSE:  Perform printf style formatting and tracing.
    Do nothing if the specified mask does not match the
    enabled trace masks.
      
   PARAMETERS:                   .
                                  
   ULONG p_dwMask                 flags from TraceGroups
   LPCTSTR p_pszFormat            printf style format
   ...                            
-------------------------------------------------------------*/
void HTraceImpl(ULONG p_dwMask, LPCTSTR p_pszFormat, ...)
{
    //Check whether this group of traces is enabled.
    if(!(*s_Impl.m_pdwTotalMask & p_dwMask))
        return;//Skip this trace

    va_list l_List;
    va_start(l_List, p_pszFormat);
    TCHAR l_szBuff[512];
    
    *l_szBuff = 0;
    int l_iAddLen = 0;
    if(s_Impl.m_bAddTime)
    {
        #if defined(TRACER_NTDRIVER)
            TIME_FIELDS l_TimeFields;
            LARGE_INTEGER l_Time;
            KeQuerySystemTime(&l_Time);
            RtlTimeToTimeFields(&l_Time, &l_TimeFields);
            _snprintf(l_szBuff + l_iAddLen, 
                sizeof(l_szBuff)/sizeof(l_szBuff[0]) - 
                l_iAddLen,
                "%d:%d:%d ",  l_TimeFields.Hour, 
                l_TimeFields.Minute, 
                l_TimeFields.Milliseconds);
            l_iAddLen += strlen(l_szBuff + l_iAddLen);
        #elif defined(_WIN32_WCE)
            ULONG l_ulTick = GetTickCount();

            _itot(l_ulTick, l_szBuff + l_iAddLen, 10);
            l_iAddLen += _tcslen(l_szBuff + l_iAddLen);
            l_szBuff[l_iAddLen++] = ':';
        #else
            //WARNING: these functions may be slow
            struct tm * l_pLocalTime;
            time_t      l_Time;
            char * l_pszTime;
        
            time(&l_Time);
            l_pLocalTime = localtime(&l_Time);
            l_pszTime = asctime(l_pLocalTime);

            ULONG l_ulTick = GetTickCount();

            int l_iLen = strlen(l_pszTime);
            memcpy(l_szBuff + l_iAddLen, l_pszTime, l_iLen);
            l_iAddLen += l_iLen;
            l_szBuff[l_iAddLen++] = ' ';

            _itoa(l_ulTick, l_szBuff + l_iAddLen, 10);
            l_iAddLen += strlen(l_szBuff + l_iAddLen);
            l_szBuff[l_iAddLen++] = ':';
        #endif
    }
    if(s_Impl.m_bAddThread)
    {
        int l_iThreadID;
        #if defined(TRACER_NTDRIVER)
            l_iThreadID = (int)KeGetCurrentThread();
        #elif defined(WIN32)
            l_iThreadID = GetCurrentThreadId();
        #elif defined(__linux__)
            l_iThreadID = getpid();
        #endif
        _itot(l_iThreadID, l_szBuff + l_iAddLen, 10);
        l_iAddLen += _tcslen(l_szBuff + l_iAddLen);
        l_szBuff[l_iAddLen++] = ':';
    }
    _vsntprintf(l_szBuff + l_iAddLen, 
        sizeof(l_szBuff)/sizeof(l_szBuff[0]) - l_iAddLen,
        p_pszFormat, l_List);
    va_end(l_List);
    l_szBuff[sizeof(l_szBuff)/sizeof(l_szBuff[0])-2] = 0;

    int l_iLen = _tcslen(l_szBuff);
    if(l_iLen > 0 && l_szBuff[l_iLen-1] != '\n')
    {
        //Add the new line character if it is not already there
        #ifdef WIN32
        l_szBuff[l_iLen++] = '\r';
        #endif
        l_szBuff[l_iLen++] = '\n';
        l_szBuff[l_iLen] = 0;
    }
    OutputTraceString(p_dwMask, l_szBuff, l_iLen);
    
}//void HTraceImpl

#endif //TRACE_ }
