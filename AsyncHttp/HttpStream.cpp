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

std::queue<std::string> m_DownloaderQueue;
TCHAR		m_szTempFile[MAX_PATH]; // Name of the temp file
BOOL        m_DownloaderShouldRun = FALSE;
HANDLE      m_hDownloader = NULL;
HANDLE		m_hFileWrite = INVALID_HANDLE_VALUE;   // File handle for writing to the temp file.
HANDLE		m_hFileRead = INVALID_HANDLE_VALUE;    // File handle for reading from the temp file.
LONGLONG	m_llDownloadLength = -1;
LONGLONG	m_llDownloadStart  = -1;
LONGLONG	m_llDownloadPos  = -1;
LONGLONG	m_llDownloadedBytes  = -1;
LONGLONG    m_llBytesRequested = 0;     // Size of most recent read request.
BOOL        m_llSeekPos = TRUE;
float       m_lldownspeed = 0.05F;
vector<BOOL> CHUNK_V; // this is the HAVING CHUNK Vector :-)
string      add_headers;
#pragma endregion

/*
 This is the stuff the downloads thread needs and holds
*/
BOOL israngeavail(LONGLONG start, LONGLONG length) 
{
    // remove
    int chunkstart = getchunkpos(start);
    // remove one byte - so that we don't reach strange barries
    int chunkend = getchunkpos(start+length-1);

    if (chunkend > (int)CHUNK_V.size()) {
        Log("israngeavail: !!! chunkend is bigger than CHUNK Vector");
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
    int chunkstart = getchunkpos(start);
    int chunkend = getchunkpos(end-1);
    *newstartpos = -1;

    if (chunkend > (int)CHUNK_V.size()) {
        Log("israngeavail_nextstart: !!! chunkend is bigger than CHUNK Vector");
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

HRESULT CreateTempFile(LONGLONG dsize)
{
	TCHAR *szTempPath = NULL;
	DWORD cch = 0;

	// Query for the size of the temp path.
	cch = GetTempPath(0, NULL);
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

	// Get the temp file name.
	UINT uval = GetTempFileName(szTempPath, TEXT("DisasterHTTPFilter"), 0, m_szTempFile);

	delete [] szTempPath;

	if (uval == 0)
	{
      return HRESULT_FROM_WIN32(GetLastError());
	}

    if (m_hFileWrite != INVALID_HANDLE_VALUE) {
      CloseHandle(m_hFileWrite);
    }
    if (m_hFileRead != INVALID_HANDLE_VALUE) {
	  CloseHandle(m_hFileRead);
    }

	// Delete old temp file to store the data.
    if (m_szTempFile[0] != TEXT('0'))
    {
        DeleteFile(m_szTempFile);
    }

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

    if (!DeviceIoControl(m_hFileWrite, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &cch, NULL))
    {
		Log("CreateTempFile: Couldn't set sparse - working without sparse");
	}

    LARGE_INTEGER fsize;
    fsize.QuadPart = dsize;
    if (SetFilePointerEx(m_hFileWrite, fsize, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
	   Log("CreateTempFile: Couldn't SetFilePointerEx of file %I64d", dsize);
       return E_FAIL;
    }
    if (!SetEndOfFile(m_hFileWrite)) {
	   Log("CreateTempFile: Couldn't set end of file %I64d", dsize);
       return E_FAIL;
    }

    // set chunk_v size
    Log("CreateTempFile: %s Chunks: %d", m_szTempFile, getchunkpos(dsize)+1);
    CHUNK_V.resize(getchunkpos(dsize)+1, FALSE);

return S_OK;
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
    m_llDownloadedBytes = 0;
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
	   LONGLONG tmp_startpos;
       DownloaderThread_geturlpos(&url, &tmp_startpos, DownloaderThread_GetDownloaderMsg());
       // allign startpos to CHUNK_SIZE
       startpos = (LONGLONG)(tmp_startpos/CHUNK_SIZE) * CHUNK_SIZE;
       Log("DownloaderThread: URL: %s Startpos: %I64d - Aligned Startpos: %I64d", url, tmp_startpos, startpos);

       // reinit all variables for THIS download
       DownloaderThread_initvars(startpos);

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
	   } catch(exception& ex) {
          Log("DownloaderThread: Fehler beim senden des Requests %s!", ex);
          SAFE_DELETE_ARRAY(request);
		  break;
	   }
       SAFE_DELETE_ARRAY(request);

       int statuscode = 999;
       string headers;
       GetHTTPHeaders(Socket, &m_llDownloadLength, &statuscode, headers);

       Log("DownloaderThread: Headers complete Downloadsize: %I64d", m_llDownloadLength);

	   SAFE_DELETE_ARRAY(szHost);
	   SAFE_DELETE_ARRAY(szPath);
      } // end CAutoLock lock(m_CritSec);

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
           // use MSG_WAITALL so the buffer should be always complete / full
		   bytesrec = recv(Socket, buffer, sizeof(buffer), MSG_WAITALL);
		   recv_calls++;

           // Bytes received write them down
		   if (bytesrec > 0) {
     		   bytesrec_sum += bytesrec;

               if (bytesrec < CHUNK_SIZE) {
                   if ( (m_llDownloadPos+bytesrec) == m_llDownloadLength )  {
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

	   } while (bytesrec > 0 && m_DownloaderShouldRun);
	   SAFE_DELETE_ARRAY(url);

	   if ((m_llDownloadStart+bytesrec_sum) == m_llDownloadLength) {
		   Log("DownloaderThread: Download finshed reached end of file! - startpos: %I64d downloaded: %I64d Bytes Remote file size: %I64d", startpos, bytesrec_sum, m_llDownloadLength);
       } else {
         if (bytesrec < 0) {
       	   Log("DownloaderThread: error received - error: %d", bytesrec);
         }
         Log("DownloaderThread: Download compl./canceled - startpos: %I64d downloaded: %I64d Bytes Remote file size: %I64d - m_DownloaderShouldRun: %s", startpos, bytesrec_sum, m_llDownloadLength, (m_DownloaderShouldRun)?"true":"false");
       }

	   // Verbindung beenden
	   closesocket(Socket);
	   SAFE_DELETE_ARRAY(url);
    }
    if ( m_DownloaderShouldRun && (m_DownloaderQueue.size() == 0)) {
        Sleep(50);
    }
  }

  // as we can also break the while set this to false again
  m_DownloaderShouldRun = false;
  Log("DownloaderThread: finished");
  ExitThread(0);
}

/*
 This is the stuff the main real class holds (CHTTPStream) thread needs and holds
*/
CHttpStream::~CHttpStream()
{
	Log("~CHttpStream() called");

	m_DownloaderShouldRun = false;
	SAFE_DELETE_ARRAY(m_FileName);

	// give the thread the time to finish
	Sleep(500);

	if (m_hFileWrite != INVALID_HANDLE_VALUE)
	{
		CloseHandle(m_hFileWrite);
	}

    if (m_hFileRead != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_hFileRead);
    }

	if (m_szTempFile[0] != TEXT('0'))
    {
        DeleteFile(m_szTempFile);
    }

    if (m_hDownloader != INVALID_HANDLE_VALUE)
    {
       CloseHandle(m_hDownloader);
    }

    StopLogger();
}


void FireDownloaderThread()
{
  UINT id;
  m_hDownloader = (HANDLE)_beginthreadex(NULL, 0, DownloaderThread, 0, 0, &id);
}

HRESULT CHttpStream::Downloader_Start(TCHAR* szUrl, LONGLONG startpoint) 
{
  // char msg[strlen(szUrl)+strlen(_i64toa(startpoint))+4+1];
  Log("CHttpStream::Downloader_Start called with URL: %s Startpos: %I64d", szUrl, startpoint);

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

HRESULT CHttpStream::ServerPreCheck(const char* url)
{
      int   Socket;
	  char *szHost = NULL;
      char *szPath = NULL;
	  int   szPort = NULL;
      LONGLONG runtime = GetSystemTimeInMS();
	  
	  Log("ServerPreCheck: Start for URL: %s", url);

	  // get Host, Path and Port from URL
      if (GetHostAndPath(url, &szHost, &szPath, &szPort) != 0)
      {
		   SAFE_DELETE_ARRAY(szHost);
		   SAFE_DELETE_ARRAY(szPath);
		   Log("ServerPreCheck: GetHostAndPath Error");
		   return E_FAIL;
      }

	  Socket = Initialize_connection(szHost, szPort);
	  if (Socket == -1) {
		   SAFE_DELETE_ARRAY(szHost);
		   SAFE_DELETE_ARRAY(szPath);
		   Log("ServerPreCheck: Socket could not be initialised.");
		   return E_FAIL;
	  }

	   char *request = buildrequeststring(szHost, szPort, szPath, 0, true, add_headers);

	   try {
  	      send_to_socket(Socket, request, strlen(request));
	   } catch(exception& ex) {
	      closesocket(Socket);
		  SAFE_DELETE_ARRAY(szHost);
		  SAFE_DELETE_ARRAY(szPath);
          SAFE_DELETE_ARRAY(request);

          Log("ServerPreCheck: Fehler beim senden des Requests %s!", ex);
	      return E_FAIL;
	   }
       SAFE_DELETE_ARRAY(request);

	   LONGLONG dsize = -1;
       int statuscode = 999;
       string headers;
       GetHTTPHeaders(Socket, &dsize, &statuscode, headers);

       Log("ServerPreCheck: Filesize: %I64d Statuscode: %d", dsize, statuscode);

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

          Log("\n\nServerPreCheck: REDIRECTED to %s!\n", newurl.c_str());
          return ServerPreCheck(newurl.c_str());
       }
       else if (statuscode == 200 || statuscode == 416) {
            Log("\n\nServerPreCheck: SERVER DOES NOT SUPPORT SEEKING!\n");
            m_llSeekPos = FALSE;
       }
       else if (statuscode == 206) {
            m_llSeekPos = TRUE;
       }
       else {
            Log("\n\nServerPreCheck: SERVER NOT SUPPORTED! Code: %d\n", statuscode);
	        closesocket(Socket);
            SAFE_DELETE_ARRAY(szHost);
	        SAFE_DELETE_ARRAY(szPath);
          return E_FAIL;
	   }

	   closesocket(Socket);
       SAFE_DELETE_ARRAY(szHost);
	   SAFE_DELETE_ARRAY(szPath);

       Log("\n\nServerPreCheck: SERVER OK => SUPPORTED!\n");

	   runtime = GetSystemTimeInMS()-runtime;
	   if (runtime > 2500) {
  		 Log("ServerPreCheck: Remote Server is too slow to render anything! Connect Time: %I64d", runtime);
		 return E_FAIL;
	   }

   HRESULT hr = CreateTempFile(dsize);
   if (FAILED(hr) || m_hFileWrite == INVALID_HANDLE_VALUE || m_hFileRead == INVALID_HANDLE_VALUE) {
	   Log("ServerPreCheck: CreateTempFile failed!");
	   return E_FAIL;
   }

   // Request the start and END
   runtime = GetSystemTimeInMS();
   add_to_downloadqueue(0);
   WaitForSize(0, (256*1024));
   if (m_llSeekPos) {
     add_to_downloadqueue(dsize-(256*1024));
     WaitForSize(dsize-(256*1024), dsize);
   }
   Log("\n\nServerPreCheck: PREBUFFER of file done\n");

   runtime = GetSystemTimeInMS()-runtime;
   // 512kb download
   if (runtime > 15000) {
	 Log("ServerPreCheck: Remote Server is too slow to render anything! Download of 500kb took: %I64d s", (LONGLONG)runtime/1000);
	 return E_FAIL;
   }

   return S_OK;
}

HRESULT CHttpStream::Initialize(LPCTSTR lpszFileName) 
{
    HRESULT hr;
    Log("CHttpStream::Initialize File: %s", lpszFileName);
    if (initWSA() != 0) {
        Log("CHttpStream::Initialize: Couldn't init WSA");
        return E_FAIL;
    }

    add_headers = "";

    string searcher = lpszFileName;
    string::size_type pos = 0;
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
        m_FileName = new TCHAR[strlen(lpszFileName)+1];
      	strcpy(m_FileName, lpszFileName);
    }

	m_szTempFile[0] = TEXT('0');

	hr = ServerPreCheck(m_FileName);
    if (FAILED(hr))
    {
        return hr;
    }

    /*
    TODO: don't start here - real read request wlll tell us where
    BUT NOT if the client waits until we buffer something
    */
    LONGLONG realstartpos;
    // get real first downloadpos
    israngeavail_nextstart(0, m_llDownloadLength, &realstartpos);
    hr = Downloader_Start(m_FileName, realstartpos);
    if (FAILED(hr))
    {
        return hr;
    }

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
    m_DownloaderShouldRun = FALSE;
    return S_OK;
}

HRESULT CHttpStream::Length(LONGLONG *pTotal, LONGLONG *pAvailable)
{
    return Length(&*pTotal, &*pAvailable, FALSE);
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

    if (!m_llSeekPos || realvalue) {
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
