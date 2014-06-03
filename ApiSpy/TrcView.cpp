/********************************************************************
Module : TrcView.cpp      - trace buffer viewer
             Written 2000,2003 by Dmitri Leman
    for an article in C/C++ journal about a tracing framework.
Purpose: A part of the tracer framework.
********************************************************************/
#ifdef TRACE_ //{

#if defined WIN32
#include <windows.h>
#include <stdio.h>
#include "HTrace.h"

#elif defined(_WIN32_WCE)

#include <windows.h>
#include "resource.h"
#include "../common/HTrace.h"

#elif defined(__linux__)

#include <unistd.h>
#include <curses.h>
#include <malloc.h>
#include <string.h>
#include <sys/time.h>
#include "HTrace.h"

//This module was ported from WIN32 to ncurses.
//Therefore, I included few declarations, which allows
//some WIN32-specific to compile under linux.
typedef WINDOW * HDC;
typedef WINDOW * HWND;
typedef int BOOL;
#define __int64 long long
typedef struct {int left, top, right, bottom;} RECT;
void DrawText(
    WINDOW * p_pWindow, const char * p_pszBuffer,
    int p_iNumBytes, RECT * p_pPaintRect, int p_iFlags)
{
    int l_iRes = 
        mvwaddnstr(stdscr, 0,0, p_pszBuffer, p_iNumBytes);
    wclrtobot(p_pWindow);
}
static ULONG GetTickCount()
{
    clock_t l_Clock = clock();
    return (ULONG)(((__int64)l_Clock) * 1000 / CLOCKS_PER_SEC);
}
#define SB_VERT 1
#define SB_THUMBTRACK 1
#define SB_LINEUP     2
#define SB_LINEDOWN   3
#define SB_PAGEUP     4
#define SB_PAGEDOWN   5
#define SB_TOP        6
#define SB_BOTTOM     7

#define _snprintf snprintf
#endif


static const char s_szCaption[] = "Trace Viewer";

static char s_szFullINIPath[_MAX_PATH];

#define MAX_ROW_LEN     256
#define NUM_EXTRA_ROWS  2

struct BufferViewer
{
    LocalTraceBufferPointers * m_pBufferPointers;

    HWND    m_hWnd;

#ifdef WIN32
    HFONT   m_hFont;
    int     m_iFontHeight;
    int     m_iFontWidth;
    int     m_iLogicalWidthChars;
    int     m_iLogicalWidthPixels;
#endif

    LPTSTR  m_pBuffer;
    int     m_iBufferSize;
    int     m_iNumLines;
    int     m_iFirstPaintIndex;
    int     m_iNumBytesPainted;
    int     m_iFirstViewPosWhenPainted;
    int     m_iNumLinesPainted;
    int     m_iFirstRowLength;
    int     m_iTotalNumCharsWhenPainted;
    int     m_iScrollChanged;
    int     m_iScrollChangedWhenPainted;

    int     m_iHorzPos;
    int     m_iCharShownFirst;
    int     m_iLineOffset;
    bool    m_bStickToEnd;
    bool    m_bMoveToStartNextTime;

    RECT    m_ClientRect;
    RECT    m_PaintRect;
    int     m_iNumLinesInClientArea;

