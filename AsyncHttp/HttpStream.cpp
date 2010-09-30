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

#include "..\Base\asyncio.h"
#include "HttpStream.h"

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

	DbgLog((LOG_TRACE, 0, TEXT("Created Tempfile %s", m_szTempFile)));

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
		DbgLog((LOG_ERROR, 0, TEXT("Could not create temp file %s"), m_szTempFile));

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
		DbgLog((LOG_ERROR, 0, TEXT("Could not open temp file for reading.")));

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

HRESULT CHttpStream::Initialize(LPCTSTR lpszFileName)
{

    DbgLog((LOG_ERROR, 0, TEXT("(HttpStream) CHttpStream::Initialize %s\n"), lpszFileName));

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

    // Start the download. The download will complete
    // asynchronously.
    hr = m_HttpRequest.SendRequest(lpszFileName);
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

    CAutoLock lock(&m_CritSec);

    HRESULT hr = S_OK;

    LARGE_INTEGER pos;

    pos.HighPart = pOverlapped->OffsetHigh;
    pos.LowPart = pOverlapped->Offset;

    LONGLONG llReadEnd = pos.QuadPart + dwBytesToRead;

    DbgLog((LOG_TRACE, 15, TEXT("CHttpStream::StartRead @ %I64d (read %d bytes)"), 
        pos.QuadPart, dwBytesToRead));

    LONGLONG llFileLen = 0;

	BOOL bResult;
	DWORD err;
		
	*pbPending = FALSE;

    DbgLog((LOG_TRACE, 15, TEXT("Data requested: %I64d, Available = %I64d"),
        llReadEnd, m_llFileLength));

    BOOL bWait = FALSE;

    m_DataLock.Lock();

    if ((llReadEnd > m_llFileLength && !m_bComplete))
    {
        // The caller has requested data past the amount 
        // that has been downloaded so far. We'll need to wait.

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

        // Wait for the data to be downloaded.
        WaitForSingleObject(m_hReadWaitEvent, INFINITE);

        if (m_pEventSink)
        {
            m_pEventSink->Notify(EC_BUFFERING_DATA, FALSE, 0);
        }
    }

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
            DbgLog((LOG_ERROR, 0, TEXT("ReadFile failed (err = %d)"), err));
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
    DbgLog((LOG_TRACE, 15, TEXT("CHttpStream::EndRead")));

    CAutoLock lock(&m_CritSec);

    BOOL bResult = 0;
    DWORD err = 0;

    // Complete the async I/O request.

    bResult = GetOverlappedResult(m_hFileRead, pOverlapped, pdwBytesRead, TRUE);

    if (!bResult)
    {
        err = GetLastError();
        DbgLog((LOG_ERROR, 0, TEXT("File error! %d"), err));

        return HRESULT_FROM_WIN32(err);
    }

	return S_OK;
}

HRESULT CHttpStream::Cancel()
{
    typedef BOOL (*CANCELIOEXPROC)(HANDLE hFile, LPOVERLAPPED lpOverlapped);

    BOOL bResult = 0;
    CANCELIOEXPROC pfnCancelIoEx = NULL;

    HMODULE hKernel32 = LoadLibrary(L"Kernel32.dll"); 

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

    if (m_bComplete)
    {
        *pTotal = m_llFileLength;
        *pAvailable = *pTotal;
        return S_OK;
    }
    
    // The file is still downloading.

    DWORD cbSize = m_HttpRequest.FileSize();

    if (cbSize == 0)
    {
        while (!m_bComplete)
        {
            WaitForSingleObject(m_hReadWaitEvent, INFINITE);
        }

        *pTotal = m_llFileLength;
        *pAvailable = *pTotal;
        return S_OK;

    }

    // Return the estimated size

    *pTotal = cbSize;
    *pAvailable = m_llFileLength; // This is the current length.

    return VFW_S_ESTIMATED;
}



// Implementation of HttpRequestCB
// (Handlers for WinHttp callback function.)

void CHttpStream::OnData(BYTE *pData, DWORD cbData) 
{
    CAutoLock lock(&m_DataLock);

    BOOL bResult = 0;
    DWORD cbWritten = 0;

    // Write the data to the temp file.
    bResult = WriteFile(m_hFileWrite, pData, cbData, &cbWritten, NULL);
    if (!bResult)
    {
        DbgLog((LOG_ERROR, 0, TEXT("Cannot write file!")));
    }

    m_llFileLength += cbWritten;

    if ((m_llBytesRequested > 0) && (m_llFileLength >= m_llBytesRequested))
    {
        DbgLog((LOG_TRACE, 15, TEXT("Requested = %I64d, available = %I64d"), 
            m_llBytesRequested, m_llFileLength));

        m_llBytesRequested = 0;

        SetEvent(m_hReadWaitEvent);
    }
} 

void CHttpStream::OnEndOfStream() 
{
    DbgLog((LOG_TRACE, 0, TEXT("End of stream")));
    CAutoLock lock(&m_DataLock);

    m_bComplete = TRUE;

    CloseHandle(m_hFileWrite);

    m_hFileWrite = INVALID_HANDLE_VALUE;

    SetEvent(m_hReadWaitEvent);
} 

void CHttpStream::OnError(DWORD dwErr) 
{
    DbgLog((LOG_ERROR, 0, TEXT("Http streaming error: 0x%X"), HRESULT_FROM_WIN32(dwErr)));

    if (m_pEventSink)
    {
        m_pEventSink->Notify(EC_ERRORABORT, TRUE, 0);
    }

    OnEndOfStream();        
} 
