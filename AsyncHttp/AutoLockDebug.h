
#include <iostream>
#include <winsock2.h>

#include <stdio.h>
#include <streams.h>
#include <sstream>
#include <fstream>
#include <crtdbg.h>
#include <atlconv.h>
#include <math.h>
#include <time.h>

#include <tchar.h>
#include <cstdio>

extern void Log(const char *fmt, ...);


class CAutoLockDebug {

  // make copy constructor and assignment operator inaccessible

  CAutoLockDebug(const CAutoLockDebug &refAutoLock);
  CAutoLockDebug &operator=(const CAutoLockDebug &refAutoLock);

protected:
  CCritSec *m_pLock;
  LPCTSTR m_sModule;
  LPCTSTR m_sFunc;
  int m_iLine;

public:
  CAutoLockDebug(CCritSec * plock, int iLine, LPCTSTR szModule, LPCTSTR szFunc)
  {
    m_pLock = plock;
    m_sModule = szModule;
    m_sFunc = szFunc;
    m_iLine = iLine;
    Log("LockDebug: lock WAIT %x - line %i in %s / %s", plock, iLine, szModule, szFunc);
    m_pLock->Lock();
    Log("LockDebug: lock SUCC %x - line %i in %s / %s", plock, iLine, szModule, szFunc);
  };

  ~CAutoLockDebug() {
    m_pLock->Unlock();
    Log("LockDebug: unlock %x - line %i in %s / %s", m_pLock, m_iLine,m_sModule,m_sFunc);
  };

};