    BufferViewer(HWND p_hWnd)
    {
        m_hWnd = p_hWnd;
        m_pBufferPointers = NULL;
        #ifdef WIN32
        m_hFont = 0;
        m_iFontHeight = 0;
        m_iFontWidth = 0;
        m_iLogicalWidthChars = 0;
        m_iLogicalWidthPixels = 0;
        #endif
        m_pBuffer = NULL;
        m_iBufferSize = 0;
        m_iNumLines = 0;
        m_iFirstPaintIndex = -1;
        m_iNumBytesPainted = -1;
        m_iFirstViewPosWhenPainted = -1;
        m_iTotalNumCharsWhenPainted = -1;
        m_iNumLinesPainted = 0;
        m_iFirstRowLength = 0;
        m_iScrollChanged = 0;
        m_iScrollChangedWhenPainted = -1;
        m_iHorzPos = 0;
        m_iCharShownFirst = 0;
        m_iLineOffset = 0;
        m_bStickToEnd = true;
        m_bMoveToStartNextTime = false;
        m_iNumLinesInClientArea = 0;
        #ifdef __linux__
        m_iInvalidated = 0;
        m_iScrollMin = 0;
        m_iScrollMax = 0;
        m_iScrollPos = 0; 
        m_pWndStatus = NULL;
        m_pWndHelp = NULL;             
        #endif
    }
    void PaintBufferWithCopy
    (
        HDC  p_hDC,
        LPCTSTR p_pTraceBuffer,
        int p_lBufferSizeChars,
        int p_iFirstViewPos,
        int p_iLinesOffset,
        int p_iDataSizeAfterScrollPos, 
        int p_iDataSizeBeforeScrollPos
    );
    void PaintBuffer
    (
        int                 p_iFirstViewPos,
        int                 p_iLinesOffset,
        HDC                 p_hDC,
        RECT              * p_pPaintRect
    );
    void UpdateScrollPos();
    void OnPaint();
    void OnSizeChanges();
    void OnTimerMessage();
    void OnScrollMessage
    (
        int p_nScrollBarType,
        int p_nScrollCode,
        int p_nPos,
        int p_nTrackPos,
        int p_nMin,
        int p_nMax
    );
    #ifdef __linux__
    //Few more routines to allow portions of WIN32 specific
    //code to run under ncurses
    void InvalidateRect(HWND,RECT*,BOOL)
    {
        m_iInvalidated++;
    }
    void GetScrollRange(HWND, int, int * p_piMin,int * p_piMax)
    {
        *p_piMin = m_iScrollMin;
        *p_piMax = m_iScrollMax;
    } 
    void SetScrollRange(HWND, int,int p_iMin, int p_iMax,BOOL)
    {
        m_iScrollMin = p_iMin;
        m_iScrollMax = p_iMax;    
    }
    void SetScrollPos(HWND, int, int p_iPos,BOOL)
    {
        m_iScrollPos = p_iPos;
    }       
    int m_iInvalidated;
    int m_iScrollMin;
    int m_iScrollMax;
    int m_iScrollPos;
    WINDOW * m_pWndStatus;
    WINDOW * m_pWndHelp;
    #endif
};//struct BufferViewer

