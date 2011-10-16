#ifdef _DEBUG
 // this needs to be defined last otherwise - compilation isn't possible
 #define new new(_NORMAL_BLOCK, __FILE__, __LINE__)
#endif