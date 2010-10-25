
#pragma warning(disable:4995)
#pragma warning(disable:4996)

#define WIN32_LEAN_AND_MEAN
#define _ATL_CSTRING_EXPLICIT_CONSTRUCTORS

#ifndef VC_EXTRALEAN
#define VC_EXTRALEAN
#endif

#define SAFE_DELETE(p)       { if(p) { delete (p);     (p)=NULL; } }
#define SAFE_DELETE_ARRAY(p) { if(p) { delete[] (p);   (p)=NULL; } }
#define SAFE_RELEASE(p)      { if(p) { (p)->Release(); (p)=NULL; } }

#define SAFE_DELETE_WAVEFORMATEX(p) { if(p) { delete[] (BYTE *)(p);   (p)=NULL; } }

// ONCE_SHARE
#ifndef ONCE_SHARE
#define ONCE_SHARE

#ifdef _DEBUG
#include <crtdbg.h>
//#define AUTOLOCK_DEBUG
//#define MANLOCK_DEBUG
#define _CRTDBG_MAP_ALLOC
#endif
#endif
