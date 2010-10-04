/*************************************************************************
*  AsyncHttp.h: Async file filter using HTTP progressive download.
*  
*  THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
*  ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
*  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
*  PARTICULAR PURPOSE.
* 
*  Copyright (c) Microsoft Corporation. All rights reserved.
* 
*************************************************************************/

#include <tchar.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <atlconv.h>

#include "HttpStream.h"

// {78057D0C-82E1-4de7-946D-D92201228C89}
DEFINE_GUID(CLSID_AsyncHttpSample, 
0x78057d0c, 0x82e1, 0x4de7, 0x94, 0x6d, 0xd9, 0x22, 0x1, 0x22, 0x8c, 0x89);


//************************************************************************
//  CAsyncFilterHttp
//
//  Async file filter. Most of the functionality is provided by the
//  CAsyncReader class in conjunction with the CHttpStream class.
//************************************************************************

class CAsyncFilterHttp : public CAsyncReader, public IFileSourceFilter
{
public:
    CAsyncFilterHttp(LPUNKNOWN pUnk, HRESULT *phr) :
        CAsyncReader(NAME("AsyncReaderHttp"), pUnk, CLSID_AsyncHttpSample, &m_FileStream, phr),
        m_pFileName(NULL),
        m_pbData(NULL)
    {
    }

    ~CAsyncFilterHttp()
    {
        delete [] m_pbData;
        delete [] m_pFileName;
    }

    static CUnknown * WINAPI CreateInstance(LPUNKNOWN, HRESULT *);

    DECLARE_IUNKNOWN

    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void **ppv)
    {
        if (riid == IID_IFileSourceFilter) {
            return GetInterface((IFileSourceFilter *)this, ppv);
        } else {
            return CAsyncReader::NonDelegatingQueryInterface(riid, ppv);
        }
    }

    STDMETHODIMP JoinFilterGraph(
        IFilterGraph * pGraph,
        LPCWSTR pName)
    {
        CAutoLock cObjectLock(m_pLock);

        HRESULT hr = CBaseFilter::JoinFilterGraph(pGraph, pName);

        m_FileStream.SetEventSink(m_pSink);

        return hr;
    }

    /*  IFileSourceFilter methods */

    //  Load a (new) file
    STDMETHODIMP Load(LPCOLESTR lpwszFileName, const AM_MEDIA_TYPE *pmt)
    {
		USES_CONVERSION;
        CheckPointer(lpwszFileName, E_POINTER);

        // lstrlenW is one of the few Unicode functions that works on win95
        int cch = lstrlenW(lpwszFileName) + 1;

        CAutoLock lck(&m_csFilter);

        /*  Check the file type */
        CMediaType cmt;
		cmt.InitMediaType();
        if (NULL == pmt) 
		{
			GUID subtype = MEDIASUBTYPE_NULL;
			GUID formattype = FORMAT_None;

			// Workaround to support AVI files in this sample.
			TCHAR *szExtension = PathFindExtension(OLE2T(lpwszFileName));

            DbgLog((LOG_TRACE, 0, TEXT("MEDIASUBTYPE")));
			if (szExtension && _tcscmp(szExtension, TEXT(".avi")) == 0)
			{
				subtype = MEDIASUBTYPE_Avi;
                DbgLog((LOG_TRACE, 0, TEXT("subtype MEDIASUBTYPE_Avi")));
			} else if (szExtension && _tcscmp(szExtension, TEXT(".mkv")) == 0)
			{
				subtype = MEDIASUBTYPE_H264;
                DbgLog((LOG_TRACE, 0, TEXT("subtype MEDIASUBTYPE_H264 / mkv")));
			}
			else {
				DbgLog((LOG_TRACE, 0, TEXT("subtype MEDIASUBTYPE_NULL / Wildcard")));
			}

			cmt.SetType(&MEDIATYPE_Stream);
			// other filters also don't set more than this
            cmt.SetSubtype(&MEDIASUBTYPE_NULL);
			
            /*cmt.SetSubtype(&subtype);
			cmt.SetFormatType(&formattype);*/
        }
        else 
		{
			// cmt = *pmt;
  		    HRESULT hr = CopyMediaType(&cmt, pmt);
   			if (FAILED(hr))
			{
				return hr;
			}
        }

		HRESULT hr = m_FileStream.Initialize(OLE2T(lpwszFileName));
		if (FAILED(hr))
		{
			return hr;
		}

        m_pFileName = new WCHAR[cch];

        if (m_pFileName!=NULL)
			CopyMemory(m_pFileName, lpwszFileName, cch*sizeof(WCHAR));

		// this is how MS async filter does it
		// cmt.bFixedSizeSamples = TRUE;
		cmt.bTemporalCompression = TRUE;
        cmt.lSampleSize = 1;

		//m_mt = cmt;
		hr = CopyMediaType(&m_mt, &cmt);
		if (FAILED(hr))
		{
			FreeMediaType(cmt);
			return hr;
		}

		FreeMediaType(cmt);
        return S_OK;
    }

    // GetCurFile: Returns the name and media type of the current file.

    STDMETHODIMP GetCurFile(LPOLESTR * ppszFileName, AM_MEDIA_TYPE *pmt)
    {
        CheckPointer(ppszFileName, E_POINTER);
        *ppszFileName = NULL;

        if (m_pFileName!=NULL) {
        	DWORD n = sizeof(WCHAR)*(1+lstrlenW(m_pFileName));

            *ppszFileName = (LPOLESTR) CoTaskMemAlloc( n );
            if (*ppszFileName!=NULL) {
                  CopyMemory(*ppszFileName, m_pFileName, n);
            }
        }

        if (pmt!=NULL) {
            CopyMediaType(pmt, &m_mt);
        }

        return NOERROR;
    }

private:
    LPWSTR      m_pFileName;
    LONGLONG    m_llSize;
    PBYTE       m_pbData;
	CHttpStream m_FileStream;
};
