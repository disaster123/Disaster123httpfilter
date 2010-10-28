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

#include <tchar.h>
#include <cstdio>

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
LONGLONG    m_llFileLength = 0;         // Current length of the temp file, in bytes
LONGLONG	m_llDownloadLength = 0;
LONGLONG	m_llFileLengthStartPoint = 0; // Start of Current length in bytes
LONGLONG    m_llBytesRequested = 0;     // Size of most recent read request.
BOOL        m_llSeekPos = TRUE;
float       m_lldownspeed;
string      add_headers;
#pragma endregion

/*
 This is the stuff the downloads thread needs and holds
*/

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

HRESULT DownloaderThread_CreateTempFile()
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
		Log("Could not create temp file %s", m_szTempFile);

		m_szTempFile[0] = TEXT('\0');

		return HRESULT_FROM_WIN32(GetLastError());
	}

    Log("Created Tempfile %s", m_szTempFile);

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
		Log("Could not open temp file for reading.");

		m_szTempFile[0] = TEXT('\0');

        CloseHandle(m_hFileWrite);
        m_hFileWrite = INVALID_HANDLE_VALUE;

		return HRESULT_FROM_WIN32(GetLastError());
	}

	return S_OK;
}


DWORD DownloaderThread_WriteData(char *buffer, int buffersize)
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

    // Write the data to the temp file.
	bResult = WriteFile(m_hFileWrite, buffer, buffersize, &cbWritten, NULL);
    if (!bResult)
    {
		DWORD err = GetLastError();
		return err;
    }

    m_llFileLength += cbWritten;

	return 0;
}

void DownloaderThread_initvars(LONGLONG startpos) {
	m_llFileLengthStartPoint = startpos;
	m_llFileLength = 0;
    m_llDownloadLength = -1;
    m_llBytesRequested = 0;
}