/*-------------------------------------------------------------

   FUNCTION: BufferViewer::PaintBufferWithCopy

   PURPOSE:  Worker routine to copy a portion of a trace buffer
    to a local display buffer and paint it to the screen.
   PARAMETERS:
    HDC  p_hDC                  - device context
    LPCTSTR p_pTraceBuffer - the whole circular buffer
    int p_lBufferSizeChars - total size of the buffer
    int p_iFirstViewPos    - current scroll position
    int p_iLinesOffset     - count p_iLinesOffset lines up or down
                            from the current scroll position to
                            find the first line to paint.
    int p_iDataSizeAfterScrollPos  - number of valid bytes in the 
                            buffer before p_iCurByte
    int p_iDataSizeBeforeScrollPos - number of valid bytes in the 
                            buffer after p_iCurByte
-------------------------------------------------------------*/
void BufferViewer::PaintBufferWithCopy
(
    HDC  p_hDC,
    LPCTSTR p_pTraceBuffer,
    int p_lBufferSizeChars,
    int p_iFirstViewPos,
    int p_iLinesOffset,
    int p_iDataSizeAfterScrollPos, 
    int p_iDataSizeBeforeScrollPos
)
{
    int l_iSize = (m_iNumLinesInClientArea + NUM_EXTRA_ROWS) * 
        MAX_ROW_LEN;
    if(m_iNumLines < m_iNumLinesInClientArea || !m_pBuffer)
    {
        //The worst uncertainty is the line width.
        //We will assume MAX_ROW_LEN = 256 byte line width.
        //Hopefully, most of lines will be shorter.
        //If most of lines will be longer than 256,
        //we will print fewer lines, than fit in window
        LPTSTR l_pBuff = (LPTSTR)malloc(l_iSize*sizeof(TCHAR));
        if(l_pBuff == NULL)
        {
            HTRACE(TG_Error, 
                _T("ERROR: malloc(%d) failed"), l_iSize);
            return;
        }
        free(m_pBuffer);
        m_pBuffer = l_pBuff;
        m_iBufferSize = l_iSize;
        m_iNumLines = m_iNumLinesInClientArea;
    }//if(m_iNumLines < m_iNumLinesInClientArea)

    //Now we need to copy memory from the trace buffer to the
    //screen buffer.
    //We need to start before the current position to find the 
    //beginning of the line, which contains the current 
    //position.
    int l_iLookBack = p_iLinesOffset < -1? 
        (-p_iLinesOffset)*MAX_ROW_LEN : MAX_ROW_LEN;
    if(l_iLookBack + p_iDataSizeAfterScrollPos < l_iSize)
    {  
        //If we have not enough data after the scroll point,
        //we need to look farther behind to avoid leaving blank
        //space (basically this means that the very last line should
        //be displayed at the bottom of the screen - not at the top)
        l_iLookBack = l_iSize - p_iDataSizeAfterScrollPos;
    }
    if(l_iLookBack > p_iDataSizeBeforeScrollPos)
       l_iLookBack = p_iDataSizeBeforeScrollPos;

    int l_iStartFromByteWithExtra = 
        (p_iFirstViewPos - l_iLookBack) % p_lBufferSizeChars;
    int l_iDisplaySize = p_iDataSizeAfterScrollPos + l_iLookBack;
    if(l_iDisplaySize > l_iSize)
       l_iDisplaySize = l_iSize;
    int l_iLookForward = l_iDisplaySize;
    if(l_iStartFromByteWithExtra + l_iLookForward > 
        p_lBufferSizeChars)
    {
        l_iLookForward = p_lBufferSizeChars - 
            l_iStartFromByteWithExtra;
        memcpy(m_pBuffer, p_pTraceBuffer + 
            l_iStartFromByteWithExtra, l_iLookForward*sizeof(TCHAR));
        //The display area continues at the beginning of buffer
        memcpy(m_pBuffer + l_iLookForward, p_pTraceBuffer, 
            (l_iDisplaySize - l_iLookForward)*sizeof(TCHAR));
    }
    else
    {
        memcpy(m_pBuffer, p_pTraceBuffer + 
            l_iStartFromByteWithExtra, l_iLookForward*sizeof(TCHAR));
    }

    int i;
    int l_iNumLines = 0;
    int l_iBeginning = l_iLookBack;

    //First count lines after the current byte, which is at the
    //position l_iLookBack in the m_pBuffer
    int l_iEnd = l_iDisplaySize;//if we will not find newlines
    int l_iFirstLineEnd = l_iEnd;
    for(i = l_iLookBack; i < l_iDisplaySize; i++)
    {
        if(m_pBuffer[i] == '\n')
        {
            l_iEnd = i;
            if(l_iNumLines == 0)
            {
                l_iFirstLineEnd = i;
            }
            l_iNumLines++;
            if(l_iNumLines >= m_iNumLinesInClientArea)
                break;
        }
    }
    //Next look back until we find the beginning of the line,
    //which the current position belongs to. And look further
    //back if we have not enough lines already to fill screen.
    int l_iCountLinesBefore = p_iLinesOffset < 0? 
        -p_iLinesOffset : 1;
    for(i = l_iLookBack-1; i >= 0; i--)
    {
        if(m_pBuffer[i] == '\n')
        {
            l_iBeginning = i+1;
            l_iNumLines++;
            if(--l_iCountLinesBefore <= 0 &&
                l_iNumLines >= m_iNumLinesInClientArea)
                break;
            l_iFirstLineEnd = i;
        }
    }
    if(i < 0)
    {//Didn't find any newlines before the current position.
        l_iBeginning = 0;
    }

    m_iFirstPaintIndex = l_iBeginning;
    m_iNumBytesPainted = l_iEnd - l_iBeginning;
    DrawText(p_hDC, m_pBuffer + m_iFirstPaintIndex, 
                    m_iNumBytesPainted, &m_PaintRect, 0);

    m_iCharShownFirst = p_iFirstViewPos - l_iLookBack + l_iBeginning;
    m_iLineOffset = 0;
    m_iFirstViewPosWhenPainted = m_iCharShownFirst;
    m_iNumLinesPainted = l_iNumLines;
    m_iFirstRowLength = 1 + l_iFirstLineEnd - l_iBeginning;
}//void BufferViewer::PaintBufferWithCopy

