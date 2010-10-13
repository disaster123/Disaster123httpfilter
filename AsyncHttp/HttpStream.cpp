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

#include <iostream>
#include <winsock2.h>

#include <stdio.h>
#include <streams.h>
#include <sstream>
#include <fstream>
#include <crtdbg.h>
#include <atlconv.h>
#include <math.h>
#include <time.h>

#include <tchar.h>
#include <cstdio>

#include "..\Base\asyncio.h"
#include "HttpStream.h"
#include "AutoLockDebug.h"

extern void Log(const char *fmt, ...);
extern void StopLogger();

static CCritSec m_lock;
static CCritSec g_lock;
static CCritSec m_readlock;

std::queue<std::string> m_DownloaderQueue;
BOOL m_DownloaderRunning = FALSE;
HANDLE m_hDownloader = NULL;
HANDLE      m_hReadWaitEvent = INVALID_HANDLE_VALUE;
HANDLE		m_hFileWrite = INVALID_HANDLE_VALUE;   // File handle for writing to the temp file.
HANDLE		m_hFileRead = INVALID_HANDLE_VALUE;    // File handle for reading from the temp file.
LONGLONG    m_llFileLength = 0;         // Current length of the temp file, in bytes
LONGLONG	m_llDownloadLength = 0;
LONGLONG	m_llFileLengthStartPoint = 0; // Start of Current length in bytes
BOOL        m_bComplete = FALSE;            // TRUE if the download is complete.
LONGLONG    m_llBytesRequested = 0;     // Size of most recent read request.
float m_lldownspeed = 1.0;

CHttpStream::~CHttpStream()
{
	Log("~CHttpStream() called");

	m_DownloaderRunning = false;

	// give the thread the time to finish
	Sleep(200);

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

    StopLogger();

	CloseHandle(m_hReadWaitEvent);
}

string GetDownloaderMsg()
{
  if ( m_DownloaderQueue.size() == 0 )
  {
    return "";
  }

  string ret = m_DownloaderQueue.front();
  m_DownloaderQueue.pop();
  return ret;
}

int GetHostAndPath(const char *szUrl, char **pszHost, char **pszPath, int *pszPort)
{
    char *Host;
    char *Path;
    int Port;

    Host = (char *) malloc (strlen(szUrl));
    Path = (char *) malloc (strlen(szUrl));

	if (sscanf(szUrl, "http://%[^:]:%d/%s", Host, &Port, Path) == 3) {
		   //Log("sscanf was 3 %s %d %s", Host, Port, Path);
		   *pszPort = Port;
	   	   *pszHost = Host;
	       *pszPath = Path;
	     return 0;
    } else if (sscanf(szUrl, "http://%[^/]/%s", Host, Path) == 2) {
		   //Log("sscanf was 2 %s %s", Host, Path);
		   *pszPort = 80;
		   *pszHost = Host;
		   *pszPath = Path;
	     return 0;
   }
  
   return 1;
}

std::runtime_error CreateSocketError()
{
    int error = WSAGetLastError();
    char* msg = NULL;
    if(FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                     NULL, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                     reinterpret_cast<char*>(&msg), 0, NULL))
    {
        try
        {
			Log("CreateSocketError: %s", msg);
            LocalFree(msg);
        }
        catch(...)
        {
            LocalFree(msg);
            throw;
        }
    } 
  return std::runtime_error(msg);
}
void DownloaderThread_SendAll(int socket, const char* const buf, const int size)
{
    int bytesSent = 0; // Anzahl Bytes die wir bereits vom Buffer gesendet haben
    do
    {
        int result = send(socket, buf + bytesSent, size - bytesSent, 0);
        if(result < 0) // Wenn send einen Wert < 0 zurück gibt deutet dies auf einen Fehler hin.
        {
            throw CreateSocketError();
        }
        bytesSent += result;
    } while(bytesSent < size);
}

