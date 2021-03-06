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

#include "..\Base\stdafx.h"

#include <streams.h>

#include "..\Base\asyncio.h"
#include "..\Base\asyncrdr.h"

#include <initguid.h>
#include "..\AsyncHttp\asynchttp.h"

#include <atlbase.h>
#include <shlobj.h>
#include <queue>

#include "..\Base\alloctracing.h"

using namespace std;

const WCHAR *szAsyncHttp = L"Disaster123's MP Alternate Source Filter";
const char *VERSION = "0.30";

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

REGFILTER2 rf2FilterReg = {
  1,              // Version 1 (no pin mediums or pin category).
  MERIT_PREFERRED,   // Merit.
  1,              // Number of pins.
  &sudOpPin        // Pointer to pin information.
};

const AMOVIESETUP_FILTER sudAsyncHttp =
{ 
  &CLSID_AsyncHttp,         // clsID
  szAsyncHttp,                    // strName
  MERIT_PREFERRED,                 // dwMerit
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
  sprintf(dest,"C:\\Temp\\Disaster123HTTPFilter.%s",name);
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
BOOL m_bLoggerDisable = false;
HANDLE m_hLogger = NULL;
BOOL logrunned = false;

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
  // push out the rest of the queue so check also for queue size
  while ( m_bLoggerRunning || m_logQueue.size() > 0 ) {
    if ( m_logQueue.size() > 0 ) {
      FILE* fp = fopen(fileName,"a+");
      if (fp != NULL)
      {
        SYSTEMTIME systemTime;
        GetLocalTime(&systemTime);
        string line = GetLogLine();
        while (!line.empty())
        {
          fprintf(fp, "%s\n", line.c_str());
          line = GetLogLine();
        }
        fclose(fp);
      } else {
        m_bLoggerDisable = true;
        break;
      }
    }
    Sleep(500);
  }

  ExitThread(0);
}


void StartLogger()
{
  UINT id;
  if (m_bLoggerDisable) {
    return;
  }
  if (!logrunned) {
    logrunned = TRUE;
    LogRotate();
  }
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

  if (m_bLoggerDisable) {
    return;
  }

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
  sprintf_s(msg, 5000,"%02.2d-%02.2d-%04.4d %02.2d:%02.2d:%02.2d.%03.3d [%5x] %s",
    systemTime.wDay, systemTime.wMonth, systemTime.wYear,
    systemTime.wHour, systemTime.wMinute, systemTime.wSecond,
    systemTime.wMilliseconds,
    GetCurrentThreadId(),
    buffer);
  CAutoLock l(&m_qLock);

#ifdef _DEBUG
  DbgLog((LOG_ERROR,0,TEXT(msg)));
#endif

  m_logQueue.push((string)msg);
};


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
  HRESULT hr;
  IFilterMapper2 *pFM2 = NULL;
  HKEY hKey;
  unsigned char    *StringUuid;
  char* regvalue;

  Log("DllRegisterServer... VERSION: %s", VERSION);

  hr = AMovieDllRegisterServer2(TRUE);
  if (FAILED(hr))
    return hr;

  hr = CoCreateInstance(CLSID_FilterMapper2, NULL, CLSCTX_INPROC_SERVER,
    IID_IFilterMapper2, (void **)&pFM2);

  if (FAILED(hr))
    return hr;

  hr = pFM2->RegisterFilter(
    CLSID_AsyncHttp,                 // Filter CLSID. 
    szAsyncHttp,                     // Filter name.
    NULL,                            // Device moniker. 
    &CLSID_LegacyAmFilterCategory,   // filter cat.
    szAsyncHttp,                       // Instance data.
    &rf2FilterReg                    // Pointer to filter information.
    );
  pFM2->Release();
  if (FAILED(hr))
    return hr;

  UuidToString(&CLSID_AsyncHttp, &StringUuid);
  regvalue = (char*) malloc (sizeof(char) * (strlen((char *)StringUuid) + 3));
  sprintf(regvalue, "{%s}", StringUuid);
  RegOpenKeyEx(HKEY_CLASSES_ROOT, "http", NULL, KEY_READ | KEY_SET_VALUE, &hKey);
  RegSetValueEx(hKey, TEXT("Source Filter"), 0, REG_SZ, (LPBYTE)regvalue, strlen(regvalue)+1);
  RegCloseKey(hKey);
  RpcStringFree(&StringUuid);
  SAFE_DELETE(regvalue);

  return hr;
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
