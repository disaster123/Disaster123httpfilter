//////////////////////////////////////////////////////////////////////////
// HttpStream.cpp
// 
// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved.
//
//////////////////////////////////////////////////////////////////////////

#include "..\Base\stdafx.h"

#include <iostream>
#include <winsock2.h>

#include <stdio.h>
#include <string>
#include <streams.h>
#include <sstream>
#include <fstream>
#include <atlconv.h>
#include <math.h>
#include <time.h>
#include <vector>

#include <tchar.h>
#include <cstdio>
#include <WinIoCtl.h>

#include "..\Base\asyncio.h"
#include "HttpStream.h"
#include "..\Base\HTTPUtilities.h"

#include "AutoLockDebug.h"
#include "..\Base\alloctracing.h"

extern void Log(const char *fmt, ...);
extern void StopLogger();

/*
GLOBAL Variable declaration like blocking, .. locking... filenames...
*/
#pragma region global var declaration
static CCritSec m_datalock;
static CCritSec m_CritSec;
static CCritSec g_CritSec;

// TODO:
// implement RTMP seeking and correct file size detection
// Idea:
// - support seek when we have metadata
// - then when we get a filepos we calculate an estimated timepos and seek to it
// - we store this in a new temp file as we don't know the real pos. in our file

std::queue<std::string> m_DownloaderQueue;
RTMP          rtmp = { 0 };
string        rtmp_onlinevideos_params = "";
TCHAR		  m_szTempFile[MAX_PATH]; // Name of the temp file
BOOL          m_DownloaderShouldRun = FALSE;
HANDLE        m_hDownloader = NULL;
HANDLE		  m_hFileWrite = INVALID_HANDLE_VALUE;   // File handle for writing to the temp file.
HANDLE		  m_hFileRead = INVALID_HANDLE_VALUE;    // File handle for reading from the temp file.
LONGLONG	  m_llDownloadLength = -1;
LONGLONG	  m_llDownloadStart  = -1;
LONGLONG	  m_llDownloadPos  = -1;
LONGLONG	  m_llDownloadedBytes  = -1;
LONGLONG      m_llBytesRequested;     // Size of most recent read request.
BOOL          m_llSeekPos;
float         m_lldownspeed;
vector<BOOL>  CHUNK_V; // this is the HAVING CHUNK Vector :-)
string        add_headers;
vector<int>   winversion;
BOOL          is_rtmp = FALSE;
BOOL          rtmp_filesize_set = FALSE;
BOOL          ssupp_waitall = TRUE;
#pragma endregion

/*
This is the stuff the downloads thread needs and holds
*/
BOOL israngeavail(LONGLONG start, LONGLONG length) 
{
  int chunkstart = getchunkpos(start);
  // remove one byte - so that we don't reach strange barriers
  int chunkend = getchunkpos(start+length-1);

  if (chunkend > (int)CHUNK_V.size()) {
    Log("israngeavail: !!! chunkend is bigger than CHUNK Vector chunkend: %d Vector: %d", chunkend, CHUNK_V.size());
    return FALSE;
  }
  int i;
  for (i = chunkstart; i <= chunkend; i++) {
    if (!CHUNK_V[i]) {
      //Log("israngeavail: start: %I64d (chunk: %d) end: %I64d (chunk: %d) chunkpos %d NOT AVAIL", start, chunkstart, start+length, chunkend, i);
      return FALSE;
    }
  }
  // just for logging as i is incremented one more time
  //Log("israngeavail: start: %I64d (chunk: %d) end: %I64d (chunk: %d) chunkpos %d IS AVAIL", start, chunkstart, start+length, chunkend, --i);
  return TRUE;
}

void israngeavail_nextstart(LONGLONG start, LONGLONG end, LONGLONG* newstartpos) 
{
  // we start with 0 so decrement end
  end--;
  int chunkstart = getchunkpos(start);
  int chunkend = getchunkpos(end);
  *newstartpos = -1;

  if (chunkend > (int)CHUNK_V.size()) {
    Log("israngeavail_nextstart: !!! chunkend is bigger than CHUNK Vectorsize chunkend: %d Vector: %d", chunkend, CHUNK_V.size());
    return;
  }
  for (int i = chunkstart; i <= chunkend; i++) {
    if (!CHUNK_V[i]) {
      *newstartpos = (LONGLONG)i*CHUNK_SIZE;
      Log("israngeavail_nextstart: start: %I64d end: %I64d chunkstart: %d chunkend: %d chunkpos: %d newstartpos: %I64d", start, end, chunkstart, chunkend, i, *newstartpos);
      break;
    }
  }
}

string DownloaderThread_GetDownloaderMsg()
{
  if ( m_DownloaderQueue.size() == 0 )
  {
    return "";
  }

  string ret = m_DownloaderQueue.front();
  m_DownloaderQueue.pop();
  return ret;
}