/*------------------------------------------------------------

   FUNCTION: BufferViewer::PaintBuffer

   PURPOSE:  Performs actual painting of the trace buffer
   PARAMETERS:
    p_iFirstViewPos - current byte (between 0 and the buffer 
        size) to be shown at the top of the window (measured 
        from the initial start of writing)
    p_iLinesOffset     - count p_iLinesOffset lines up or down
                            from the current scroll position to
                            find the first line to paint.
------------------------------------------------------------*/
void BufferViewer::PaintBuffer
(
    int                 p_iFirstViewPos,
    int                 p_iLinesOffset,
    HDC                 p_hDC,
    RECT              * p_pPaintRect
)
{
    if(!m_pBufferPointers || !m_pBufferPointers->m_pGlobalFooter ||
        !m_pBufferPointers->m_dwTextAreaSize)
        return;
    int   l_iCharsWritten = 
        m_pBufferPointers->m_pGlobalFooter->m_dwNumBytesWritten /
        sizeof(TCHAR);
    int   l_iBufferSizeChars = 
        m_pBufferPointers->m_dwTextAreaSize / sizeof(TCHAR);

    if(p_iFirstViewPos > l_iCharsWritten)
        p_iFirstViewPos = l_iCharsWritten;
        //don't show empty space

    LPTSTR l_pText = m_pBufferPointers->m_pTextArea;

    //Decide which byte in the trace buffer corresponds to the
    //scroll position. This will be the last line to be shown.

    int l_iDataSizeAfterScrollPos= l_iCharsWritten - p_iFirstViewPos;
    if(l_iDataSizeAfterScrollPos > l_iBufferSizeChars)
    {
        //The scroll position is behind the beginning of the 
        //valid data in the buffer (since the scroll position was
        //moved there the data were overwritten).
        if(m_iFirstViewPosWhenPainted == p_iFirstViewPos &&
           m_iFirstPaintIndex >= 0 && m_pBuffer != NULL)
        {
            //Fortunately, we already have the data in the 
            //display buffer
            DrawText(p_hDC, m_pBuffer + m_iFirstPaintIndex, 
                        m_iNumBytesPainted, p_pPaintRect, 0);
        }
        else
        {
            DrawText(p_hDC, _T("Data Lost"), 9, p_pPaintRect, 0);
        }
    }
    else
    {
        int l_iDataSizeBeforeScrollPos;
        if(l_iCharsWritten > l_iBufferSizeChars)
           l_iDataSizeBeforeScrollPos = 
            l_iBufferSizeChars - l_iDataSizeAfterScrollPos;
        else
           l_iDataSizeBeforeScrollPos = p_iFirstViewPos;

        PaintBufferWithCopy(
            p_hDC, l_pText, l_iBufferSizeChars, 
            p_iFirstViewPos, p_iLinesOffset,
            l_iDataSizeAfterScrollPos, l_iDataSizeBeforeScrollPos);
    }
    m_iTotalNumCharsWhenPainted = l_iCharsWritten;
    m_iScrollChangedWhenPainted = m_iScrollChanged;
}//void BufferViewer::PaintBuffer