DWORD DownloaderThread_WriteData(char *buffer, int buffersize)
{
	//CAutoLockDebug WriteData(&m_lock, __LINE__, __FILE__,__FUNCTION__);
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

void DownloaderThread_GetLine(int socket, string& line) {
	line.clear();
    for(char c; recv(socket, &c, 1, 0) > 0; line += c)
    {
        if(c == '\n')
        {
			// Log("Got Line Complete: %s", line.c_str());
			line += c;
            return;
        }
    }
    throw CreateSocketError(); 
}

ULONGLONG GetSystemTimeInMS() {
  SYSTEMTIME systemTime;
  GetSystemTime(&systemTime);

  FILETIME fileTime;
  SystemTimeToFileTime(&systemTime, &fileTime);

  ULARGE_INTEGER uli;
  uli.LowPart = fileTime.dwLowDateTime; // could use memcpy here!
  uli.HighPart = fileTime.dwHighDateTime;

  ULONGLONG systemTimeIn_ms(uli.QuadPart/10000);
  return systemTimeIn_ms;
}

double Round(double Zahl, int Stellen)
{
    double v[] = { 1, 10, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8 };  // mgl. verlängern
    return floor(Zahl * v[Stellen] + 0.5) / v[Stellen];
}

UINT CALLBACK DownloaderThread(void* param)
{
  //Log("DownloaderThread started");
  WSADATA w;
  string firstline;
  firstline.empty();
  if(int result = WSAStartup(MAKEWORD(2,2), &w) != 0)
  {
     Log("DownloaderThread: Winsock 2 konnte nicht gestartet werden! Error %d", result);
     return 1;
  } 
  while ( m_DownloaderRunning && !m_bComplete ) {
	if ( m_DownloaderQueue.size() > 0 || ((firstline.length() > 0) && (m_DownloaderQueue.size() == 0)) ) {
       string line;
	   if ((firstline.length() > 0) && (m_DownloaderQueue.size() == 0)) {
			line = firstline.c_str();
		   	m_bComplete = FALSE;
			m_llFileLengthStartPoint = 0;
			m_llFileLength = 0;
	   } else {
  	        line = GetDownloaderMsg();
	   }
	   char url[500];
	   LONGLONG startpos = -1;
	   m_llDownloadLength = 0;

	   //Log("DownloaderThread: Got String: %s", line.c_str());
	   sscanf(line.c_str(), "%s || %I64d", url, &startpos);
	   if (firstline.length() == 0) {
	      firstline += line.c_str();
	   }
	   Log("DownloaderThread: Detected URL: %s Startpos: %I64d", url, startpos);

	   char *szHost = NULL;
       char *szPath = NULL;
	   int szPort = NULL;

       int err = GetHostAndPath(url, &szHost, &szPath, &szPort);
       if (err != 0)
       {
		   Log("DownloaderThread: GetHostAndPath Error", HRESULT_FROM_WIN32(err));
       }

	   //Log("DownloaderThread: Detected URL: %s Port %d Path %s", szHost, szPort, szPath);

	   int Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	   if (Socket == -1) {
		   Log("DownloaderThread: Socket konnte nicht erstellt werden! %d", Socket);
       }   
       sockaddr_in service; // Normale IPv4 Struktur
       service.sin_family = AF_INET; // AF_INET für IPv4, für IPv6 wäre es AF_INET6
	   service.sin_port = htons(szPort); // Das HTTP-Protokoll benutzt Port 80
	   // szHost to IP
	   hostent* phe = gethostbyname(szHost);
	   if(phe == NULL) {
          Log("DownloaderThread: Hostname %s konnte nicht aufgelöst werden.", szHost);
		  ExitThread(1);
       }
	   if(phe->h_addrtype != AF_INET) {
          Log("DownloaderThread: Keine IPv4 Adresse gefunden!");
		  ExitThread(1);
       }
       if(phe->h_length != 4) {
          Log("DownloaderThread: Keine IPv4 Adresse gefunden!");
		  ExitThread(1);
       }
	   char *szIP = inet_ntoa(*reinterpret_cast<in_addr*>(*phe->h_addr_list));
	   Log("DownloaderThread: Host: %s mit IP: %s", szHost, szIP);
       service.sin_addr.s_addr = inet_addr(szIP);

	   int result = connect(Socket, reinterpret_cast<sockaddr*>(&service), sizeof(service));
	   if (result == -1) {
		  closesocket(Socket);
          Log("DownloaderThread: Connect fehlgeschlagen!");
		  ExitThread(1);
       }

	   char request[2000];
	   sprintf(request, "GET /%s HTTP/1.1\r\nHost: %s:%d\r\nRange: Bytes=%I64d-\r\nConnection: close\r\n\r\n", szPath, szHost, szPort, startpos);
	   //Log("Sending Request: %s", request);

	   try {
	      DownloaderThread_SendAll(Socket, request, sizeof(request));
	   } catch(exception& ex) {
          Log("DownloaderThread: Fehler beim senden des Requests %s!", ex);
		  ExitThread(1);
	   }

       // Read Header and ignore
	   string HeaderLine;
	   LONGLONG tmp1,tmp2,contlength,contrange;
	   contlength=0;
       contrange=0;
	   for (int loop = 0; loop < 20; loop++) {
		   try {
     	       DownloaderThread_GetLine(Socket, HeaderLine);
			   //Log("DownloaderThread: Headerline: %s", HeaderLine.c_str());
			   // Content-Range: bytes 1555775744-1555808025/1555808026
			   sscanf(HeaderLine.c_str(), "Content-Length: %I64d", &contlength);
			   sscanf(HeaderLine.c_str(), "Content-Range: bytes %I64d-%I64d/%I64d", &tmp1, &tmp2, &contrange);
			   if (strcmp(HeaderLine.c_str(), "\r\n") == 0) {
				   m_llDownloadLength = (contrange > contlength) ? contrange : contlength;
				   break;
			   }
		   } catch(...) {
			   Log("DownloaderThread: Headerfailure");
		   }
	   }
	   Log("DownloaderThread: Headers complete Downloadsize: %I64d", m_llDownloadLength);

	   char buffer[1024*256];
	   LONGLONG bytesrec = 0;
	   int mb_print_counter = 1;
	   LONGLONG bytesrec_sum = 0;
	   LONGLONG bytesrec_sum_old = 0;
	   LONGLONG recv_calls = 0;
	   LONGLONG start = GetSystemTimeInMS();
	   do {
		   bytesrec = recv(Socket, buffer, sizeof(buffer), 0);
		   recv_calls++;

		   if (bytesrec > 0) {
     		   bytesrec_sum += bytesrec;

			   // If we move this under DownloadThread so that we don't need the next if again it doesn't work... no idea why
		       if (m_DownloaderQueue.size() > 0) {
			     LONGLONG end = GetSystemTimeInMS();
  			     int diff = (int)(end-start)/1000;
				 Log("DownloaderThread: Downloaded MB (found new queue request): %.2Lf tooked time: %d Speed: %.4Lf MB/s", ((float)bytesrec_sum/1024/1024), diff, Round(((float)bytesrec_sum/1024/1024)/diff, 4));
			     Log("DownloaderThread: found new queue request - so cancel");

		         if ((m_llBytesRequested > 0) && ((m_llFileLength+m_llFileLengthStartPoint) >= m_llBytesRequested)) {
				    Log("DownloaderThread: Tick m_hReadWaitEvent Request READY!: Requested until Pos = %I64d, max. Available = %I64d", m_llBytesRequested, m_llFileLength+m_llFileLengthStartPoint);
				    SetEvent(m_hReadWaitEvent);
 			     }

				 break;
		       }

		       DownloaderThread_WriteData(buffer, bytesrec);

		       if ((m_llBytesRequested > 0) && ((m_llFileLength+m_llFileLengthStartPoint) >= m_llBytesRequested)) {
				    Log("DownloadThread: Tick m_hReadWaitEvent Request READY!: Requested until Pos = %I64d, max. Available = %I64d", m_llBytesRequested, m_llFileLength+m_llFileLengthStartPoint);
				    SetEvent(m_hReadWaitEvent);
			   }
  			   if ((int)(bytesrec_sum/1024/1024/5) == mb_print_counter) {
			     mb_print_counter++;
			     LONGLONG end = GetSystemTimeInMS();
			 	 LONGLONG bytesdiff = bytesrec_sum-bytesrec_sum_old;
		 		 float timediff = (end-start)/1000;
	 			 m_lldownspeed = (float)Round(((float)bytesdiff/1024/1024)/timediff, 4);
				 Log("DownloaderThread: Downloaded %.2LfMB time: %.2Lf Speed: %.4Lf MB/s Recv: %I64d Last requested: %I64d", ((float)bytesrec_sum/1024/1024), timediff, m_lldownspeed, recv_calls, m_llBytesRequested);
				 start = GetSystemTimeInMS();
				 bytesrec_sum_old = bytesrec_sum;
			   }
		   } else  {
			   Log("DownloaderThread: 0 or - bytes received - bytesrec: %I64d", bytesrec);
		   }

	   } while (bytesrec > 0 && m_DownloaderRunning);

	   Log("DownloaderThread: Download compl./canceled - startpos: %I64d downloaded: %I64d Bytes Header Bytes: %I64d - bytesrec: %I64d - m_DownloaderRunning: %s", startpos, bytesrec_sum, m_llDownloadLength, bytesrec, (m_DownloaderRunning)?"true":"false");

	   if ((startpos == 0) && (bytesrec_sum == m_llDownloadLength || m_llDownloadLength == 0) && !m_DownloaderRunning) {
		   Log("DownloaderThread: Download completely completed - BREAK!");
		   m_bComplete = true;
		   firstline.empty();
   	       closesocket(Socket); 
		   break;
	   }

	   // Verbindung beenden
	   closesocket(Socket); 
    }
    Sleep(50);
  }
  Log("DownloaderThread ended");

  ExitThread(0);
}

void StartDownloader()
{
  UINT id;
  m_hDownloader = (HANDLE)_beginthreadex(NULL, 0, DownloaderThread, 0, 0, &id);
}


HRESULT CHttpStream::Downloader_Start(TCHAR* szUrl, LONGLONG startpoint) 
{
  char msg[500];
  Log("CHttpStream::Downloader_Start called with URL: %s Startpos: %I64d", szUrl, startpoint);

  sprintf_s(msg, 500, "%s || %I64d", szUrl, startpoint);
  m_DownloaderQueue.push((string)msg);

  if (!m_hDownloader || !m_DownloaderRunning) 
  {
    m_DownloaderRunning = true;
    StartDownloader();
  }

  return S_OK;
};



//************************************************************************
//  CreateTempFile
//  Creates a temp file for the HTTP download.
//***********************************************************************/

HRESULT CHttpStream::CreateTempFile()
{
    //CAutoLockDebug mLock(&m_lock, __LINE__, __FILE__,__FUNCTION__);

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
	UINT uval = GetTempFileName(szTempPath, TEXT("ASY"), 0, m_szTempFile);

	delete [] szTempPath;

	if (uval == 0)
	{
		return HRESULT_FROM_WIN32(GetLastError());
	}

	Log("Created Tempfile %s", m_szTempFile);

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


//************************************************************************
//  Initialize
//  Initializes the stream.
//
//  lpszFileName: Contains the URL of the file to download.
//***********************************************************************/

HRESULT CHttpStream::ReInitialize(LPCTSTR lpszFileName, LONGLONG startbytes)
{
    USES_CONVERSION;
    //CAutoLockDebug mLock(&m_lock, __LINE__, __FILE__,__FUNCTION__);

    CloseHandle(m_hFileWrite);
	m_hFileWrite = INVALID_HANDLE_VALUE;

	CloseHandle(m_hFileRead);
	m_hFileRead = INVALID_HANDLE_VALUE;

	// Delete old temp file to store the data.
    if (m_szTempFile[0] != TEXT('0'))
    {
        DeleteFile(m_szTempFile);
    }

    // Create a temp file to store the data.
	HRESULT hr = CreateTempFile();
    if (FAILED(hr))
    {
        return hr;
    }

	// init vars
	m_bComplete = FALSE;
	m_llFileLengthStartPoint = startbytes;
	m_llFileLength = 0;

	hr = Downloader_Start(m_FileName, startbytes);
    if (FAILED(hr))
    {
        return hr;
    }

	return S_OK;
}


HRESULT CHttpStream::Initialize(LPCTSTR lpszFileName) 
{
    USES_CONVERSION;
    // CAutoLockDebug mLock(&m_lock, __LINE__, __FILE__,__FUNCTION__);

	Log("(HttpStream) CHttpStream::Initialize %s", lpszFileName);

	HRESULT hr = S_OK;

	m_DownloaderRunning = FALSE;
    m_hDownloader = NULL;
    m_hReadWaitEvent = INVALID_HANDLE_VALUE;
    m_hFileWrite = INVALID_HANDLE_VALUE;   // File handle for writing to the temp file.
    m_hFileRead = INVALID_HANDLE_VALUE;    // File handle for reading from the temp file.
    m_llFileLength = 0;         // Current length of the temp file, in bytes
    m_llDownloadLength = 0;
    m_llFileLengthStartPoint = 0; // Start of Current length in bytes
    m_bComplete = FALSE;            // TRUE if the download is complete.
    m_llBytesRequested = 0;     // Size of most recent read request.
    m_lldownspeed = 1.0;

    // Create the event used to wait for the download.
    m_hReadWaitEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (m_hReadWaitEvent == NULL)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // Create a temp file to store the data.
	hr = CreateTempFile();
    if (FAILED(hr))
    {
        return hr;
    }

	// init vars
	m_bComplete = FALSE;
	m_llFileLengthStartPoint = 0;
	m_llFileLength = 0;

	m_FileName = new TCHAR[strlen(lpszFileName)+1];
	strcpy(m_FileName, lpszFileName);

    // Start the download. The download will complete
    // asynchronously.
	hr = Downloader_Start(m_FileName, 0);
    if (FAILED(hr))
    {
        return hr;
    }

	return S_OK;
}


// Implementation of CAsyncStream methods

HRESULT CHttpStream::StartRead(
	PBYTE pbBuffer,
	DWORD dwBytesToRead,
	BOOL bAlign,
	LPOVERLAPPED pOverlapped,
	LPBOOL pbPending,
	LPDWORD pdwBytesRead
	)
{
    HRESULT hr = S_OK;

    LARGE_INTEGER pos;

    pos.HighPart = pOverlapped->OffsetHigh;
    pos.LowPart = pOverlapped->Offset;

	LONGLONG llReadEnd = pos.QuadPart + dwBytesToRead;

	BOOL bResult;
	DWORD err;
		
	*pbPending = FALSE;
    BOOL bWait = FALSE;

//	Log("CHttpStream::StartRead: Diff Endpos: %I64d Startpos requested: %I64d Endpos requested: %I64d, AvailableStart = %I64d, FileLength = %I64d, AvailableEnd = %I64d",
//		((m_llFileLengthStartPoint+m_llFileLength)-llReadEnd), pos.QuadPart, llReadEnd, m_llFileLengthStartPoint, m_llFileLength, (m_llFileLengthStartPoint+m_llFileLength));

    if (
		(pos.QuadPart < m_llFileLengthStartPoint) ||
		(llReadEnd > (m_llFileLengthStartPoint+m_llFileLength))
		)
    {
        // The caller has requested data past the amount 
        // that has been downloaded so far. We'll need to wait.

		Log("CHttpStream::StartRead: Request out of range - wanted start: %I64d end: %I64d min avail: %I64d max avail: %I64d", pos.QuadPart, llReadEnd, m_llFileLengthStartPoint, (m_llFileLengthStartPoint+m_llFileLength));
		// check if we'll reach the barrier within a few seconds
		if ((pos.QuadPart > m_llFileLengthStartPoint) || (llReadEnd > (m_llFileLengthStartPoint+m_llFileLength))) {
			if ((pos.QuadPart > (m_llFileLengthStartPoint+(m_lldownspeed*1024*1024*5))) || (llReadEnd > (m_llFileLengthStartPoint+m_llFileLength+(m_lldownspeed*1024*1024*5)))) {
				Log("CHttpStream::StartRead: will not reach pos. within 5 seconds - speed: %.4Lf MB/s ", m_lldownspeed);
                ReInitialize(m_FileName, pos.QuadPart);
			} else {
			   // out of range BUT will reach Limit in 5 sek.
			}
		} else {
		  // out of range BUT not in line so can't reach it
		  ReInitialize(m_FileName, pos.QuadPart);
		}

        bWait = TRUE;
    } 

    if (bWait)
    {
        // Notify the application that the filter is buffering data.
        if (m_pEventSink)
        {
            m_pEventSink->Notify(EC_BUFFERING_DATA, TRUE, 0);
        }

		// Wait for the data to be downloaded.
        m_llBytesRequested = llReadEnd;
		Log("CHttpStream::StartRead: Wait for m_hReadWaitEvent - wait for size/pos: %I64d", m_llBytesRequested);
        WaitForSingleObject(m_hReadWaitEvent, INFINITE);
		m_llBytesRequested = 0;

     	Log("CHttpStream::StartRead: Wait for m_hReadWaitEvent DONE Startpos requested: %I64d Endpos requested: %I64d, AvailableStart = %I64d, AvailableEnd = %I64d",
		pos.QuadPart, llReadEnd, m_llFileLengthStartPoint, (m_llFileLengthStartPoint+m_llFileLength));

		if (m_pEventSink)
        {
            m_pEventSink->Notify(EC_BUFFERING_DATA, FALSE, 0);
        }
    }

	//Log("CHttpStream::StartRead: Read from Temp File BytesToRead: %u Startpos: %u", dwBytesToRead, pOverlapped->Offset);
    // Read the data from the temp file. (Async I/O request.)
	pOverlapped->Offset = pOverlapped->Offset - m_llFileLengthStartPoint;
	//Log("CHttpStream::StartRead: Read from Temp File %d %d %d", dwBytesToRead, pOverlapped->Offset, pOverlapped->OffsetHigh);
	//m_lock.Lock();
	bResult = ReadFile(
        m_hFileRead, 
        pbBuffer, 
        dwBytesToRead, 
        pdwBytesRead,
        pOverlapped
        );
	//m_lock.Unlock();
	//Log("CHttpStream::StartRead: Readed from Temp File");

    if (bResult == 0)
    {
        err = GetLastError();
        if (err == ERROR_IO_PENDING)
        {
			//Log("ReadFile got IO_PENDING: %d - IO: %d", err, ERROR_IO_PENDING);
            *pbPending = TRUE;
        }
        else
        {
            Log("ReadFile failed (err = %d) 38 => EOF", err);
            // An actual error occurred.
            hr = HRESULT_FROM_WIN32(err);
        }
    }
	return hr;
}

void CHttpStream::Lock() {
	//Log("CHttpStream::Lock(): called");
	g_lock.Lock();
}

void CHttpStream::Unlock() {
	//Log("CHttpStream::Unlock(): called");
	g_lock.Unlock();
}

HRESULT CHttpStream::EndRead(
    LPOVERLAPPED pOverlapped, 
    LPDWORD pdwBytesRead
    )
{
    //Log("CHttpStream::EndRead called - cancel IO");

    BOOL bResult = 0;

    // Complete the async I/O request.
    bResult = GetOverlappedResult(m_hFileRead, pOverlapped, pdwBytesRead, TRUE);

    if (!bResult)
    {
		DWORD err = GetLastError();
		LPTSTR Error = 0;
		FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,                    NULL,                    err,                    0,                    (LPTSTR)&Error,                    0,                    NULL);
        Log("File error! %s", Error);

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

    // pAvailable can be NULL in IAsyncReader::Length,
    // but not in this method.

    // NOTE: While the file is still downloading, if
    // we return a smaller size, it may confuse the
    // downstream filter. (e.g., the AVI Splitter)

    // Therefore, if the file is not complete yet,
    // we try to get the size that was returned in
    // the HTTP headers. If that doesn't work, we
    // simply block until the entire file is downloaded.

    if (m_bComplete)
    {
		*pTotal = m_llFileLength;
        *pAvailable = *pTotal;

		return S_OK;
    }
    
    // The file is still downloading.
    LONGLONG cbSize = m_llDownloadLength;
    if (cbSize == 0)
    {
		Log("CHttpStream::Length: is 0 wait until a few bytes are here!");
		m_llBytesRequested = 5;
        WaitForSingleObject(m_hReadWaitEvent, INFINITE);
		m_llBytesRequested = 0;

        *pTotal = m_llFileLength;
        *pAvailable = *pTotal;
        return S_OK;
    }

    *pTotal = cbSize;
    *pAvailable = *pTotal;

    return S_OK;
}

