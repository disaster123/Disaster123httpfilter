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

#include <stdio.h>
#include <streams.h>
#include <crtdbg.h>
#include <atlconv.h>

#include <tchar.h>
#include <cstdio>

#include "..\Base\asyncio.h"
#include "HttpStream.h"

extern void Log(const char *fmt, ...);

CHttpStream::~CHttpStream()
{
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

    CloseHandle(m_hReadWaitEvent);
}



//************************************************************************
//  CreateTempFile
//  Creates a temp file for the HTTP download.
//***********************************************************************/

HRESULT CHttpStream::CreateTempFile()
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

	Log("(HttpStream) CHttpStream::ReInitialize %s Start: %I64d\n", lpszFileName, startbytes);
	Sleep(1000);

	CAutoLock lock(&m_DataLock);

	Log("(HttpStream) CHttpStream::ReInitialize after LOCK\n");
	Sleep(1000);

	HRESULT hr = S_OK;

	hr = m_HttpRequest.End();
    if (FAILED(hr))
    {
        return hr;
    }

	Log("(HttpStream) CHttpStream::ReInitialize after m_HttpRequest.End call\n");

	// Intialize the HTTP session.
	hr = m_HttpRequest.InitializeSession(this);
    if (FAILED(hr))
    {
        return hr;
    }

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
	hr = CreateTempFile();
    if (FAILED(hr))
    {
        return hr;
    }

	// init vars
	m_bComplete = FALSE;
	m_llFileLengthStartPoint = startbytes;
	m_llFileLength = 0;

	char lpszHeaders[255];
	sprintf_s(lpszHeaders,"Range: bytes=%I64d-\r\n", startbytes);

    // Start the download. The download will complete
    // asynchronously.
	Log("(HttpStream) m_HttpRequest.SendRequest Start\n");

	hr = m_HttpRequest.SendRequest(A2W(lpszFileName), A2W(lpszHeaders));
    if (FAILED(hr))
    {
        return hr;
    }

	Log("(HttpStream) m_HttpRequest.SendRequest done\n");
	Sleep(1000);

	return S_OK;
}