HRESULT SetNewFileSize(LONGLONG dsize)
{
  DWORD cch = 0;

  if (!DeviceIoControl(m_hFileWrite, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &cch, NULL))
  {
    Log("SetNewFileSize: Couldn't set sparse - working without sparse");
  }

  LARGE_INTEGER fsize;
  fsize.QuadPart = dsize;
  if (SetFilePointerEx(m_hFileWrite, fsize, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
    Log("SetNewFileSize: Couldn't SetFilePointerEx of file %I64d", dsize);
    return E_FAIL;
  }
  if (!SetEndOfFile(m_hFileWrite)) {
    Log("SetNewFileSize: Couldn't set end of file %I64d", dsize);
    return E_FAIL;
  }

  // set chunk_v size
  Log("SetNewFileSize: %s Chunks: %d", m_szTempFile, getchunkpos(dsize)+1);
  CHUNK_V.resize(getchunkpos(dsize)+1, FALSE);

  return S_OK;
}

HRESULT CreateTempFile(LONGLONG dsize)
{
  TCHAR *szTempPath = NULL;
  DWORD cch = GetTempPath(0, NULL);

  // Query for the size of the temp path.
  if (cch == 0)
  {
    return HRESULT_FROM_WIN32(GetLastError());
  }

  // Allocate a buffer for the name.
  szTempPath = new TCHAR[cch];
  if (szTempPath == NULL)
  {
    return E_OUTOFMEMORY;
  }

  // Get the temp path.
  cch = GetTempPath(cch, szTempPath);
  if (cch == 0)
  {
    delete [] szTempPath;
    return HRESULT_FROM_WIN32(GetLastError());
  }

  if (m_hFileWrite != INVALID_HANDLE_VALUE) {
    CloseHandle(m_hFileWrite);
    m_hFileWrite = INVALID_HANDLE_VALUE;
  }

  if (m_hFileRead != INVALID_HANDLE_VALUE) {
    CloseHandle(m_hFileRead);
    m_hFileRead = INVALID_HANDLE_VALUE;
  }

  // Delete old temp file to store the data.
  if (m_szTempFile[0] != TEXT('0')) {
    DeleteFile(m_szTempFile);
  }

  // as our files can be really big and windows doesn't clean tmp automatically we do that
  // if there is a 2nd running instance this isn't a problem as windows blocks
  // our delete request
  for (int i = 0; i < 100; i++) {
    int length = _snprintf(NULL, 0, "%s\\Disaster123HTTPFilter_%d.file", szTempPath, i);
    TCHAR *tmpname = new TCHAR[length+1];
    _snprintf(tmpname, length, "%s\\Disaster123HTTPFilte_%d.file", szTempPath, i);
    DeleteFile(tmpname);
    SAFE_DELETE_ARRAY(tmpname);
  }

  for (int i = 0; i < 100; i++) {
    _snprintf(m_szTempFile, sizeof(m_szTempFile), "%s\\Disaster123HTTPFilter_%d.file", szTempPath, i);

    // Create the temp file and open the handle for writing. 
    m_hFileWrite = CreateFile(
      m_szTempFile,
      GENERIC_WRITE,
      FILE_SHARE_READ,
      NULL,
      CREATE_ALWAYS,
      0,
      NULL
      );

    if (m_hFileWrite != INVALID_HANDLE_VALUE) {
      break;
    }
  }

  // Get the temp file name.
  SAFE_DELETE_ARRAY(szTempPath);

  if (m_hFileWrite == INVALID_HANDLE_VALUE) 
  {
    Log("CreateTempFile: Could not create temp file %s", m_szTempFile);
    m_szTempFile[0] = TEXT('\0');
    return HRESULT_FROM_WIN32(GetLastError());
  }

  // Open a read handle for the same temp file.
  m_hFileRead = CreateFile(
    m_szTempFile,
    GENERIC_READ,
    FILE_SHARE_WRITE | FILE_SHARE_READ,
    NULL,
    OPEN_EXISTING,
    FILE_FLAG_OVERLAPPED,
    NULL
    );

  if (m_hFileRead == INVALID_HANDLE_VALUE) 
  {
    Log("CreateTempFile: Could not open temp file for reading.");
    m_szTempFile[0] = TEXT('\0');
    CloseHandle(m_hFileWrite);
    m_hFileWrite = INVALID_HANDLE_VALUE;

    return HRESULT_FROM_WIN32(GetLastError());
  }

  Log("CreateTempFile: %s created", m_szTempFile);

  return SetNewFileSize(dsize);
}

DWORD DownloaderThread_WriteData(LONGLONG startpos, char *buffer, int buffersize)
{
#ifndef AUTOLOCK_DEBUG
  CAutoLock lock(&m_datalock);
#endif
#ifdef AUTOLOCK_DEBUG
  CAutoLockDebug lock(&m_datalock, __LINE__, __FILE__,__FUNCTION__);
#endif

  BOOL bResult = 0;
  DWORD cbWritten = 0;

  if (m_hFileWrite == INVALID_HANDLE_VALUE) {
    return -1;
  }
  LARGE_INTEGER iPos;
  iPos.QuadPart = startpos;
  DWORD dr = SetFilePointerEx(m_hFileWrite, iPos, NULL, FILE_BEGIN);
  if (dr == INVALID_SET_FILE_POINTER) {
    DWORD err = GetLastError();
    LPTSTR Error = 0;
    FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, NULL, err, 0, (LPTSTR)&Error, 0, NULL);
    Log("DownloaderThread_WriteData: INVALID_SET_FILE_POINTER Pos: %I64d Startpos: %I64d Error: %s", iPos.QuadPart, startpos, Error);
    SAFE_DELETE(Error);

    return HRESULT_FROM_WIN32(err);
  }

  // Write the data to the temp file.
  bResult = WriteFile(m_hFileWrite, buffer, buffersize, &cbWritten, NULL);
  if (!bResult)
  {
    DWORD err = GetLastError();
    return err;
  }
  // do this BEFORE m_llDownloadPos - as we always set the starting point
  CHUNK_V[getchunkpos(m_llDownloadPos)] = TRUE;
  m_llDownloadPos += cbWritten;
  m_llDownloadedBytes += cbWritten;
  //Log("DownloaderThread_WriteData: Wrote from: %I64d to: %I64d Chunk: %d Length: %u Buffer: %d downpos: %I64d", startpos, startpos+cbWritten, getchunkpos(m_llDownloadPos-cbWritten), cbWritten, buffersize, m_llDownloadPos);

  return 0;
}

void DownloaderThread_initvars(LONGLONG startpos) {
  m_llDownloadStart = startpos;
  m_llDownloadPos = startpos;
  // this shouldn't be resetted while restart download
  // m_llDownloadedBytes = 0;
  m_llBytesRequested = 0;
}

