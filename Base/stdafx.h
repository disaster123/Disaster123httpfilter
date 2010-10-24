
#pragma once
#include "SharedInclude.h"

#define WIN32_LEAN_AND_MEAN
#define _ATL_CSTRING_EXPLICIT_CONSTRUCTORS

#ifndef VC_EXTRALEAN
#define VC_EXTRALEAN
#endif

// results in a double link errror
//#include <afx.h>
//#include <afxwin.h>         // MFC core and standard components

#define SAFE_DELETE(p)       { if(p) { delete (p);     (p)=NULL; } }
#define SAFE_DELETE_ARRAY(p) { if(p) { delete[] (p);   (p)=NULL; } }
#define SAFE_RELEASE(p)      { if(p) { (p)->Release(); (p)=NULL; } }

#define SAFE_DELETE_WAVEFORMATEX(p) { if(p) { delete[] (BYTE *)(p);   (p)=NULL; } }

