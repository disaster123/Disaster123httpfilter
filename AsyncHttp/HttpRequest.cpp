/*************************************************************************
*  HttpRequest.cpp: HTTP request helper class.
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
#include "HttpRequest.h"

#include <errno.h>

HttpRequest::~HttpRequest()
{
    if (m_hSession)
    {
        // Remove the status callback.
        WinHttpSetStatusCallback(
            m_hSession, 
            NULL, 
            WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS, 
            NULL
            );
    }

    WinHttpCloseHandle(m_hRequest);
    WinHttpCloseHandle(m_hConnect);
    WinHttpCloseHandle(m_hSession);

    delete [] m_pBuffer;
}

HRESULT HttpRequest::End()
{
    if (m_hSession)
    {
        // Remove the status callback.
        WinHttpSetStatusCallback(
            m_hSession, 
            NULL, 
            WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS, 
            NULL
            );
    }

    WinHttpCloseHandle(m_hRequest);
    WinHttpCloseHandle(m_hConnect);
    WinHttpCloseHandle(m_hSession);

    delete [] m_pBuffer;

	return S_OK;
}


//************************************************************************
//  InitializeSession
//  Creates the HTTP session.
//
//  pCB: Pointer to a callback interface, implemented by the caller. The
//       caller will be notified when HTTP data is ready (or errors
//       occur, etc).
//***********************************************************************/

HRESULT HttpRequest::InitializeSession(HttpRequestCB *pCB)
{
    if (pCB == NULL)
    {
        return E_POINTER;
    }

    m_pCB = pCB;

    m_hSession = WinHttpOpen(
        NULL,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        WINHTTP_FLAG_ASYNC
        );

    if (m_hSession == NULL)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    WINHTTP_STATUS_CALLBACK result = WinHttpSetStatusCallback(
        m_hSession,
        HttpRequest::Callback,
        WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS,
        NULL
        );

    if (result == WINHTTP_INVALID_STATUS_CALLBACK)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    return S_OK;
}

//************************************************************************
//  SendRequest
//  Sends an HTTP request for a specified URL resource.
//
//  szUrl: Null-terminated string that contains the URL.
//
//***********************************************************************/

HRESULT HttpRequest::SendRequest(LPCWSTR szUrl, LPCWSTR szHeaders)
{
	BOOL bResult = FALSE;
    DWORD err = 0;

    WCHAR *szHost = NULL;
    WCHAR *szPath = NULL;
	INTERNET_PORT szPort = NULL;

    err = GetHostAndPath(szUrl, &szHost, &szPath, &szPort);
    if (err != 0)
    {
        return HRESULT_FROM_WIN32(err);
    }
	// DbgLog((LOG_ERROR, 0, TEXT("URL: %s Host: %s Path: %s Port: %d"), szUrl, szHost, szPath, szPort));

	m_hConnect = WinHttpConnect(
		m_hSession,
		szHost,
		szPort,
		0
		);

	if (m_hConnect == NULL)
	{
		goto done;
	}

	m_hRequest = WinHttpOpenRequest(
		m_hConnect,
		L"GET",
		szPath,
		NULL,
		WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES,
		0
		);

	if (m_hRequest == NULL)
	{
		goto done;
	}

	if (szHeaders == NULL) {
		szHeaders = WINHTTP_NO_ADDITIONAL_HEADERS;
	}
	bResult = WinHttpSendRequest(
		m_hRequest,
		szHeaders,
		0,
		WINHTTP_NO_REQUEST_DATA,
		0,
		0,
		(DWORD_PTR)this // Context
		);
done:
    delete [] szHost;
    delete [] szPath;

    if (!bResult)
	{
		return HRESULT_FROM_WIN32(GetLastError());
	}
	return S_OK;
}


//************************************************************************
//  OnStatus
//  Callback for WinHttp functions
//***********************************************************************/

void HttpRequest::OnStatus(
    HINTERNET hInternet,
    DWORD dwInternetStatus,
    LPVOID lpvStatusInformation,
    DWORD dwStatusInformationLength
    )
{
    BOOL bResult = TRUE;
    DWORD err = 0;
    DWORD cbDataAvailable = 0;

    WINHTTP_ASYNC_RESULT *pAsyncResult = NULL;

    switch (dwInternetStatus)
    {
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        err = OnSendRequestComplete();
        break;

    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        err = OnHeadersAvailable();
        break;

    case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
        cbDataAvailable = *(static_cast<DWORD*>(lpvStatusInformation));
        err = OnDataAvailable(cbDataAvailable);
        break;

    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        err = OnReadComplete();
        break;

    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
        DbgLog((LOG_ERROR, 0, TEXT("WINHTTP_CALLBACK_STATUS_REQUEST_ERROR")));
        pAsyncResult = static_cast<WINHTTP_ASYNC_RESULT*>(lpvStatusInformation);
        err = pAsyncResult->dwError;
        break;
    }

    if (err != 0)
    {
        if (m_pCB)
        {
            m_pCB->OnError(err);
        }
    }
}

//************************************************************************
//  OnSendRequestComplete
//  Called when the HTTP request has been sent.
//************************************************************************

DWORD HttpRequest::OnSendRequestComplete()
{
    DbgLog((LOG_TRACE, 1, TEXT("WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE")));

    if (0 == WinHttpReceiveResponse(m_hRequest, NULL))
    {
        return GetLastError();
    }
    return 0;
}

//************************************************************************
//  OnHeadersAvailable
//  Called the HTTP headers are available.
//************************************************************************

