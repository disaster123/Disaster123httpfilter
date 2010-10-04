/*************************************************************************
*  HttpRequest.h: HTTP request helper class.
*  
*  THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
*  ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
*  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
*  PARTICULAR PURPOSE.
* 
*  Copyright (c) Microsoft Corporation. All rights reserved.
* 
*************************************************************************/

#pragma once

#include <windows.h>
#include <Winhttp.h>

//************************************************************************
//  HttpRequestCB
//  Callback interface for the HttpRequest class.
//
//  The user of HttpRequest must implement this callback interface, and
//  set the callback by calling HttpRequest::InitializeSession().
//***********************************************************************/

struct HttpRequestCB
{
	virtual void OnData(BYTE *pData, DWORD cbData) = 0;
	virtual void OnEndOfStream() = 0;
	virtual void OnError(DWORD dwErr) = 0;
};

//************************************************************************
//  HttpRequest
//  Helper class for making HTTP requests.
//
//  Note: This helper is supports a very restricted set of HTTP features. 
//        See the InitializeSession and SendRequest methods for details.
//
//  WinHttp functions are performed in asynchronous mode, and the results 
//  are conveyed to the caller through the callback interface.
//***********************************************************************/

class HttpRequest
{
public:

	HttpRequest() : 
		m_pCB(NULL), 
		m_hSession(NULL),
		m_hConnect(NULL),
		m_hRequest(NULL),
        m_pBuffer(NULL),
        m_cbData(0),
        m_cbFileSize(0)
	{
	}

	~HttpRequest();

    HRESULT InitializeSession(HttpRequestCB *pCB);
	HRESULT SendRequest(LPCWSTR szUrl, LPCWSTR szHeaders);

	HRESULT End();

    DWORD   FileSize() const
    {
        return m_cbFileSize;
    }

private:

    // Per-instance callback for WinHttp functions
    void OnStatus(
        HINTERNET hInternet,
        DWORD dwInternetStatus,
        LPVOID lpvStatusInformation,
        DWORD dwStatusInformationLength
        );

    // Callback handlers
    DWORD OnSendRequestComplete();
    DWORD OnHeadersAvailable();
    DWORD OnDataAvailable(DWORD cbDataAvailable);
    DWORD OnReadComplete();

    DWORD QueryFileSize();
    DWORD GetHostAndPath(LPCWSTR szUrl, WCHAR **pszHost, WCHAR **pszPath, INTERNET_PORT *pszPort);

private:

	HttpRequestCB *m_pCB;       // Callback pointer

	HINTERNET	m_hSession;
	HINTERNET	m_hConnect;
	HINTERNET	m_hRequest;

	BYTE		*m_pBuffer;     // Temporary data buffer.
	DWORD       m_cbData;       // Size of m_pBuffer.
    DWORD       m_cbFileSize;   // Total file size.

public:

    // Callback for WinHttp functions. Calls through to OnStatus method.
    static void CALLBACK Callback(
	  HINTERNET hInternet,
	  DWORD_PTR dwContext,
	  DWORD dwInternetStatus,
	  LPVOID lpvStatusInformation,
	  DWORD dwStatusInformationLength
	)
	{
		HttpRequest *pThis = (HttpRequest*)dwContext;

		if (pThis)
		{
			pThis->OnStatus(
				hInternet, 
				dwInternetStatus, 
				lpvStatusInformation, 
				dwStatusInformationLength
				);
		}
	}

};