#ifdef WIN32 //{
/*-------------------------------------------------------------

   FUNCTION: BufferViewer::OnPaint

   PURPOSE:  Called to paint screen buffer view window
-------------------------------------------------------------*/
void BufferViewer::OnPaint()
{
    PAINTSTRUCT l_Paint;
    BeginPaint(m_hWnd, &l_Paint);
    FillRect(l_Paint.hdc, &m_ClientRect, 
        (HBRUSH) GetStockObject(WHITE_BRUSH));

    if(m_pBufferPointers && m_pBufferPointers->m_pGlobalFooter)
    {
        HGDIOBJ l_hPrevFont = SelectObject
            (l_Paint.hdc, m_hFont);
        if(!m_iFontHeight)
        {
            SIZE l_Size;
            m_iFontHeight = GetTextExtentPoint32(
                l_Paint.hdc, _T("W"), 4, &l_Size);
            m_iFontHeight = l_Size.cy;
            m_iFontWidth  = l_Size.cx;
            m_iLogicalWidthPixels = m_iLogicalWidthChars * 
                m_iFontWidth;

            OnSizeChanges();
        }
        //long l_lVertPos = GetScrollPos(m_hWnd, SB_VERT);
       
        int l_iOldestCharInBuffer = ((int)
            (m_pBufferPointers->m_pGlobalFooter->m_dwNumBytesWritten-
           m_pBufferPointers->m_dwTextAreaSize))/(int)sizeof(TCHAR);
        if(m_bMoveToStartNextTime && 
            m_iCharShownFirst < l_iOldestCharInBuffer)
        {
            m_iCharShownFirst = l_iOldestCharInBuffer;
        }
        m_bMoveToStartNextTime = false;
        SetViewportOrgEx(l_Paint.hdc, -m_iHorzPos, 0, NULL);
        
        FillRect(l_Paint.hdc, &m_PaintRect, 
            (HBRUSH) GetStockObject(WHITE_BRUSH));
        PaintBuffer(m_iCharShownFirst, m_iLineOffset,
            l_Paint.hdc, &m_PaintRect);
        SelectObject(l_Paint.hdc, l_hPrevFont);
    }

    EndPaint(m_hWnd, &l_Paint);
    
    UpdateScrollPos();
}//void BufferViewer::OnPaint()

/*-------------------------------------------------------------

   FUNCTION: BufferViewer::OnSizeChanges

   PURPOSE:  Called in response to WM_SIZE
-------------------------------------------------------------*/
void BufferViewer::OnSizeChanges()
{
    GetClientRect(m_hWnd, &m_ClientRect);
    m_iNumLinesInClientArea = 0;
    if(m_iFontHeight > 0)
    {
        m_iNumLinesInClientArea = 
        (m_ClientRect.bottom - m_ClientRect.top)/m_iFontHeight;
    }
    m_PaintRect = m_ClientRect;
    m_PaintRect.right = m_ClientRect.right + m_iHorzPos;
    InvalidateRect(m_hWnd, NULL, FALSE);
}//void BufferViewer::OnSizeChanges()
#endif//WIN32 }