UINT CALLBACK DownloaderThread(void* param)
{
  Log("DownloaderThread: started");

  while ( m_DownloaderShouldRun ) {

    if ( m_DownloaderQueue.size() > 0 ) {
      char *url;
      LONGLONG startpos = -1;
      int   Socket;
      char *szHost = NULL;
      char *szPath = NULL;
      int   szPort = NULL;

      { // start new block for CAutoLock
#ifndef AUTOLOCK_DEBUG
        CAutoLock lock(&m_datalock);
#else
        CAutoLockDebug lock(&m_datalock, __LINE__, __FILE__,__FUNCTION__);
#endif

        // get message from the queue AND URL and POS from queuestring
        LONGLONG startpos_orig = -1;
        LONGLONG new_startpos = -1;
        DownloaderThread_geturlpos(&url, &startpos_orig, DownloaderThread_GetDownloaderMsg());
        if (startpos_orig < 0) {
          SAFE_DELETE_ARRAY(url);
          Log("DownloaderThread: Startpos: %I64d not OK - skipping", startpos_orig);
          break;
        }
        if (m_llDownloadLength > 0 && startpos_orig >= m_llDownloadLength) {
          SAFE_DELETE_ARRAY(url);
          Log("DownloaderThread: Startpos: %I64d >= m_llDownloadLength %I64d - skipping", startpos_orig, m_llDownloadLength);
          break;
        }
        // allign startpos to CHUNK_SIZE
        startpos = (LONGLONG)(startpos_orig/CHUNK_SIZE) * CHUNK_SIZE;
        Log("DownloaderThread: Startpos: %I64d Al.Spos: %I64d URL: %s", startpos_orig, startpos, url);
        // we can only do / check the next stuff if we have a file size
        if (m_llDownloadLength > 0) {
          if (startpos >= m_llDownloadLength) {
            SAFE_DELETE_ARRAY(url);
            Log("DownloaderThread: startpos is out of range %I64d >= %I64d (startpos >= m_llDownloadLength)", startpos, m_llDownloadLength);
            break;
          }
          israngeavail_nextstart(startpos, m_llDownloadLength, &new_startpos);
          if (new_startpos >= m_llDownloadLength) {
            SAFE_DELETE_ARRAY(url);
            Log("DownloaderThread: newstartpos >= m_llDownloadLength reached - cancel furthur download");
            break;
          }
          if (new_startpos > startpos) {
            Log("DownloaderThread: skip startpos to new chunk newstartpos %I64d -> %I64d", startpos, new_startpos);
            startpos = new_startpos;
          }
        }

        // reinit all variables for THIS download
        DownloaderThread_initvars(startpos);

        if (is_rtmp) {
          // do nothing here atm
        }
        else 
        {
          // get Host, Path and Port from URL
          if (GetHostAndPath(url, &szHost, &szPath, &szPort) != 0)
          {
            SAFE_DELETE_ARRAY(url);
            SAFE_DELETE_ARRAY(szHost);
            SAFE_DELETE_ARRAY(szPath);
            Log("DownloaderThread: GetHostAndPath Error");
            break;
          }

          Socket = Initialize_connection(szHost, szPort);
          if (Socket == -1) {
            SAFE_DELETE_ARRAY(url);
            SAFE_DELETE_ARRAY(szHost);
            SAFE_DELETE_ARRAY(szPath);
            Log("DownloaderThread: Socket could not be initialised.");
            break;
          }

          char *request = buildrequeststring(szHost, szPort, szPath, startpos, m_llSeekPos, add_headers);

          try {
            send_to_socket(Socket, request, strlen(request));
          } catch(exception ex) {
            Log("DownloaderThread: Fehler beim senden des Requests %s!", ex);
            SAFE_DELETE_ARRAY(request);
            break;
          }
          SAFE_DELETE_ARRAY(request);

          int statuscode = 999;
          string headers;
          LONGLONG NULLL;
          GetHTTPHeaders(Socket, &NULLL, &statuscode, headers);
          if (statuscode != 206 && statuscode != 200) {
            Log("DownloaderThread: Statuscode not OK: %d - retry download", statuscode);
            char msg[500];
            sprintf_s(msg, sizeof(msg), "%s || %I64d", url, startpos_orig);
            m_DownloaderQueue.push((string)msg);
            continue;
          }

          Log("DownloaderThread: Headers complete Downloadsize: %I64d", m_llDownloadLength);

          SAFE_DELETE_ARRAY(szHost);
          SAFE_DELETE_ARRAY(szPath);
        } // end CAutoLock lock(m_CritSec);
      }

      //char buffer[1024*128];
      //unsigned int buflen = sizeof(buffer)/8; // use 16kb of the buffer as default
      char buffer[CHUNK_SIZE];
      BOOL buffcalc = FALSE;
      int bytesrec = 0;
      LONGLONG bytesrec_sum = 0;
      LONGLONG bytesrec_sum_old = 0;
      LONGLONG recv_calls = 0;
      LONGLONG time_start = GetSystemTimeInMS();
      LONGLONG time_end;
      buffer[0] = '\0';
      do {
        if (is_rtmp) {
          bytesrec = rtmp_recv_wait_all(&rtmp, buffer, sizeof(buffer));
        } else {
          // MSG_WAITALL is broken / not available on Win XP so we've to use our own function
          bytesrec = recv_wait_all(Socket, buffer, sizeof(buffer), ssupp_waitall);
        }
        recv_calls++;

        // Bytes received write them down
        if (bytesrec > 0) {
          bytesrec_sum += bytesrec;

          if (bytesrec < CHUNK_SIZE) {
            if (is_rtmp && !rtmp_filesize_set) {
              Log("DownloaderThread: got %d bytes instead of a full buffer of size %I64d - but IT COULD BE OK rtmp filesize not set", bytesrec, (LONGLONG)CHUNK_SIZE);
              m_llDownloadLength = m_llDownloadPos+bytesrec;
            } else if (is_rtmp && 
              (getchunkpos(m_llDownloadPos+bytesrec) >= (getchunkpos(m_llDownloadLength)-1)) ) {
                // RTMP filesize does not MATCH exactly the filepos
                Log("DownloaderThread: got %d bytes instead of a full buffer of size %I64d - but this is OK we are near END of RTMP file", bytesrec, (LONGLONG)CHUNK_SIZE);
                m_llDownloadLength = m_llDownloadPos+bytesrec;
            } else if ( (m_llDownloadPos+bytesrec) == m_llDownloadLength )  {
              Log("DownloaderThread: got %d bytes instead of a full buffer of size %I64d - but this is OK it is END of file", bytesrec, (LONGLONG)CHUNK_SIZE);
            } else {
              Log("DownloaderThread: got %d bytes instead of a full buffer of size %I64d - restart download at pos %I64d", bytesrec, (LONGLONG)CHUNK_SIZE, m_llDownloadPos);
              char msg[500];
              sprintf_s(msg, sizeof(msg), "%s || %I64d", url, m_llDownloadPos);
              m_DownloaderQueue.push((string)msg);
            }
          }

          // check if the position we want to write is already there and Queue is 0
          if (m_DownloaderQueue.size() == 0 && israngeavail(m_llDownloadPos, bytesrec)) {
            LONGLONG newstartpos;
            israngeavail_nextstart(m_llDownloadPos, m_llDownloadLength, &newstartpos);
            if (newstartpos >= m_llDownloadLength || newstartpos <= 0) {
              Log("DownloaderThread: newstartpos >= m_llDownloadLength reached - cancel furthur download");
              break;
            }
            Log("DownloaderThread: write range is already available Start: %I64d End: %I64d New startpos: %I64d", m_llDownloadPos, m_llDownloadPos+bytesrec, newstartpos);
            char msg[500];
            sprintf_s(msg, sizeof(msg), "%s || %I64d", url, newstartpos);
            m_DownloaderQueue.push((string)msg);
          }

          if (m_DownloaderQueue.size() > 0) {
            time_end = GetSystemTimeInMS();
            LONGLONG bytesdiff = bytesrec_sum-bytesrec_sum_old;
            float timediff = ((float)(time_end-time_start))/1000;
            // set only new downloadspeed if we have at least a value with a timediff of > 0.2s
            if (timediff > 0.2) {
              m_lldownspeed = (float)Round(((float)bytesdiff/1024/1024)/timediff, 4);
            }

            Log("DownloaderThread: Downloaded (found new queue request): %.2LfMB time: %.2Lf Speed: %.4Lf MB/s Recv: %I64d Last requested: %I64d", ((float)bytesrec_sum/1024/1024), timediff, m_lldownspeed, recv_calls, m_llBytesRequested);
            break;
          }

          DownloaderThread_WriteData(m_llDownloadPos, buffer, bytesrec);

          time_end = GetSystemTimeInMS();
          // print every 3s
          if ((time_end-time_start) > 3000) {
            LONGLONG bytesdiff = bytesrec_sum-bytesrec_sum_old;
            float timediff = ((float)(time_end-time_start))/1000;
            m_lldownspeed = (float)Round(((float)bytesdiff/1024/1024)/timediff, 4);
            Log("DownloaderThread: Downloaded %.2LfMB Pos: %.4LfMB time: %.2Lf Speed: %.4Lf MB/s Recv: %I64d Last requested: %I64d", ((float)bytesrec_sum/1024/1024), ((float)m_llDownloadPos/1024/1024), timediff, m_lldownspeed, recv_calls, m_llBytesRequested);
            time_start = GetSystemTimeInMS();
            bytesrec_sum_old = bytesrec_sum;
          }
        }

      } while (bytesrec > 0 && m_DownloaderShouldRun && (!is_rtmp || (RTMP_IsConnected(&rtmp) && !RTMP_IsTimedout(&rtmp))) );
      SAFE_DELETE_ARRAY(url);

      if ((m_llDownloadStart+bytesrec_sum) == m_llDownloadLength) {
        Log("DownloaderThread: Download finshed reached end of file! - startpos: %I64d downloaded: %I64d Bytes Remote file size: %I64d", startpos, bytesrec_sum, m_llDownloadLength);
      } else {
        if (bytesrec < 0) {
          Log("DownloaderThread: negative bytes received - error: %d", bytesrec);
        }
        Log("DownloaderThread: Download compl./canceled - startpos: %I64d downloaded: %I64d Bytes Remote file size: %I64d - m_DownloaderShouldRun: %s", startpos, bytesrec_sum, m_llDownloadLength, (m_DownloaderShouldRun)?"true":"false");
      }

      // Verbindung beenden
      if (!is_rtmp) {
        closesocket(Socket);
      }
      SAFE_DELETE_ARRAY(url);
    }
    if ( m_DownloaderShouldRun && (m_DownloaderQueue.size() == 0)) {
      Sleep(50);
    }
  }

  // as we can also break the while set this to false
  m_DownloaderShouldRun = false;
  Log("DownloaderThread: finished");
  ExitThread(0);
}