UINT CALLBACK DownloaderThread(void* param)
{
  Log("DownloaderThread: started");

  while ( m_DownloaderShouldRun ) {

	if ( m_DownloaderQueue.size() > 0 ) {
      char *url;
	  LONGLONG startpos = -1;
      int Socket;
	  char *szHost = NULL;
      char *szPath = NULL;
	  int szPort = NULL;

	  { // start new block for CAutoLock
#ifndef AUTOLOCK_DEBUG
          CAutoLock lock(&m_datalock);
#else
     	  CAutoLockDebug lock(&m_datalock, __LINE__, __FILE__,__FUNCTION__);
#endif

	   // get message from the queue AND URL and POS from queuestring
       DownloaderThread_geturlpos(&url, &startpos, DownloaderThread_GetDownloaderMsg());
	   Log("DownloaderThread: URL: %s Startpos: %I64d", url, startpos);
       // reinit all variables for THIS download
       DownloaderThread_initvars(startpos);

	   // create TEMP File
       DownloaderThread_CreateTempFile();

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

	   SAFE_DELETE_ARRAY(url);
	   SAFE_DELETE_ARRAY(szHost);
	   SAFE_DELETE_ARRAY(szPath);
      } // end CAutoLock lock(m_CritSec);

	   char buffer[1024*128];
	   unsigned int buflen = sizeof(buffer)/8; // use 16kb of the buffer as default
	   BOOL buffcalc = FALSE;
	   int bytesrec = 0;
	   LONGLONG bytesrec_sum = 0;
	   LONGLONG bytesrec_sum_old = 0;
	   LONGLONG recv_calls = 0;
	   LONGLONG time_start = GetSystemTimeInMS();
       LONGLONG time_end;
	   do {
		   bytesrec = recv(Socket, buffer, buflen, 0);
		   recv_calls++;

           // Bytes received write them down
		   if (bytesrec > 0) {
     		   bytesrec_sum += bytesrec;

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

		       DownloaderThread_WriteData(buffer, bytesrec);

               time_end = GetSystemTimeInMS();
			   if (!buffcalc && (time_end-time_start) > 500) {
				   buffcalc = TRUE;
                   int i = (int)recv_calls/25;
			       buflen = max(min(sizeof(buffer), i*buflen), buflen);
				   Log("DownloaderThread: in 500ms we were called: %I64d times - raising buffer to %d", recv_calls, buflen);
			   }
               // print every 3s
               if ((time_end-time_start) > 3000) {
			 	 LONGLONG bytesdiff = bytesrec_sum-bytesrec_sum_old;
                 float timediff = ((float)(time_end-time_start))/1000;
                 m_lldownspeed = (float)Round(((float)bytesdiff/1024/1024)/timediff, 4);
				 Log("DownloaderThread: Downloaded %.2LfMB time: %.2Lf Speed: %.4Lf MB/s Recv: %I64d Last requested: %I64d", ((float)bytesrec_sum/1024/1024), timediff, m_lldownspeed, recv_calls, m_llBytesRequested);
				 time_start = GetSystemTimeInMS();
				 bytesrec_sum_old = bytesrec_sum;
			   }
		   }

	   } while (bytesrec > 0 && m_DownloaderShouldRun);

       if ((m_llFileLength+m_llFileLengthStartPoint) == m_llDownloadLength) {
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
  char msg[500];
  Log("CHttpStream::Downloader_Start called with URL: %s Startpos: %I64d", szUrl, startpoint);

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
      int Socket;
	  char *szHost = NULL;
      char *szPath = NULL;
	  int szPort = NULL;
	  
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
       else if (statuscode == 200) {
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

    // new file new location new server reset downloadspeed
    // and copy filename to global filename variable
    m_lldownspeed = 0.05F; // assume 50kb/s as a start value
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
        stringreplace(add_headers, "\r", "");
        stringreplace(add_headers, "\\n", "\n");
        stringreplace(add_headers, "\n", "\r\n");
    } else {
        m_FileName = new TCHAR[strlen(lpszFileName)+1];
      	strcpy(m_FileName, lpszFileName);
    }

	m_szTempFile[0] = TEXT('0');

	LONGLONG runtime = GetSystemTimeInMS();
	hr = ServerPreCheck(m_FileName);
	runtime = GetSystemTimeInMS()-runtime;
    if (FAILED(hr))
    {
        return hr;
    }
	if (runtime > 1500) {
		Log("CHttpStream::Initialize: Remote Server is too slow to render anything! Connect Time: %I64d", runtime);
		return E_FAIL;
	}

    hr = Downloader_Start(m_FileName, 0);
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

void CHttpStream::WaitForSize(LONGLONG start, LONGLONG end) {

    // wait max. ~20s
	int i = 0;
    for (i = 0; i < 2000; i++) {
        if ((m_llFileLengthStartPoint <= start) && ((m_llFileLengthStartPoint+m_llFileLength) >= end)) {
            break;
        }
        Sleep(10);
    }
	if (i >= 2000) {
		Log("CHttpStream::WaitForSize: Timed OUT! (%I64d !<= %I64d) && (%I64d !>= %I64d)", m_llFileLengthStartPoint, start, (m_llFileLengthStartPoint+m_llFileLength), end);
	}
#ifdef _DEBUG
	else {
        Log("CHttpStream::WaitForSize: OK! (%I64d <= %I64d) && (%I64d >= %I64d)", m_llFileLengthStartPoint, start, (m_llFileLengthStartPoint+m_llFileLength), end);
	}
#endif
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

    pos.HighPart = pOverlapped->OffsetHigh;
    pos.LowPart = pOverlapped->Offset;

	LONGLONG llReadEnd = pos.QuadPart + dwBytesToRead;

    Log("CHttpStream::StartRead: Startpos requested: %I64d Endpos requested: %I64d, AvailableStart = %I64d, AvailableEnd = %I64d, Diff Endpos: %I64d",
 	  	 pos.QuadPart, llReadEnd, m_llFileLengthStartPoint, (m_llFileLengthStartPoint+m_llFileLength), ((m_llFileLengthStartPoint+m_llFileLength)-llReadEnd));

    if ((m_llDownloadLength > 0) && (pos.QuadPart > m_llDownloadLength)) {
	   Log("CHttpStream::StartRead: THIS SHOULD NEVER HAPPEN! requested startpos out of max. range - return end of file");
	   m_datalock.Unlock();
	   return HRESULT_FROM_WIN32(38);
    }

    if (
		(pos.QuadPart < m_llFileLengthStartPoint) ||
		(llReadEnd > (m_llFileLengthStartPoint+m_llFileLength))
		)
    {

		Log("CHttpStream::StartRead: Request out of range - wanted start: %I64d end: %I64d min avail: %I64d max avail: %I64d", pos.QuadPart, llReadEnd, m_llFileLengthStartPoint, (m_llFileLengthStartPoint+m_llFileLength));
		// check if we'll reach the barrier within a few seconds
		if ((pos.QuadPart > m_llFileLengthStartPoint) || (llReadEnd > (m_llFileLengthStartPoint+m_llFileLength))) {
			if ((pos.QuadPart > (m_llFileLengthStartPoint+(m_lldownspeed*1024*1024*2))) || (llReadEnd > (m_llFileLengthStartPoint+m_llFileLength+(m_lldownspeed*1024*1024*5)))) {
				Log("CHttpStream::StartRead: will not reach pos. within 2 seconds - speed: %.4Lf MB/s ", m_lldownspeed);
			    m_datalock.Unlock();
                if (m_llSeekPos) {
                   add_to_downloadqueue(pos.QuadPart);
                }
			} else {
			    // out of range BUT will reach Limit in 2 sek.
         		m_datalock.Unlock();
			}
		} else {
		  // out of range BUT not in line so can't reach it
		  m_datalock.Unlock();
          if (m_llSeekPos) {
		     add_to_downloadqueue(pos.QuadPart);
          }
		}

        bWait = TRUE;
	} else {
		m_datalock.Unlock();
	}

    if (bWait)
    {
        // Notify the application that the filter is buffering data.
        if (m_pEventSink)
        {
            m_pEventSink->Notify(EC_BUFFERING_DATA, TRUE, 0);
        }

        m_llBytesRequested = llReadEnd;

		Log("CHttpStream::StartRead: wait for size/pos: %I64d", m_llBytesRequested);
        WaitForSize(pos.QuadPart, m_llBytesRequested);
		m_llBytesRequested = 0;

     	Log("CHttpStream::StartRead: Wait DONE Startpos requested: %I64d Endpos requested: %I64d, AvailableStart = %I64d, AvailableEnd = %I64d",
		pos.QuadPart, llReadEnd, m_llFileLengthStartPoint, (m_llFileLengthStartPoint+m_llFileLength));

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
	// recaluate new start pos
    LARGE_INTEGER new_pos;
    new_pos.LowPart = pOverlapped->Offset;
    new_pos.HighPart = pOverlapped->OffsetHigh;
	new_pos.QuadPart = new_pos.QuadPart - m_llFileLengthStartPoint;
	pOverlapped->Offset = new_pos.LowPart;
	pOverlapped->OffsetHigh = new_pos.HighPart;

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

    Log("CHttpStream::EndRead: Read done Startpos: %I64d (Real: %I64d) Read up to (don't trust this value - some splitters send strange buffers to us): %I64d", pos.QuadPart, (m_llFileLengthStartPoint+pos.QuadPart), (pos.QuadPart+(LONGLONG)pdwBytesRead));

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
    typedef BOOL (*CANCELIOEXPROC)(HANDLE hFile, LPOVERLAPPED lpOverlapped);

    BOOL bResult = 0;
    CANCELIOEXPROC pfnCancelIoEx = NULL;

    HMODULE hKernel32 = LoadLibrary("Kernel32.dll"); 

    if (hKernel32)
    {
        bResult = (pfnCancelIoEx)(m_hFileRead, NULL);

        FreeLibrary(hKernel32);
    }
    else
    {
        bResult = CancelIo(m_hFileRead);
    }

	if (!bResult)
	{
		return HRESULT_FROM_WIN32(GetLastError());
	}
    return S_OK;
}

HRESULT CHttpStream::Length(LONGLONG *pTotal, LONGLONG *pAvailable)
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

		m_llBytesRequested = 5+m_llFileLengthStartPoint; // at least 5 bytes
        m_datalock.Unlock();
        WaitForSize(m_llFileLengthStartPoint, m_llBytesRequested);
        m_datalock.Lock();
		m_llBytesRequested = 0;

		if (m_pEventSink)
        {
            m_pEventSink->Notify(EC_BUFFERING_DATA, FALSE, 0);
        }
    }

    if (!m_llSeekPos) {
        *pTotal = m_llDownloadLength;
        *pAvailable = m_llFileLength;
    } else {
        *pTotal = m_llDownloadLength;
        *pAvailable = *pTotal;
    }

    m_datalock.Unlock();
    //Log("Length called: return: total: %I64d avail: %I64d", *pTotal, *pAvailable);

    return S_OK;
}
