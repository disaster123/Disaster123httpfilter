
#define ENABLE_ALLOC_BOOKKEEPING

#ifdef ENABLE_ALLOC_BOOKKEEPING
#ifdef _DEBUG
#define _CRTDBG_MAP_ALLOC

#include <stdlib.h>
#include <crtdbg.h>

#define DEBUG_NEW_OWN new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW_OWN
#endif
#endif