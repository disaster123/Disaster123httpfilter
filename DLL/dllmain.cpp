/*************************************************************************
*  DllMain.cpp: DLL entry point and filter registration code.
*  
*  THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
*  ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
*  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
*  PARTICULAR PURPOSE.
* 
*  Copyright (c) Microsoft Corporation. All rights reserved.
* 
*************************************************************************/

#include <streams.h>

#include "..\Base\asyncio.h"
#include "..\Base\asyncrdr.h"

#include <initguid.h>
#include "..\AsyncHttp\asynchttp.h"

#include <atlbase.h>
#include <shlobj.h>
#include <queue>

using namespace std;

const WCHAR *szAsyncHttp = L"CSSR SPriebe Http Filter";
const char *VERSION = "0.10";

//
// Setup data for filter registration
//
const AMOVIESETUP_MEDIATYPE sudOpPinTypes =
{ 
    &MEDIATYPE_Stream,     // clsMajorType
    &MEDIASUBTYPE_NULL     // clsMinorType
};

const AMOVIESETUP_PIN sudOpPin =
{ 
    L"Output",          // strName
    FALSE,              // bRendered
    TRUE,               // bOutput
    FALSE,              // bZero
    FALSE,              // bMany
    &CLSID_NULL,        // clsConnectsToFilter
    L"Input",           // strConnectsToPin
    1,                  // nTypes
    &sudOpPinTypes      // lpTypes
};

const AMOVIESETUP_FILTER sudAsyncHttp =
{ 
    &CLSID_AsyncHttp,         // clsID
    szAsyncHttp,                    // strName
	MERIT_NORMAL,                 // dwMerit
    1,                              // nPins
    &sudOpPin                       // lpPin
};


//
//  Object creation template
//
CFactoryTemplate g_Templates[] = 
{
    {
        szAsyncHttp,
        &CLSID_AsyncHttp,
        CAsyncFilterHttp::CreateInstance,
        NULL,
        &sudAsyncHttp
    }
};

int g_cTemplates = sizeof(g_Templates) / sizeof(g_Templates[0]);



void LogPath(char* dest, char* name)
{
  sprintf(dest,"C:\\Temp\\async.%s",name);
}


void LogRotate()
{
  TCHAR fileName[MAX_PATH];
  LogPath(fileName, "log");
  TCHAR bakFileName[MAX_PATH];
  LogPath(bakFileName, "bak");
  remove(bakFileName);
  // ignore if rename fails 
  (void)rename(fileName, bakFileName);
}


CCritSec m_qLock;
std::queue<std::string> m_logQueue;
BOOL m_bLoggerRunning;
HANDLE m_hLogger = NULL;

string GetLogLine()
{
  CAutoLock lock(&m_qLock);
  if ( m_logQueue.size() == 0 )
  {
    return "";
  }

  string ret = m_logQueue.front();
  m_logQueue.pop();
  return ret;
}


UINT CALLBACK LogThread(void* param)
{
  TCHAR fileName[MAX_PATH];
  LogPath(fileName, "log");
  while ( m_bLoggerRunning ) {
    if ( m_logQueue.size() > 0 ) {
      FILE* fp = fopen(fileName,"a+");
      if (fp != NULL)
      {
        SYSTEMTIME systemTime;
        GetLocalTime(&systemTime);
        string line = GetLogLine();
        while (!line.empty())
        {
          fprintf(fp, "%s", line.c_str());
          line = GetLogLine();
        }
        fclose(fp);
      }
    }
    Sleep(500);
  }

  ExitThread(0);
}


void StartLogger()
{
  UINT id;
  LogRotate();
  m_hLogger = (HANDLE)_beginthreadex(NULL, 0, LogThread, 0, 0, &id);
  SetThreadPriority(m_hLogger, THREAD_PRIORITY_BELOW_NORMAL);
}


void StopLogger()
{
  if (m_hLogger)
  {
    m_bLoggerRunning = FALSE;
    WaitForSingleObject(m_hLogger, INFINITE);	
    m_hLogger = NULL;
  }
}

void Log(const char *fmt, ...) 
{
  static CCritSec lock;
  va_list ap;
  va_start(ap, fmt);

  CAutoLock logLock(&lock);
  if (!m_hLogger) 
  {
    m_bLoggerRunning = true;
    StartLogger();
  }
  char buffer[1000]; 
  int tmp;
  va_start(ap, fmt);
  tmp = vsprintf(buffer, fmt, ap);
  va_end(ap); 

  SYSTEMTIME systemTime;
  GetLocalTime(&systemTime);
  char msg[5000];
  sprintf_s(msg, 5000,"%02.2d-%02.2d-%04.4d %02.2d:%02.2d:%02.2d.%03.3d [%5x] %s\n",
    systemTime.wDay, systemTime.wMonth, systemTime.wYear,
    systemTime.wHour, systemTime.wMinute, systemTime.wSecond,
    systemTime.wMilliseconds,
    GetCurrentThreadId(),
    buffer);
  CAutoLock l(&m_qLock);
  m_logQueue.push((string)msg);
};

HRESULT __fastcall UnicodeToAnsi(LPCOLESTR pszW, LPSTR* ppszA)
{
  ULONG cbAnsi;
  ULONG cCharacters;

  // If input is null then just return the same.
  if (pszW == NULL)
  {
    *ppszA = NULL;
    return NOERROR;
  }

  cCharacters = (ULONG)wcslen(pszW)+1;
  // Determine number of bytes to be allocated for ANSI string. An
  // ANSI string can have at most 2 bytes per character (for Double
  // Byte Character Strings.)
  cbAnsi = cCharacters*2;

  // Use of the OLE allocator is not required because the resultant
  // ANSI  string will never be passed to another COM component. You
  // can use your own allocator.
  *ppszA = (LPSTR) CoTaskMemAlloc(cbAnsi);
  if (NULL == *ppszA)
    return E_OUTOFMEMORY;

  // Convert to ANSI.
  if (0 == WideCharToMultiByte(CP_ACP, 0, pszW, cCharacters, *ppszA,
    cbAnsi, NULL, NULL))
  {
    DWORD dwError = GetLastError();
    CoTaskMemFree(*ppszA);
    *ppszA = NULL;
    return HRESULT_FROM_WIN32(dwError);
  }
  return NOERROR;
}

const char *getVersion() {
   return VERSION;
}

////////////////////////////////////////////////////////////////////////
//
// Exported entry points for registration and unregistration 
// (in this case they only call through to default implementations).
//
////////////////////////////////////////////////////////////////////////

STDAPI DllRegisterServer()
{
	Log("CAsyncFilterHttp::CreateInstance... VERSION: %s", VERSION);

    return AMovieDllRegisterServer2(TRUE);
}

STDAPI DllUnregisterServer()
{
    return AMovieDllRegisterServer2(FALSE);
}

//
// DllEntryPoint
//
extern "C" BOOL WINAPI DllEntryPoint(HINSTANCE, ULONG, LPVOID);

BOOL APIENTRY DllMain(HANDLE hModule, 
                      DWORD  dwReason, 
                      LPVOID lpReserved)
{
	return DllEntryPoint((HINSTANCE)(hModule), dwReason, lpReserved);
}
