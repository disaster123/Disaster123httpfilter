/*************************************************************************
*  AsyncHttp.cpp: Async file filter using HTTP progressive download.
*  
*  THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
*  ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
*  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
*  PARTICULAR PURPOSE.
* 
*  Copyright (c) Microsoft Corporation. All rights reserved.
* 
*************************************************************************/

#include "..\Base\stdafx.h"

#include <streams.h>

#include "..\Base\asyncio.h"
#include "..\Base\asyncrdr.h"

#include "asynchttp.h"

#include "..\Base\alloctracing.h"

extern const char *getVersion();

//* Create a new instance of this class
CUnknown * WINAPI CAsyncFilterHttp::CreateInstance(LPUNKNOWN pUnk, HRESULT *phr)
{
    ASSERT(phr);
#ifdef _DEBUG
	Log("(asynchttp) CAsyncFilterHttp::CreateInstance DEBUG Version: %s", getVersion());
    #ifdef _CRTDBG_MAP_ALLOC
       _CrtSetDbgFlag ( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF );
    #endif
#else
	Log("(asynchttp) CAsyncFilterHttp::CreateInstance Version: %s", getVersion());
#endif

    //  DLLEntry does the right thing with the return code and
    //  the returned value on failure

	return new CAsyncFilterHttp(pUnk, phr);
}
