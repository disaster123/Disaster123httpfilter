//////////////////////////////////////////////////////////////////////////
// HttpStream.h
// 
// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved.
//
//////////////////////////////////////////////////////////////////////////

#pragma once

#include <atlbase.h>
#include <shlobj.h>
#include <queue>

using namespace std;

//************************************************************************
//  CHttpStream
//  Implements the CAsyncStream interface for HTTP progressive download.
//
//  This class uses WinHttp to make asynchronous HTTP requests and 
//  saves the results into a temp file. The BeginRead/EndRead methods
//  read from the temp file. The read request will block until the 
//  requested data has finished downloading. Therefore, if the caller
//  attempts to read data from the end of the file, it will block until
//  the entire file is downloaded (or the download is canceled).
//
//***********************************************************************/


class CHttpStream : public CAsyncStream //, public HttpRequestCB
{
public:

	CHttpStream() : 
      m_pEventSink(NULL)
	{
	}

	~CHttpStream();

	HRESULT Initialize(LPCTSTR lpszFileName);
    HRESULT add_to_downloadqueue(LONGLONG startpos);
    void WaitForSize(LONGLONG start, LONGLONG end);

    HRESULT SetEventSink(IMediaEventSink *pSink)
    {
        m_pEventSink = pSink; // Do not add ref;
        return S_OK;
    }

	// Implementation of CAsyncStream methods

    HRESULT StartRead(
		PBYTE pbBuffer,
		DWORD dwBytesToRead,
		BOOL bAlign,
		LPOVERLAPPED pOverlapped,
		LPBOOL pbPending,
		LPDWORD pdwBytesRead
		);

	HRESULT EndRead(
        LPOVERLAPPED pOverlapped, 
        LPDWORD pdwBytesRead
        );

	HRESULT Cancel();

    HRESULT Length(LONGLONG *pTotal, LONGLONG *pAvailable);
	DWORD Alignment() { return 1; }

	void Lock();
	void Unlock();

    void OnEndOfStream();
	void OnError(DWORD dwErr); 

	HRESULT CHttpStream::Downloader_Start(TCHAR* szUrl, LONGLONG startpoint);
	HRESULT CHttpStream::ServerPreCheck(const char* url);

private:

	TCHAR       *m_FileName;

    IMediaEventSink *m_pEventSink;
};