void FireDownloaderThread()
{
  UINT id;
  m_hDownloader = (HANDLE)_beginthreadex(NULL, 0, DownloaderThread, 0, 0, &id);
}

HRESULT CHttpStream::Downloader_Start(TCHAR* szUrl, LONGLONG startpoint) 
{
  // char msg[strlen(szUrl)+strlen(_i64toa(startpoint))+4+1];
  Log("Downloader_Start Startpos: %I64d URL: %s", startpoint, szUrl);

  char msg[500];
  sprintf_s(msg, sizeof(msg), "%s || %I64d", szUrl, startpoint);
  m_DownloaderQueue.push((string)msg);

  if (!m_hDownloader || !m_DownloaderShouldRun) 
  {
    m_DownloaderShouldRun = true;
    FireDownloaderThread();
  }

  return S_OK;
};

/*
This is the stuff the main real class holds (CHTTPStream) thread needs and holds
*/
CHttpStream::~CHttpStream()
{
  DbgLog((LOG_ERROR,0,TEXT("~CHttpStream() called")));
  Log("~CHttpStream() called");

  m_DownloaderShouldRun = false;

  if (m_hFileWrite != INVALID_HANDLE_VALUE)
  {
    CloseHandle(m_hFileWrite);
    m_hFileWrite = INVALID_HANDLE_VALUE;
  }

  if (m_hFileRead != INVALID_HANDLE_VALUE)
  {
    CloseHandle(m_hFileRead);
    m_hFileRead = INVALID_HANDLE_VALUE;
  }

  if (m_szTempFile[0] != TEXT('0'))
  {
    DeleteFile(m_szTempFile);
    m_szTempFile[0] = TEXT('0');
  }

  if (m_hDownloader != NULL)
  {
    WaitForSingleObject(m_hDownloader, INFINITE);	
    m_hDownloader = NULL;
  }
  if (is_rtmp) {
    RTMP_Close(&rtmp);
    is_rtmp = FALSE;
    rtmp_filesize_set = FALSE;
    rtmp_onlinevideos_params = "";
  }

  Log("~CHttpStream() StopLogger...");
  StopLogger();

  SAFE_DELETE_ARRAY(m_FileName);
  // this if the filter is used a second time this value stays to -1 until it is initialized
  m_llDownloadedBytes  = -1;
  DbgLog((LOG_ERROR,0,TEXT("~CHttpStream() called => END")));
}

