#ifdef _DEBUG
// #define AUTOLOCK_DEBUG
// #define MANLOCK_DEBUG
// this needs to be done very early in all files to make memory debugging possible
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

#pragma warning(disable:4995)
#pragma warning(disable:4996)

#define STRSAFE_USE_SECURE_CRT 1
#define WIN32_LEAN_AND_MEAN
#define _ATL_CSTRING_EXPLICIT_CONSTRUCTORS

#ifndef VC_EXTRALEAN
#define VC_EXTRALEAN
#endif

// #define CHUNK_SIZE 65536
#define CHUNK_SIZE 32768

// 500MB
#define BIGFILE 524288000
// 100MB
#define LOWDISKSPACE 104857600
//#define LOWDISKSPACE 71887224832
// 10MB
#define SKIP_BEGINNING 10485760

#define SAFE_DELETE(p)       { if(p) { delete (p);     (p)=NULL; } }
#define SAFE_DELETE_ARRAY(p) { if(p) { delete[] (p);   (p)=NULL; } }
#define SAFE_RELEASE(p)      { if(p) { (p)->Release(); (p)=NULL; } }

#define SAFE_DELETE_WAVEFORMATEX(p) { if(p) { delete[] (BYTE *)(p);   (p)=NULL; } }