/*-------------------------------------------------------------

   FUNCTION: BufferViewer::OnTimerMessage

   PURPOSE:  Called in response to WM_TIMER message in the 
    buffer view window. Calculates a new scroll position and 
    forces repaint
------------------------------------------------------------*/
void BufferViewer::OnTimerMessage()
{
    if(!m_pBufferPointers || !m_pBufferPointers->m_pGlobalFooter)
        return;

    int l_iCharsWritten = 
        m_pBufferPointers->m_pGlobalFooter->m_dwNumBytesWritten / 
        sizeof(TCHAR);
    if(m_iTotalNumCharsWhenPainted == l_iCharsWritten &&
       m_iScrollChangedWhenPainted == m_iScrollChanged)
        return;//Nothing new was written since last paint
    UpdateScrollPos();
    if(m_iCharShownFirst == m_iFirstViewPosWhenPainted &&
       m_iNumLinesPainted >= m_iNumLinesInClientArea &&
       m_iLineOffset == 0)
    {
       return;//don't repaint the same place again
    }
    InvalidateRect(m_hWnd, NULL, FALSE);
}//void BufferViewer::OnTimerMessage

void BufferViewer::UpdateScrollPos()
{
    int l_iBufferSizeChars = m_pBufferPointers->m_dwTextAreaSize / 
        sizeof(TCHAR);
    int l_iCharsWritten = 
        m_pBufferPointers->m_pGlobalFooter->m_dwNumBytesWritten / 
        sizeof(TCHAR);

    int l_iRange = 0;
    int l_iScrollPos = 0;
    //We will set the scroll range depending where the 
    //current viewing point (m_iCharShownFirst) is. If it
    //is within the memory buffer, we need to set the range
    //to the size of the buffer (or the number of bytes 
    //written, if the buffer is not full yet). If the 
    //current viewing point is behind the buffer (the data 
    //were lost), we need to set the range to the whole 
    //distance between the newest data and the view point.

    if(l_iCharsWritten < l_iBufferSizeChars)
    {//The buffer is not full yet.
        l_iRange = l_iCharsWritten;
        l_iScrollPos = m_iCharShownFirst;
    }
    else
    {//The buffer is full
        int l_iFirstByteInBuffer = 
            l_iCharsWritten - l_iBufferSizeChars;
        if(m_iCharShownFirst < l_iFirstByteInBuffer)
        {   //The view is behind the oldest data in the 
            //buffer. Set the range to the whole distance 
            //between the newest data and the view point.
            l_iRange = l_iCharsWritten - m_iCharShownFirst;
            l_iScrollPos = 0;
        }
        else
        {
            //The view point is within the buffer.
            //The range should be equal to the buffer size.
            l_iRange = l_iBufferSizeChars;
            l_iScrollPos = m_iCharShownFirst - l_iFirstByteInBuffer;
        }
    }
    if(m_bStickToEnd)
    {
        l_iScrollPos = l_iRange;
        m_iCharShownFirst = l_iCharsWritten;
    }
    if(m_iCharShownFirst > l_iCharsWritten)
    {
       m_iCharShownFirst = 0;//buffer was reset
       m_bStickToEnd = true;
    }

    SetScrollRange(m_hWnd, SB_VERT, 0, l_iRange, FALSE);
    SetScrollPos(m_hWnd, SB_VERT, l_iScrollPos, TRUE);
}//void BufferViewer::UpdateScrollPos()