HRESULT CHttpStream::ServerRTMPPreCheck(char* url, string& filetype)
{ 
  LONGLONG runtime = GetSystemTimeInMS();
  Log("CHttpStream::ServerRTMPPreCheck: Start for URL: %s", url);

  AVal hostname = { 0, 0 };
  AVal playpath = { 0, 0 };
  AVal subscribepath = { 0, 0 };
  int port = -1;
  int protocol = RTMP_PROTOCOL_UNDEFINED;
  int retries = 0;
  int bLiveStream = FALSE;	// is it a live stream? then we can't seek/resume
  int bHashes = FALSE;		// display byte counters not hashes by default
  uint32_t dSeek = 0;		// seek position in resume mode, 0 otherwise
  long int timeout = 20;	// timeout connection after 20 seconds
  uint32_t dStartOffset = 0;	// seek position in non-live mode
  uint32_t dStopOffset = 0;
  AVal swfUrl = { 0, 0 };
  AVal tcUrl = { 0, 0 };
  AVal pageUrl = { 0, 0 };
  AVal app = { 0, 0 };
  AVal auth = { 0, 0 };
  AVal swfHash = { 0, 0 };
  uint32_t swfSize = 0;
  AVal flashVer = { 0, 0 };
  AVal sockshost = { 0, 0 };
  AVal parsedHost, parsedApp, parsedPlaypath;
  unsigned int parsedPort = 0;
  int parsedProtocol = RTMP_PROTOCOL_UNDEFINED;

  RTMP_Init(&rtmp);

  if (!RTMP_ParseURL(url, &parsedProtocol, &parsedHost, &parsedPort, &parsedPlaypath, &parsedApp)) {
    Log("RTMP_ParseURL: Couldn't parse the specified url %s!", url);
    return E_FAIL;
  }

  if (!hostname.av_len)
    hostname = parsedHost;
  if (port == -1)
    port = parsedPort;
  if (playpath.av_len == 0 && parsedPlaypath.av_len)
    playpath = parsedPlaypath;
  if (protocol == RTMP_PROTOCOL_UNDEFINED)
    protocol = parsedProtocol;
  if (app.av_len == 0 && parsedApp.av_len)
    app = parsedApp;

  if (port == 0) {
    if (protocol & RTMP_FEATURE_SSL)
      port = 443;
    else if (protocol & RTMP_FEATURE_HTTP)
      port = 80;
    else
      port = 1935;
  }

  if (tcUrl.av_len == 0) {
    char str[512] = { 0 };

    tcUrl.av_len = _snprintf_c(str, 511, "%s://%.*s:%d/%.*s", RTMPProtocolStringsLower[protocol], hostname.av_len,
      hostname.av_val, port, app.av_len, app.av_val);
    tcUrl.av_val = (char *) malloc(tcUrl.av_len + 1);
    strcpy(tcUrl.av_val, str);
  }

  // special OnlineVideos subparam parsing
  if (rtmp_onlinevideos_params.length() > 0) {
    string lvalue = "";
    if (GetURLParam(rtmp_onlinevideos_params, "tcUrl", lvalue) == S_OK) {
      tcUrl.av_len = lvalue.length();
      tcUrl.av_val = (char *) malloc(lvalue.length() + 1);
      strcpy(tcUrl.av_val, lvalue.c_str());
    }
    if (GetURLParam(rtmp_onlinevideos_params, "hostname", lvalue) == S_OK) {
      hostname.av_len = lvalue.length();
      hostname.av_val = (char *) malloc(lvalue.length() + 1);
      strcpy(hostname.av_val, lvalue.c_str());
    }
    if (GetURLParam(rtmp_onlinevideos_params, "playpath", lvalue) == S_OK) {
      playpath.av_len = lvalue.length();
      playpath.av_val = (char *) malloc(lvalue.length() + 1);
      strcpy(playpath.av_val, lvalue.c_str());
    }
    if (GetURLParam(rtmp_onlinevideos_params, "subscribepath", lvalue) == S_OK) {
      subscribepath.av_len = lvalue.length();
      subscribepath.av_val = (char *) malloc(lvalue.length() + 1);
      strcpy(subscribepath.av_val, lvalue.c_str());
    }
    if (GetURLParam(rtmp_onlinevideos_params, "pageurl", lvalue) == S_OK) {
      pageUrl.av_len = lvalue.length();
      pageUrl.av_val = (char *) malloc(lvalue.length() + 1);
      strcpy(pageUrl.av_val, lvalue.c_str());
    }
    if (GetURLParam(rtmp_onlinevideos_params, "swfurl", lvalue) == S_OK) {
      swfUrl.av_len = lvalue.length();
      swfUrl.av_val = (char *) malloc(lvalue.length() + 1);
      strcpy(swfUrl.av_val, lvalue.c_str());
    }
    if (GetURLParam(rtmp_onlinevideos_params, "swfsize", lvalue) == S_OK) {
      swfSize = atoi(lvalue.c_str());
    }
    if (GetURLParam(rtmp_onlinevideos_params, "swfhash", lvalue) == S_OK) {
      swfHash.av_len = lvalue.length();
      swfHash.av_val = (char *) malloc(lvalue.length() + 1);
      strcpy(swfHash.av_val, lvalue.c_str());
    }
    if (GetURLParam(rtmp_onlinevideos_params, "app", lvalue) == S_OK) {
      app.av_len = lvalue.length();
      app.av_val = (char *) malloc(lvalue.length() + 1);
      strcpy(app.av_val, lvalue.c_str());
    }
    if (GetURLParam(rtmp_onlinevideos_params, "auth", lvalue) == S_OK) {
      auth.av_len = lvalue.length();
      auth.av_val = (char *) malloc(lvalue.length() + 1);
      strcpy(auth.av_val, lvalue.c_str());
    }
    if (GetURLParam(rtmp_onlinevideos_params, "live", lvalue) == S_OK) {
      bLiveStream = atoi(lvalue.c_str());
    }
  }

  // Conn
  // int RTMP_SetOpt(RTMP *r, const AVal *opt, AVal *arg)

  Log("RTMP Options: detected\n\ttcURL: %s\n\tprot: %s\n\thostname: %.*s\n\tport: %d\n\tsockshost: %s\n\tplaypath: %s\n\tswfUrl: %s\n\tpageUrl: %s\n\tapp: %.*s\n\tauth: %s\n\tswfHash: %s\n\tswfSize: %d\n\tflashVer: %s\n\tsubscribepath: %s\n\tdSeek: %d\n\tdStopOffset: %d\n\tbLiveStream: %d\n",
    tcUrl.av_val, RTMPProtocolStringsLower[protocol], 
    hostname.av_len, hostname.av_val, port, sockshost.av_val, playpath.av_val,
    swfUrl.av_val, pageUrl.av_val, app.av_len, app.av_val, auth.av_val, swfHash.av_val, swfSize,
    flashVer.av_val, subscribepath.av_val, dSeek, dStopOffset, bLiveStream);

  RTMP_SetupStream(&rtmp, protocol, &hostname, port, &sockshost, &playpath,
    &tcUrl, &swfUrl, &pageUrl, &app, &auth, &swfHash, swfSize,
    &flashVer, &subscribepath, dSeek, dStopOffset, bLiveStream, timeout);

  /* Try to keep the stream moving if it pauses on us */
  if (!bLiveStream && !(protocol & RTMP_FEATURE_HTTP))
    rtmp.Link.lFlags |= RTMP_LF_BUFX;

  RTMP_SetBufferMS(&rtmp, (uint32_t) (2 * 3600 * 1000)); // 2hrs

  if (!RTMP_Connect(&rtmp, NULL))
  {
    Log("CHttpStream::ServerRTMPPreCheck: RTMP_Connect not possible to: %s", url);
    return E_FAIL;
  }

  if (!RTMP_ConnectStream(&rtmp, 0))
  {
    Log("CHttpStream::ServerRTMPPreCheck: RTMP_ConnectStream not possible to: %s", url);
    return E_FAIL;
  }

  // TODO: support seeking
  m_llSeekPos = FALSE;
  // we assume a min of 1 hour and kbs 2000 (250 in bytes)
  m_llDownloadLength = 250 * 1024 * 60 * 60;

  HRESULT hr = CreateTempFile(m_llDownloadLength);
  if (FAILED(hr) || m_hFileWrite == INVALID_HANDLE_VALUE || m_hFileRead == INVALID_HANDLE_VALUE) {
    Log("CHttpStream::ServerRTMPPreCheck: CreateTempFile failed!");
    return E_FAIL;
  }

  add_to_downloadqueue( 0 );
  // we should have the metadata at this point
  WaitForSize(0, 4 * CHUNK_SIZE );
  // if the header is still 0 wait again for the same size
  if (rtmp.m_read.nMetaHeaderSize == 0) {
    WaitForSize(0, 4 * CHUNK_SIZE * 2 );
  }
  Log("CHttpStream::ServerRTMPPreCheck: waiting for MetaHeader done - Size: %d", rtmp.m_read.nMetaHeaderSize);

  LONGLONG filesize = (LONGLONG)rtmp_get_double_from_metadata(rtmp.m_read.metaHeader, rtmp.m_read.nMetaHeaderSize, "filesize");
  Log("CHttpStream::ServerRTMPPreCheck: got Filesize: %I64d", filesize);
  if (filesize <= 0) {
    // calculate with bitrate kbs 2000 (250 in bytes)
    filesize = 250 * 1024 * (LONGLONG)rtmp.m_fDuration;
    Log("CHttpStream::ServerRTMPPreCheck: Calculated size: %I64d from duration: %f", filesize, rtmp.m_fDuration);
    if (rtmp.m_fDuration <= 0) {
      Log("CHttpStream::ServerRTMPPreCheck: Duration is not OK", filesize);
      return E_FAIL;
    }
  } else {
    // we got the real filesize - in all other cases the filesize is nearly unknown
    rtmp_filesize_set = TRUE;
  }
  m_llDownloadLength = filesize;
  SetNewFileSize(filesize);

  return S_OK;
}