HRESULT CHttpStream::Initialize(LPCTSTR lpszFileName) 
{
    USES_CONVERSION;

	Log("(HttpStream) CHttpStream::Initialize %s\n", lpszFileName);

	if (m_hFileWrite != INVALID_HANDLE_VALUE)
	{
		return E_FAIL;
	}

	HRESULT hr = S_OK;

    // Create the event used to wait for the download.
    m_hReadWaitEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (m_hReadWaitEvent == NULL)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // Intialize the HTTP session.
	hr = m_HttpRequest.InitializeSession(this);
    if (FAILED(hr))
    {
        return hr;
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

    // Start the download. The download will complete
    // asynchronously.
    hr = m_HttpRequest.SendRequest(A2W(lpszFileName), NULL);
    if (FAILED(hr))
    {
        return hr;
    }

    // we have to free this??
	/*if (m_FileName[0] = '0') {
   	   DbgLog((LOG_ERROR, 1, TEXT("(HttpStream) CHttpStream::Initialize delete [] m_FileName\n")));
  	   delete [] m_FileName;
	}*/

	m_FileName = new TCHAR[strlen(lpszFileName)+1];
	strcpy(m_FileName, lpszFileName);

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

    CAutoLock lock(&m_CritSec);

    HRESULT hr = S_OK;

    LARGE_INTEGER pos;

    pos.HighPart = pOverlapped->OffsetHigh;
    pos.LowPart = pOverlapped->Offset;

	LONGLONG llReadEnd = pos.QuadPart + dwBytesToRead;
    LONGLONG llFileLen = 0;

	BOOL bResult;
	DWORD err;
		
	*pbPending = FALSE;

	Log("CHttpStream::StartRead: Diff Endpos: %I64d Startpos requested: %I64d Endpos requested: %I64d, AvailableStart = %I64d, FileLength = %I64d, AvailableEnd = %I64d",
		((m_llFileLengthStartPoint+m_llFileLength)-llReadEnd), pos.QuadPart, llReadEnd, m_llFileLengthStartPoint, m_llFileLength, (m_llFileLengthStartPoint+m_llFileLength));

    BOOL bWait = FALSE;

    m_DataLock.Lock();
    if (
		(pos.QuadPart < m_llFileLengthStartPoint) ||
		(llReadEnd > (m_llFileLengthStartPoint+m_llFileLength))
		)
    {
        // The caller has requested data past the amount 
        // that has been downloaded so far. We'll need to wait.

		Log("CHttpStream::StartRead: Request out of range - wanted start: %I64d end: %I64d min avail: %I64d max avail: %I64d", pos.QuadPart, llReadEnd, m_llFileLengthStartPoint, (m_llFileLengthStartPoint+m_llFileLength));

        m_DataLock.Unlock();
		ReInitialize(m_FileName, pos.QuadPart);
		Log("CHttpStream::StartRead: Reinitiated");

        m_DataLock.Lock();
        m_llBytesRequested = llReadEnd;
        bWait = TRUE;
    } 
    m_DataLock.Unlock();

    if (bWait)
    {
        // Notify the application that the filter is buffering data.
        if (m_pEventSink)
        {
            m_pEventSink->Notify(EC_BUFFERING_DATA, TRUE, 0);
        }

		Log("CHttpStream::StartRead: Wait for m_hReadWaitEvent");

		// Wait for the data to be downloaded.
        WaitForSingleObject(m_hReadWaitEvent, INFINITE);

     	Log("CHttpStream::StartRead: Wait for m_hReadWaitEvent DONE Startpos requested: %I64d Endpos requested: %I64d, AvailableStart = %I64d, AvailableEnd = %I64d",
		pos.QuadPart, llReadEnd, m_llFileLengthStartPoint, (m_llFileLengthStartPoint+m_llFileLength));

		if (m_pEventSink)
        {
            m_pEventSink->Notify(EC_BUFFERING_DATA, FALSE, 0);
        }
    }

	Log("CHttpStream::StartRead: Read from Temp File");
    // Read the data from the temp file. (Async I/O request.)
    bResult = ReadFile(
        m_hFileRead, 
        pbBuffer, 
        dwBytesToRead, 
        pdwBytesRead,
        pOverlapped
        );

    if (bResult == 0)
    {
        err = GetLastError();
        if (err == ERROR_IO_PENDING)
        {
            *pbPending = TRUE;
        }
        else
        {
            Log("ReadFile failed (err = %d)", err);
            // An actual error occurred.
            hr = HRESULT_FROM_WIN32(err);
        }
    }
	return hr;
}



HRESULT CHttpStream::EndRead(
    LPOVERLAPPED pOverlapped, 
    LPDWORD pdwBytesRead
    )
{
    Log("CHttpStream::EndRead");

    CAutoLock lock(&m_CritSec);

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

    CAutoLock lock(&m_DataLock);

    // NOTE: While the file is still downloading, if
    // we return a smaller size, it may confuse the
    // downstream filter. (e.g., the AVI Splitter)

    // Therefore, if the file is not complete yet,
    // we try to get the size that was returned in
    // the HTTP headers. If that doesn't work, we
    // simply block until the entire file is downloaded.

    //DbgLog((LOG_TRACE, 0, TEXT("CHttpStream::Length called!")));
    if (m_bComplete)
    {
		// DbgLog((LOG_TRACE, 0, TEXT("CHttpStream::Length: Downloaded file is complete - so put in the real values!")));

		*pTotal = m_llFileLength;
        *pAvailable = *pTotal;
        return S_OK;
    }
    
    // The file is still downloading.

    DWORD cbSize = m_HttpRequest.FileSize();
    if (cbSize == 0)
    {
		Log("CHttpStream::Length: This should NOT happen - we should always have a filesize through http!");
        while (!m_bComplete)
        {
            WaitForSingleObject(m_hReadWaitEvent, INFINITE);
        }

        *pTotal = m_llFileLength;
        *pAvailable = *pTotal;
        return S_OK;
    }

    // we always return the FULL size - as we allow seeking in http
	// STEFAN 
/*    *pTotal = cbSize;
	*pAvailable = m_llFileLength;

	return VFW_S_ESTIMATED;*/
	
    *pTotal = cbSize;
    *pAvailable = *pTotal;

    return S_OK;
}



// Implementation of HttpRequestCB
// (Handlers for WinHttp callback function.)

void CHttpStream::OnData(BYTE *pData, DWORD cbData) 
{
    Log("CHttpStream::OnData() before Lock");
    CAutoLock lock(&m_DataLock);
    Log("CHttpStream::OnData() after Lock");

    BOOL bResult = 0;
    DWORD cbWritten = 0;

	//DbgLog((LOG_ERROR, 0, TEXT("CHttpStream::OnData: WriteFile")));

	if (m_hFileWrite == INVALID_HANDLE_VALUE) {
		return;
	}

    // Write the data to the temp file.
    bResult = WriteFile(m_hFileWrite, pData, cbData, &cbWritten, NULL);
    if (!bResult)
    {
		DWORD err = GetLastError();
		LPTSTR Error = 0;
		FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,                    NULL,                    err,                    0,                    (LPTSTR)&Error,                    0,                    NULL);

		Log("CHttpStream::OnData: Cannot write file! %s", Error);
    }

    m_llFileLength += cbWritten;

    if ((m_llBytesRequested > 0) && (m_llFileLength >= m_llBytesRequested))
    {
		Log("CHttpStream::OnData Tick m_hReadWaitEvent Request READY!: Requested = %I64d, available = %I64d", 
            m_llBytesRequested, m_llFileLength);

        m_llBytesRequested = 0;

        SetEvent(m_hReadWaitEvent);
    }
} 

void CHttpStream::OnEndOfStream() 
{
	// TODO - the "old" OnEndOfStream seems to get calles here when we init a new instance
	//CAutoLock lock(&m_DataLock);

    //m_bComplete = TRUE;

    //CloseHandle(m_hFileWrite);
    //m_hFileWrite = INVALID_HANDLE_VALUE;

    //Log("CHttpStream::OnEndOfStream: Tick m_hReadWaitEvent");
    //SetEvent(m_hReadWaitEvent);
} 

void CHttpStream::OnError(DWORD dwErr) 
{
/*	Log("CHttpStream::OnError: Http streaming error: 0x%X", HRESULT_FROM_WIN32(dwErr));

    if (m_pEventSink)
    {
        m_pEventSink->Notify(EC_ERRORABORT, TRUE, 0);
    }

    OnEndOfStream();*/
} 
