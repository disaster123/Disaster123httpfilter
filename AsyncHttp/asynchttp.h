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

extern void Log(const char *fmt, ...);

// {A713A1D8-BA12-4bb8-A05A-C3931C685E82}
DEFINE_GUID(CLSID_AsyncHttp, 
0xA713A1D8, 0xBA12, 0x4bb8, 0xa0, 0x5a, 0xc3, 0x93, 0x1c, 0x68, 0x5e, 0x82);


//************************************************************************
//  CAsyncFilterHttp
//
//  Async file filter. Most of the functionality is provided by the
//  CAsyncReader class in conjunction with the CHttpStream class.
//************************************************************************

class CAsyncFilterHttp : public CAsyncReader, public IFileSourceFilter, public IAMOpenProgress
{
public:
    CAsyncFilterHttp(LPUNKNOWN pUnk, HRESULT *phr) :
        CAsyncReader(NAME("CSSR SPriebe HTTP Filter"), pUnk, CLSID_AsyncHttp, &m_FileStream, phr),
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
        } else if (riid == IID_IAMOpenProgress) {
            return GetInterface((IAMOpenProgress *)this, ppv);
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
			TCHAR *szExtension = PathFindExtension(OLE2T(lpwszFileName));

            if ( (szExtension && _tcscmp(szExtension, TEXT(".avi")) == 0) ||
                 (_tcsstr(szExtension, ".avi?") > 0) ||
                 (_tcsstr(szExtension, ".avi&&&&") > 0) )
			{
				subtype = MEDIASUBTYPE_Avi;
                Log("subtype MEDIASUBTYPE_Avi / avi");
			}
			else if ( (szExtension && _tcscmp(szExtension, TEXT(".divx")) == 0) ||
                 (_tcsstr(szExtension, ".divx?") > 0) ||
                 (_tcsstr(szExtension, ".divx&&&&") > 0) )
			{
				subtype = MEDIASUBTYPE_Avi;
                Log("subtype MEDIASUBTYPE_Avi / divx");
			}
			else if ( (szExtension && _tcscmp(szExtension, TEXT(".mkv")) == 0) ||
                 (_tcsstr(szExtension, ".mkv?") > 0) ||
                 (_tcsstr(szExtension, ".mkv&&&&") > 0) )
			{
				subtype = MEDIASUBTYPE_H264;
                Log("subtype MEDIASUBTYPE_H264 / mkv");
			}
			else
			{
				Log("subtype MEDIASUBTYPE_NULL / Wildcard");
			}

			cmt.SetType(&MEDIATYPE_Stream);
            cmt.SetSubtype(&subtype);
        }
        else 
		{
  		    HRESULT hr = CopyMediaType(&cmt, pmt);
   			if (FAILED(hr))
			{
				return hr;
			}
        }

		HRESULT hr = m_FileStream.Initialize(OLE2T(lpwszFileName));
		if (FAILED(hr))
		{
			Log("asynchttp: Initialisation failed! Cannot load URL!");
			return hr;
		}

        m_pFileName = new WCHAR[cch];

        if (m_pFileName!=NULL)
			CopyMemory(m_pFileName, lpwszFileName, cch*sizeof(WCHAR));

		// this is how MS async filter does it
		cmt.bFixedSizeSamples = TRUE;
		cmt.bTemporalCompression = FALSE;
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

	STDMETHODIMP QueryProgress(LONGLONG* pllTotal, LONGLONG* pllCurrent)
    {
      LONGLONG llLength = 0, llAvailable = 0;
      *pllTotal = llLength;
      *pllCurrent = llAvailable;

      if (!m_pFileName) {
          return S_OK;
      }

      HRESULT hr = m_FileStream.Length(&llLength, &llAvailable, TRUE);
      // Log("QueryProgress m_FileStream.Length done!!! %I64d %I64d", llLength, llAvailable);
      
      if (SUCCEEDED(hr)) {
          *pllTotal = llLength;
          *pllCurrent = llAvailable;
          hr = S_OK;
      }
        else
      {
          hr = E_FAIL;
      }

	  return hr;
    }

    STDMETHODIMP AbortOperation()
    {
	   //Log("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA AbortOperation called");
       return m_FileStream.Cancel();
	   //return E_NOTIMPL;
    }

private:
    LPWSTR      m_pFileName;
    LONGLONG    m_llSize;
    PBYTE       m_pbData;
	CHttpStream m_FileStream;
};