void BufferViewer::OnScrollMessage
(
    int p_nScrollBarType,
    int p_nScrollCode,
    int p_nPos,
    int p_nTrackPos,
    int p_nMin,
    int p_nMax
)
{
    if(!m_pBufferPointers || !m_pBufferPointers->m_pGlobalFooter)
        return;
    long l_lCurPos = p_nPos;
    long l_lNewPos = p_nPos;
    int  l_iLineOffset = 0;
    bool l_bStickToEnd = m_bStickToEnd, 
        l_bMoveToStartNextTime = m_bMoveToStartNextTime;
    switch(p_nScrollCode)
    {
    case SB_THUMBTRACK:
        l_lNewPos = p_nTrackPos;
        if(p_nTrackPos >= p_nMax)
        {
            l_bStickToEnd = true;
        }
        break;
    case SB_LINEUP:
        if(p_nScrollBarType == SB_VERT)
            l_lNewPos -= 1;
        else
            l_lNewPos -= m_iFontWidth;
        break;
    case SB_LINEDOWN:
        if(p_nScrollBarType == SB_VERT)
            l_lNewPos += m_iFirstRowLength;
        else
            l_lNewPos += m_iFontWidth;
        break;
    case SB_PAGEUP:
        if(p_nScrollBarType == SB_VERT)
            l_iLineOffset = -m_iNumLinesInClientArea;
        else
            l_lNewPos -= m_ClientRect.right - m_ClientRect.left - 
                m_iFontWidth;
        break;
    case SB_PAGEDOWN:
        if(p_nScrollBarType == SB_VERT)
            l_lNewPos += m_iNumBytesPainted-1;
        else
            l_lNewPos += m_ClientRect.right - m_ClientRect.left -
                m_iFontWidth;
        break;
    case SB_TOP:
        l_lNewPos = 0;
        l_bMoveToStartNextTime = true;
        break;
    case SB_BOTTOM:
        l_lNewPos = p_nMax;
        l_bStickToEnd = true;
        break;
    }
    if(l_lNewPos <  p_nMin)
       l_lNewPos =  p_nMin;
    else if(l_lNewPos >  p_nMax)
       l_lNewPos = p_nMax;
       
    if(p_nScrollBarType == SB_VERT)
    {
        m_iLineOffset = l_iLineOffset;
        int l_iDiffPos = l_lNewPos - l_lCurPos;
        if(l_iDiffPos < 0 || l_iLineOffset < 0)
        {
            l_bStickToEnd = false;
        }
        m_iCharShownFirst += l_iDiffPos;
        m_bStickToEnd = l_bStickToEnd;
        m_bMoveToStartNextTime = l_bMoveToStartNextTime;
        int l_iOldestCharInBuffer = ((int)
            (m_pBufferPointers->m_pGlobalFooter->m_dwNumBytesWritten- 
           m_pBufferPointers->m_dwTextAreaSize))/(int)sizeof(TCHAR);
        if(m_iCharShownFirst < l_iOldestCharInBuffer)
        {
            m_bMoveToStartNextTime = true;
        }
        else if(m_iCharShownFirst >= (int)
            (m_pBufferPointers->m_pGlobalFooter->m_dwNumBytesWritten/
            sizeof(TCHAR)) && m_iLineOffset >= 0)
        {
            m_bStickToEnd = true;
        }
    }
    else
    {
        if(m_iHorzPos == l_lNewPos)
            return;
        m_iHorzPos = l_lNewPos;
        m_PaintRect.right = m_ClientRect.right + m_iHorzPos;
    }
    SetScrollPos(m_hWnd, p_nScrollBarType, l_lNewPos, TRUE);
    InvalidateRect(m_hWnd, NULL, FALSE);
    m_iScrollChanged++;
}//void BufferViewer::OnScrollMessage