HRESULT CHttpStream::ServerHTTPPreCheck(const char* url, string& filetype)
{
  int   Socket;
  char *szHost = NULL;
  char *szPath = NULL;
  int   szPort = NULL;
  LONGLONG runtime = GetSystemTimeInMS();

  Log("CHttpStream::ServerHTTPPreCheck: Start for URL: %s", url);

  // get Host, Path and Port from URL
  if (GetHostAndPath(url, &szHost, &szPath, &szPort) != 0)
  {
    SAFE_DELETE_ARRAY(szHost);
    SAFE_DELETE_ARRAY(szPath);
    Log("CHttpStream::ServerHTTPPreCheck: GetHostAndPath Error");
    return E_FAIL;
  }

  Socket = Initialize_connection(szHost, szPort);
  if (Socket == -1) {
    SAFE_DELETE_ARRAY(szHost);
    SAFE_DELETE_ARRAY(szPath);
    Log("ServerHTTPPreCheck: Socket could not be initialised.");
    return E_FAIL;
  }

  char *request = buildrequeststring(szHost, szPort, szPath, 0, true, add_headers);
  try {
    send_to_socket(Socket, request, strlen(request));
  } catch(exception ex) {
    closesocket(Socket);
    SAFE_DELETE_ARRAY(szHost);
    SAFE_DELETE_ARRAY(szPath);
    SAFE_DELETE_ARRAY(request);

    Log("ServerHTTPPreCheck: Fehler beim Senden des Requests %s!", ex);
    return E_FAIL;
  }
  SAFE_DELETE_ARRAY(request);

  LONGLONG dsize = -1;
  int statuscode = 999;
  string headers;
  GetHTTPHeaders(Socket, &dsize, &statuscode, headers);
  GetValueFromHeader(headers.c_str(), "Content-Type", filetype);

  Log("ServerHTTPPreCheck: Filesize: %I64d Statuscode: %d Filetype: %s", dsize, statuscode, filetype.c_str());
  if (statuscode == 301) {
    // map 301 to 302 for us - it doesn't matter if the redirect is permanent or temporary
    statuscode = 302;
  }
  if (statuscode != 302 && dsize <= 0) {
    closesocket(Socket);
    SAFE_DELETE_ARRAY(szHost);
    SAFE_DELETE_ARRAY(szPath);

    Log("Server reported illegal Filesize of 0 - we cannot download 0 bytes!");

    return E_FAIL;
  }

  if (statuscode == 302) {
    string newurl = GetLocationFromHeader(headers);

    closesocket(Socket);
    SAFE_DELETE_ARRAY(szHost);
    SAFE_DELETE_ARRAY(szPath);

    SAFE_DELETE_ARRAY(m_FileName);
    m_FileName = new TCHAR[strlen(newurl.c_str())+1];
    strcpy(m_FileName, newurl.c_str());

    Log("\n\nServerHTTPPreCheck: REDIRECTED to %s!\n", newurl.c_str());
    return ServerHTTPPreCheck(newurl.c_str(), filetype);
  }
  else if (statuscode == 200 || statuscode == 416) {
    Log("\n\nServerHTTPPreCheck: SERVER DOES NOT SUPPORT SEEKING!\n");
    m_llSeekPos = FALSE;
  }
  else if (statuscode == 206) {
    m_llSeekPos = TRUE;
  }
  else {
    Log("\n\nServerHTTPPreCheck: SERVER NOT SUPPORTED! Code: %d\n", statuscode);
    closesocket(Socket);
    SAFE_DELETE_ARRAY(szHost);
    SAFE_DELETE_ARRAY(szPath);
    return E_FAIL;
  }

  closesocket(Socket);
  SAFE_DELETE_ARRAY(szHost);
  SAFE_DELETE_ARRAY(szPath);

  m_llDownloadLength = dsize;
  Log("\n\nServerHTTPPreCheck: SERVER OK => SUPPORTED!\n");

  runtime = GetSystemTimeInMS()-runtime;
  if (runtime > 2500) {
    Log("ServerHTTPPreCheck: Remote Server is too slow to render anything! Connect Time: %I64d", runtime);
    return E_FAIL;
  }

  HRESULT hr = CreateTempFile(dsize);
  if (FAILED(hr) || m_hFileWrite == INVALID_HANDLE_VALUE || m_hFileRead == INVALID_HANDLE_VALUE) {
    Log("ServerHTTPPreCheck: CreateTempFile failed!");
    return E_FAIL;
  }

  // Request the start and END
  runtime = GetSystemTimeInMS();
#ifdef _DEBUG
  Log("ServerHTTPPreCheck: added queue download 0");
#endif
  add_to_downloadqueue( 0 );
  WaitForSize(0, min( (256*1024), dsize) );
#ifdef _DEBUG
  Log("ServerHTTPPreCheck: wait for size: %I64d - DONE", min( (256*1024), dsize) );
#endif
  if (m_llSeekPos) {
    add_to_downloadqueue( max(0, dsize-(256*1024) ) );
    WaitForSize( max(0, dsize-(256*1024) ) , dsize);
#ifdef _DEBUG
    Log("ServerHTTPPreCheck: wait for size: %I64d - DONE", max(0, dsize-(256*1024) ) );
#endif
  }
  Log("\n\nServerHTTPPreCheck: PREBUFFER of file done\n");

  runtime = GetSystemTimeInMS()-runtime;
  // 512kb download
  if (runtime > 15000) {
    Log("ServerHTTPPreCheck: Remote Server is too slow to render anything! Download of 500kb took: %I64d s", (LONGLONG)runtime/1000);
    return E_FAIL;
  }

  return S_OK;
}