DWORD HttpRequest::OnHeadersAvailable()
{
    DbgLog((LOG_TRACE, 1, TEXT("WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE")));

    DWORD err = 0;

    // Try to get the total file size from the HTTP headers.
    err = QueryFileSize();

    if (err != 0)
    {
        return err;
    }

    // Ask for data. 
    if (0 == WinHttpQueryDataAvailable(m_hRequest, NULL))
    {
        err = GetLastError();
    }

    return err;
}


//************************************************************************
//  OnDataAvailable
//  Called when there is data ready.
//************************************************************************

DWORD HttpRequest::OnDataAvailable(DWORD cbDataAvailable)
{
    if (cbDataAvailable > 0)
    {
        // Try to read the available data.

        m_pBuffer = new BYTE[cbDataAvailable];
        if (m_pBuffer == NULL)
        {
            return ERROR_NOT_ENOUGH_MEMORY;
        }

        m_cbData = cbDataAvailable;

        if (0 == WinHttpReadData(
            m_hRequest,
            m_pBuffer,
            cbDataAvailable,
            NULL
            ))
        {
            // Error, dispose of the buffer.

            delete [] m_pBuffer;
            return GetLastError();
        }
    }
    else
    {
        // No more data available.
        m_pCB->OnEndOfStream();
    }
    return 0;
}

//************************************************************************
//  OnReadComplete
//  Called when there is data in the buffer.
//************************************************************************

DWORD HttpRequest::OnReadComplete()
{
    m_pCB->OnData(m_pBuffer, m_cbData);

    delete [] m_pBuffer;

    m_pBuffer = NULL;

    if (0 == WinHttpQueryDataAvailable(m_hRequest, NULL))
    {
        return GetLastError();
    }

    return 0;
}

//************************************************************************
//  QueryFileSize
//  Attempts to get the total file size from the HTTP headers.
//************************************************************************

DWORD HttpRequest::QueryFileSize()
{
    BOOL bResult = FALSE;
    DWORD err = 0;
    DWORD cbHeader = 0;
    WCHAR *szHeader = NULL;

    // First ask for the size of the header.

    bResult = WinHttpQueryHeaders(
        m_hRequest,
        WINHTTP_QUERY_CONTENT_LENGTH,
        WINHTTP_HEADER_NAME_BY_INDEX,
        WINHTTP_NO_OUTPUT_BUFFER,
        &cbHeader,
        WINHTTP_NO_HEADER_INDEX 
        );

    err = GetLastError();

    if (err == ERROR_INSUFFICIENT_BUFFER)
    {
        // This error code is expected when WinHttpQueryHeaders
        // is called with the WINHTTP_NO_OUTPUT_BUFFER flag.
        err = 0; 
    }
    else if (err == ERROR_WINHTTP_HEADER_NOT_FOUND)
    {
        // The Content Length header is not present. Not an error.
        return 0;
    }
    else if (err != ERROR_SUCCESS)
    {
        return err;
    }

    szHeader = new WCHAR[cbHeader];
    if (szHeader == NULL)
    {
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    bResult = WinHttpQueryHeaders(
        m_hRequest,
        WINHTTP_QUERY_CONTENT_LENGTH,
        WINHTTP_HEADER_NAME_BY_INDEX,
        szHeader,
        &cbHeader,
        WINHTTP_NO_HEADER_INDEX 
        );

    if (bResult)
    {
        int cbFile = _wtoi(szHeader);

        if (cbFile > 0 && errno != ERANGE)
        {
            m_cbFileSize = cbFile;
        }
    }
    else
    {
        err = GetLastError();
    }


    delete [] szHeader;

    return err;
}


//************************************************************************
//  GetHostAndPath
//  Get the host name and path from the URL.
//
//  Note: For this sample, we do not get any of the other fields from the
//  URL_COMPONENTS structure (such as username, password, extra info).
//************************************************************************

DWORD HttpRequest::GetHostAndPath(
    LPCWSTR szUrl, 
    WCHAR **pszHost, 
    WCHAR **pszPath,
	INTERNET_PORT *pszPort
    )
{
    BOOL bResult = FALSE;
    DWORD err = 0;
	DWORD dwUrlLen = 0;

    WCHAR *szHost = NULL;
    WCHAR *szPath = NULL;

    URL_COMPONENTS url = { 0 } ;

    // DbgLog((LOG_ERROR, 3, TEXT("Try to open URL %s"), szUrl));

    url.dwStructSize = sizeof(url);
    url.dwHostNameLength = -1;
    url.dwUrlPathLength = -1;
	url.nPort = -1;
    
    bResult = WinHttpCrackUrl(szUrl, 0, 0, &url); 
    if (!bResult)
    {
        return GetLastError();
    }

    // WinHttpCrackUrl returns the length of the each string
    // in characters NOT including the terminating NULL.
    // We need to allocate buffers that are one WCHAR larger.

    url.dwHostNameLength += 1;  
    url.dwUrlPathLength += 1;

    szHost = new WCHAR[ url.dwHostNameLength ];
    if (szHost == NULL)
    {
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    szPath = new WCHAR [ url.dwUrlPathLength ];
    if (szPath == NULL)
    {
        delete [] szHost;
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    url.lpszHostName = szHost;
    url.lpszUrlPath = szPath;

    // On this call, the string lengths that we pass in
    // are the buffer sizes, INCLUDING room for the 
    // terminating NULL.

    bResult = WinHttpCrackUrl(szUrl, 0, 0, &url);
    if (bResult)
    {
		*pszHost = szHost;
		*pszPath = szPath;
		*pszPort = url.nPort;
    }
    else
    {
        err = GetLastError();
        delete [] szHost;
        delete [] szPath;
    }
    return err;
}


