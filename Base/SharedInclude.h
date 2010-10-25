#ifndef IncDSUtil_SharedInclude_h
#define IncDSUtil_SharedInclude_h

#pragma warning(disable:4995)
#pragma warning(disable:4996)

#ifdef _DEBUG
   #define _CRTDBG_MAP_ALLOC // include Microsoft memory leak detection procedures
//   #define AUTOLOCK_DEBUG
   #define MANLOCK_DEBUG

#if 0
	#include <crtdbg.h>
	#define DNew new(_NORMAL_BLOCK, __FILE__, __LINE__)
#else
	#define DNew new(__FILE__, __LINE__)
#endif

#else

#define DNew new

#endif
#endif // IncDSUtil_SharedInclude_h