HRESULT CHttpStream::Initialize(LPCTSTR lpszFileName, string& filetype) 
{
  HRESULT hr;
  Log("CHttpStream::Initialize File: %s", lpszFileName);
  if (initWSA() != 0) {
    Log("CHttpStream::Initialize: Couldn't init WSA");
    return E_FAIL;
  }

  m_llDownloadLength = -1;
  m_llDownloadStart  = -1;
  m_llDownloadPos  = -1;
  m_llDownloadedBytes  = 0;
  m_llBytesRequested = 0;
  m_llSeekPos = TRUE;
  m_lldownspeed = 0.05F;
  CHUNK_V.clear();
  add_headers = "";
  winversion.clear();
  ssupp_waitall = TRUE;
  is_rtmp = FALSE;
  rtmp_filesize_set = FALSE;
  rtmp_onlinevideos_params = "";
  m_szTempFile[0] = TEXT('0');

  GetOperationSystemName(winversion);
  Log("CHttpStream::Initialize: Windows Version: %d.%d.%d", winversion[0], winversion[1], winversion[2]);
  if ( (winversion[0] < 5) ||
    (winversion[0] == 5 && winversion[1] <= 1) )
  {
    ssupp_waitall = FALSE;
  }

  string searcher = lpszFileName;
  string::size_type pos = 0;

  if ((searcher.find("rtmp://", 0) != string::npos) || (searcher.find("rtmpe://", 0) != string::npos)) {
    Log("CHttpStream::Initialize: rtmp URL found");
    is_rtmp = TRUE;

    // special OnlineVideos handling
    if ((searcher.find("rtmp://127.0.0.1/stream.flv?", 0) != string::npos) ||
      (searcher.find("rtmpe://127.0.0.1/stream.flv?", 0) != string::npos)) {
        // seems to be an OnlineVideos case :-)
        string keyvalue = "";
        if (GetURLParam(searcher, "rtmpurl", keyvalue) == S_OK) {
          // we simply use here the whole orig. string this is the easiest way
          rtmp_onlinevideos_params = searcher;

          // copy the new value to searcher we will copy it later to the local URL VALUE
          searcher = keyvalue;

          Log("CHttpStream::Initialize: OnlineVideos RTMP Stream detected - new URL: %s", keyvalue.c_str());
        }
    }
  }

  // check for &&&& special params
  if ((pos = searcher.find("&&&&", 0)) != string::npos) {
    string url = searcher.substr(0, pos);
    m_FileName = new TCHAR[strlen(url.c_str())+1];
    strcpy(m_FileName, url.c_str());

    add_headers = searcher.substr(pos+4, searcher.length()-pos-4);
    UrlDecode(add_headers);
    Log("CHttpStream::Initialize: Found request with additional headers. URL: %s Headers: %s", m_FileName, add_headers.c_str());
    stringreplace(add_headers, "\\r", "\r");
    stringreplace(add_headers, "\r", "");
    stringreplace(add_headers, "\\n", "\n");
    stringreplace(add_headers, "\n", "\r\n");
  } else {
    // no special params found copy the searcher to m_FileName
    m_FileName = new TCHAR[strlen(searcher.c_str())+1];
    strcpy(m_FileName, searcher.c_str());
  }

  if (is_rtmp) {
    DbgLog((LOG_ERROR,0,TEXT("ServerRTMPPreCheck start")));
    hr = ServerRTMPPreCheck(m_FileName, filetype);
    DbgLog((LOG_ERROR,0,TEXT("ServerRTMPPreCheck end")));
  } else {
    DbgLog((LOG_ERROR,0,TEXT("ServerHTTPPreCheck start")));
    hr = ServerHTTPPreCheck(m_FileName, filetype);
    DbgLog((LOG_ERROR,0,TEXT("ServerHTTPPreCheck end")));
  }
  if (FAILED(hr))
  {
    Log("ServerPreCheck failed!");
    return hr;
  }

  // only do this if seeking is supported - otherwise
  // the download is still running from the precheck
  // also do this - otherwise some programs who query for buffering
  // will wait forever
  if (m_llSeekPos) {
    LONGLONG realstartpos;

    Log("Seeking is supported - start download");
    // get real first downloadpos
    israngeavail_nextstart(0, m_llDownloadLength, &realstartpos);
    if (realstartpos >= 0 && realstartpos < m_llDownloadLength) {
      hr = Downloader_Start(m_FileName, realstartpos);
      if (FAILED(hr))
      {
        return hr;
      }
    } else {
      Log("Download not needed - file already available");
    }
  }

#ifdef _DEBUG
  Log("return S_OK from Load");
#endif

  return S_OK;
}

HRESULT CHttpStream::add_to_downloadqueue(LONGLONG startpos) 
{
  HRESULT hr;

  hr = Downloader_Start(m_FileName, startpos);
  if (FAILED(hr))
  {
    return hr;
  }

  return S_OK;
}

void CHttpStream::WaitForSize(LONGLONG start, LONGLONG end) 
{
  LONGLONG length = end-start;
  // wait max. ~20s
  int i = 0;
  for (i = 0; i <= 20; i++) {
    // We need now to check if the requested space is allocated or not...
    if (israngeavail(start,length)) {
      break;
    }
    Sleep(100);
  }
  if (i >= 2000) {
    Log("CHttpStream::WaitForSize: Timed OUT! Start: %I64d Length: %I64d", start, length);
  }
  /*
  #ifdef _DEBUG
  else {
  Log("CHttpStream::WaitForSize: OK! Start: %I64d Length: %I64d", start, length);
  }
  #endif
  */
}