#ifdef WIN32 //{
/*-------------------------------------------------------------

   FUNCTION: WindowProc

   PURPOSE:  Window procedure for the trace buffer view window
------------------------------------------------------------*/
static LRESULT CALLBACK TraceViewWindowProc
(  
    HWND p_hWnd,
    UINT p_uMsg,
    WPARAM p_wParam,
    LPARAM p_lParam
)
{
    HTRACEK((KeyWordAppDebug, _T("View WindowProc %x %x %x %x"), 
        p_hWnd,  p_uMsg,  p_wParam, p_lParam ));

    long l_lBufferViewer = GetWindowLong(p_hWnd, 0);
    BufferViewer * l_pViewer =(BufferViewer * )l_lBufferViewer;
    switch(p_uMsg)
    {
    case WM_CREATE:
        {
            SetTimer(p_hWnd, 1, 1000, NULL);

            BufferViewer * l_pViewer =new BufferViewer(p_hWnd);
            l_pViewer->m_pBufferPointers = pGetTraceBuffer();
            l_pViewer->m_hFont = 
                (HFONT)GetStockObject(
#if !defined(_WIN32_WCE)
                SYSTEM_FIXED_FONT
#else
                SYSTEM_FONT
#endif
                );

            SetWindowLong(p_hWnd, 0, (long)l_pViewer);

            if(l_pViewer->m_pBufferPointers)
            {
                SetScrollRange(p_hWnd, SB_VERT, 0, 
                 l_pViewer->m_pBufferPointers->m_dwTextAreaSize/
                    sizeof(TCHAR), FALSE);
                l_pViewer->m_iLogicalWidthChars = 512;
                SetScrollRange(p_hWnd, SB_HORZ, 0, 
                    l_pViewer->m_iLogicalWidthChars, FALSE);
            }
        }
    case WM_SIZE:
        if(l_pViewer)
            l_pViewer->OnSizeChanges();
        break;
    case WM_TIMER:
        if(l_pViewer)
            l_pViewer->OnTimerMessage();
        break;
    case WM_VSCROLL:
    case WM_HSCROLL:
        if(l_pViewer)
        {
            int l_nScrollBarType = 
                p_uMsg == WM_VSCROLL? SB_VERT : SB_HORZ;
            int l_nScrollCode = (int) LOWORD(p_wParam);
            short int l_nPos = (short int) HIWORD(p_wParam);
            HWND l_hwndScrollBar = (HWND) p_lParam;

            SCROLLINFO l_ScrollInfo;
            memset(&l_ScrollInfo, 0, sizeof(l_ScrollInfo));
            l_ScrollInfo.cbSize = sizeof(l_ScrollInfo);
            l_ScrollInfo.fMask = 
                SIF_TRACKPOS | SIF_POS | SIF_RANGE;
            GetScrollInfo(p_hWnd, l_nScrollBarType, &l_ScrollInfo);
            long l_lPos = l_ScrollInfo.nPos;
            l_pViewer->OnScrollMessage(
                l_nScrollBarType, l_nScrollCode,
                l_ScrollInfo.nPos, l_ScrollInfo.nTrackPos,
                l_ScrollInfo.nMin, l_ScrollInfo.nMax);
        }
        break;
    case WM_PAINT:
        if(l_pViewer)
            l_pViewer->OnPaint();
        break;
    case WM_USER:
        KillTimer(p_hWnd, 1);
        SetTimer(p_hWnd, 1, p_wParam, NULL);
        break;
    case WM_USER+1:
        if(l_pViewer && l_pViewer->m_pBufferPointers && 
           l_pViewer->m_pBufferPointers->m_pGlobalFooter)
           l_pViewer->m_pBufferPointers->m_pGlobalFooter->
                m_dwNumBytesWritten = 0;
        break;
    case WM_DESTROY:
        KillTimer(p_hWnd, 1);
        PostQuitMessage(0);
        
        if(l_pViewer)
        {
            l_pViewer->m_pBufferPointers = NULL;
            delete l_pViewer;
        }
        SetWindowLong(p_hWnd, 0, 0);
        break;
    }
    return DefWindowProc(p_hWnd, p_uMsg, p_wParam, p_lParam);
}//LRESULT CALLBACK WindowProc

void RegisterTraceView(LPCTSTR p_pszClassName)
{
    WNDCLASS l_Class;
    memset(&l_Class,0,sizeof(l_Class));
    l_Class.lpfnWndProc = TraceViewWindowProc;
    l_Class.hCursor = LoadCursor(0,IDC_ARROW);
    l_Class.hbrBackground = (HBRUSH)(1 + COLOR_WINDOW);
    l_Class.lpszClassName = p_pszClassName;
    l_Class.cbWndExtra = 4;
    RegisterClass(&l_Class);
}
#endif //#ifdef WIN32 }
#endif //#ifdef TRACE_ }