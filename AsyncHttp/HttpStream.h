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

class CHttpStream : public CAsyncStream
{
public:
	CHttpStream() : 
      m_pEventSink(NULL)
	{
	}

	~CHttpStream();

	HRESULT Initialize(LPCTSTR lpszFileName, string& filetype);
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
    HRESULT Length(LONGLONG *pTotal, LONGLONG *pAvailable, BOOL realvalue);
	DWORD Alignment() { return 1; }

	void Lock();
	void Unlock();

    void OnEndOfStream();
	void OnError(DWORD dwErr); 

	HRESULT Downloader_Start(TCHAR* szUrl, LONGLONG startpoint);
	HRESULT ServerHTTPPreCheck(const char* url, string& filetype);
	HRESULT ServerRTMPPreCheck(char* url, string& filetype);

private:
	TCHAR       *m_FileName;
    IMediaEventSink *m_pEventSink;
};


