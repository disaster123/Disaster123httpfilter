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

#include <streams.h>

#include "..\Base\asyncio.h"
#include "..\Base\asyncrdr.h"

#include "asynchttp.h"


//* Create a new instance of this class
CUnknown * WINAPI CAsyncFilterHttp::CreateInstance(LPUNKNOWN pUnk, HRESULT *phr)
{
    ASSERT(phr);
    DbgLog((LOG_ERROR, 0, TEXT("(asynchttp) CAsyncFilterHttp::CreateInstance\n")));

    //  DLLEntry does the right thing with the return code and
    //  the returned value on failure

	return new CAsyncFilterHttp(pUnk, phr);
}