HRESULT CHttpStream::StartRead(PBYTE pbBuffer,DWORD dwBytesToRead,BOOL bAlign,LPOVERLAPPED pOverlapped,LPBOOL pbPending, LPDWORD pdwBytesRead)
{
  HRESULT hr = S_OK;
  LARGE_INTEGER pos;
  BOOL bResult;
  DWORD err;
  *pbPending = FALSE;
  BOOL bWait = FALSE;

#ifndef AUTOLOCK_DEBUG
  CAutoLock lock(&m_CritSec);
#endif
#ifdef AUTOLOCK_DEBUG
  CAutoLockDebug lock(&m_CritSec, __LINE__, __FILE__,__FUNCTION__);
#endif

#ifdef MANLOCK_DEBUG
  Log("CHttpStream::StartRead: m_datalock.Lock");
#endif
  m_datalock.Lock();
#ifdef MANLOCK_DEBUG
  Log("CHttpStream::StartRead: m_datalock.Lock done - UNLOCK WILL FOLLOW");
#endif

  if (m_hFileRead == INVALID_HANDLE_VALUE) {
    Log("CHttpStream::StartRead: File handle is invalid - return E_FAIL");
    m_datalock.Unlock();
    return E_FAIL;
  }
  if (dwBytesToRead == 0) {
    Log("CHttpStream::StartRead: dwBytesToRead is 0 - return E_FAIL");
    m_datalock.Unlock();
    return E_FAIL;
  }
  pos.LowPart = pOverlapped->Offset;
  pos.HighPart = pOverlapped->OffsetHigh;

  LONGLONG llLength = dwBytesToRead;
  LONGLONG llReadEnd = pos.QuadPart + llLength;

#ifdef _DEBUG
  Log("CHttpStream::StartRead: read from %I64d (%.4Lf MB) to %I64d (%.4Lf MB)", pos.QuadPart, ((float)pos.QuadPart/1024/1024), llReadEnd, ((float)llReadEnd/1024/1024) );
#endif

  if ((m_llDownloadLength > 0) && (pos.QuadPart > m_llDownloadLength || llReadEnd > m_llDownloadLength)) {
    Log("CHttpStream::StartRead: THIS SHOULD NEVER HAPPEN! requested start or endpos out of max. range - return end of file");
    m_datalock.Unlock();
    return HRESULT_FROM_WIN32(38);
  }

  // is the requested range available?
  if (!israngeavail(pos.QuadPart,llLength))
  {
    // request is out of range let's check if we can reach it
#ifdef _DEBUG
    Log("CHttpStream::StartRead: Request out of range - downstart: %I64d (%.4Lf MB) downpos: %I64d (%.4Lf MB)", m_llDownloadStart, ((float)m_llDownloadStart/1024/1024), m_llDownloadPos, ((float)m_llDownloadPos/1024/1024) );
#endif
    // check if we can reach the barrier at all
    if ((pos.QuadPart >= m_llDownloadStart) && (llReadEnd > m_llDownloadPos))
    {
      // check if we'll reach the pos. within X sec.
      int reachin = 5;
      if ( llReadEnd > (m_llDownloadPos+(m_lldownspeed*1024*1024*reachin)) ) {
        Log("CHttpStream::StartRead: will not reach pos. within %d seconds - speed: %.4Lf MB/s ", reachin, m_lldownspeed);
        m_datalock.Unlock();
        if (m_llSeekPos) {
          add_to_downloadqueue(pos.QuadPart);
        }
      }
      else
      { // out of range BUT will reach Limit in X sek.
        m_datalock.Unlock();
      }
    } else {
      // out of range AND not in line so can't reach it
      m_datalock.Unlock();
      if (m_llSeekPos) {
        add_to_downloadqueue(pos.QuadPart);
      }
    }

    bWait = TRUE;

  } else {
    // request is OK Data is already there
    m_datalock.Unlock();
  }

  if (bWait)
  {
    // Notify the application that the filter is buffering data.
    if (m_pEventSink)
    {
      m_pEventSink->Notify(EC_BUFFERING_DATA, TRUE, 0);
    }

    // Log("CHttpStream::StartRead: wait for start: %I64d end: %I64d", pos.QuadPart, llReadEnd);
    m_llBytesRequested = llReadEnd;
    WaitForSize(pos.QuadPart, llReadEnd);
    m_llBytesRequested = 0;
    // Log("CHttpStream::StartRead: wait for start: DONE");

    if (m_pEventSink)
    {
      m_pEventSink->Notify(EC_BUFFERING_DATA, FALSE, 0);
    }
  }
  // Lock the datalock
  // this prevents the downloadthread from starting a new donwloadpos,
  // closing, reopening the file handle while we want to read, ...
#ifdef MANLOCK_DEBUG
  Log("CHttpStream::StartRead: m_datalock.Lock()");
#endif
  m_datalock.Lock();

  //Log("CHttpStream::StartRead: Read from Temp File BytesToRead: %u Startpos: %I64d", dwBytesToRead, pos.QuadPart);
  // Read the data from the temp file. (Async I/O request.)
  // Log("CHttpStream::StartRead: Read from Temp File %u %I64d", dwBytesToRead, pos.QuadPart);
  bResult = ReadFile(
    m_hFileRead, 
    pbBuffer, 
    dwBytesToRead, 
    pdwBytesRead,
    pOverlapped
    );
  //Log("CHttpStream::StartRead: Read from Temp File");

  m_datalock.Unlock();
#ifdef MANLOCK_DEBUG
  Log("CHttpStream::StartRead: m_datalock.Unlock() done");
#endif

  if (bResult == 0)
  {
    err = GetLastError();

    // IO_PENDING isn't really an error it is wanted in this case as we have async I/O
    // Endread finishes the read of data
    if (err == ERROR_IO_PENDING)
    {
      *pbPending = TRUE;
    }
    else
    {
      LPTSTR Error = 0;
      if(::FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
        NULL,
        err,
        0,
        (LPTSTR)&Error,
        0,
        NULL) == 0) {
          // Failed in translating.
      }

      Log("CHttpStream::StartRead: ReadFile failed (err = %d) %s", err, Error);
      if ( Error ) {
        LocalFree(Error);
      }

      // An actual error occurred.
      hr = HRESULT_FROM_WIN32(err);
    }
  }
  return hr;
}

void CHttpStream::Lock() {
  // The MS Sample Filter uses here also the m_CritSec but when we do this with MP we have a deadlock
  // with Graphedit it works fine
#ifdef MANLOCK_DEBUG
  Log("CHttpStream::Lock(): Lock");
#endif
  g_CritSec.Lock();
}

void CHttpStream::Unlock() {
  // The MS Sample Filter uses here also the m_CritSec but when we do this with MP we have a deadlock
  // with Graphedit it works fine
#ifdef MANLOCK_DEBUG
  Log("CHttpStream::Unlock(): Unlock");
#endif
  g_CritSec.Unlock();
}

HRESULT CHttpStream::EndRead(
  LPOVERLAPPED pOverlapped, 
  LPDWORD pdwBytesRead
  )
{
#ifndef AUTOLOCK_DEBUG
  CAutoLock lock(&m_CritSec);
#endif
#ifdef AUTOLOCK_DEBUG
  CAutoLockDebug lock(&m_CritSec, __LINE__, __FILE__,__FUNCTION__);
#endif

  LARGE_INTEGER pos;
  pos.LowPart = pOverlapped->Offset;
  pos.HighPart = pOverlapped->OffsetHigh;

  BOOL bResult = 0;

  // Complete the async I/O request.
  m_datalock.Lock();
  bResult = GetOverlappedResult(m_hFileRead, pOverlapped, pdwBytesRead, TRUE);
  m_datalock.Unlock();

  //Log("CHttpStream::EndRead: Read done Startpos: %I64d Read up to (don't trust this value - some splitters send strange buffers to us): %I64d", pos.QuadPart, (pos.QuadPart+(LONGLONG)*pdwBytesRead));

  if (!bResult)
  {
    DWORD err = GetLastError();
    LPTSTR Error = 0;
    FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, NULL, err, 0, (LPTSTR)&Error, 0, NULL);
    Log("CHttpStream::EndRead: File error! %s", Error);
    SAFE_DELETE(Error);

    return HRESULT_FROM_WIN32(err);
  }

  return S_OK;
}

HRESULT CHttpStream::Cancel()
{
  Log("CHttpStream::Cancel()");
  CHttpStream::~CHttpStream();

  return S_OK;
}

HRESULT CHttpStream::Length(LONGLONG *pTotal, LONGLONG *pAvailable)
{
  return Length(pTotal, pAvailable, FALSE);
}

HRESULT CHttpStream::Length(LONGLONG *pTotal, LONGLONG *pAvailable, BOOL realvalue)
{
  ASSERT(pTotal != NULL);
  ASSERT(pAvailable != NULL);
  m_datalock.Lock();

  // The file is still downloading.
  if (m_llDownloadLength <= 0)
  {
    Log("CHttpStream::Length: is 0 wait until a few bytes are here!");
    if (m_pEventSink)
    {
      m_pEventSink->Notify(EC_BUFFERING_DATA, TRUE, 0);
    }

    m_llBytesRequested = 5+m_llDownloadStart; // at least 5 bytes
    m_datalock.Unlock();
    WaitForSize(m_llDownloadStart, m_llBytesRequested);
    m_datalock.Lock();
    m_llBytesRequested = 0;

    if (m_pEventSink)
    {
      m_pEventSink->Notify(EC_BUFFERING_DATA, FALSE, 0);
    }
  }

  // TODO: can we remove this when seeking is working?
  // ATM: we always provide the real values if we have rtmp
  if (is_rtmp || !m_llSeekPos || realvalue) {
    *pTotal = m_llDownloadLength;
    *pAvailable = m_llDownloadedBytes;
  } else {
    *pTotal = m_llDownloadLength;
    *pAvailable = *pTotal;
  }

  m_datalock.Unlock();
  //Log("Length called: return: total: %I64d avail: %I64d", *pTotal, *pAvailable);

  return S_OK;
}
