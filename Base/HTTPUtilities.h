
extern void Log(const char *fmt, ...);

void stringreplace(string& str, string search, string replace)
{
    ASSERT( search != replace );

    string::size_type pos = 0;
    while ( (pos = str.find(search, pos)) != string::npos ) {
        if (replace.length() == 0) {
            str.erase( pos, search.size() );
            pos = pos - search.length() + 1;
        } else {
            str.replace( pos, search.size(), replace );
            pos = pos - search.length() + replace.length() + 1;
        }
    }
}

double Round(double Zahl, int Stellen)
{
    double v[] = { 1, 10, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8 };  // mgl. verlängern
    return floor(Zahl * v[Stellen] + 0.5) / v[Stellen];
}

ULONGLONG GetSystemTimeInMS() {
  SYSTEMTIME systemTime;
  GetSystemTime(&systemTime);

  FILETIME fileTime;
  SystemTimeToFileTime(&systemTime, &fileTime);

  ULARGE_INTEGER uli;
  uli.LowPart = fileTime.dwLowDateTime; // could use memcpy here!
  uli.HighPart = fileTime.dwHighDateTime;

  ULONGLONG systemTimeIn_ms(uli.QuadPart/10000);

  return systemTimeIn_ms;
}

int GetHostAndPath(const char *szUrl, char **pszHost, char **pszPath, int *pszPort)
{
    char *Host;
    char *Path;
    int Port;

    Host = (char *) malloc (strlen(szUrl)+1);
    Path = (char *) malloc (strlen(szUrl)+1);

	if (sscanf(szUrl, "http://%[^:]:%d/%s", Host, &Port, Path) == 3) {
		   //Log("sscanf was 3 %s %d %s", Host, Port, Path);
		   *pszPort = Port;
	   	   *pszHost = Host;
	       *pszPath = Path;

	     return 0;
    } else if (sscanf(szUrl, "http://%[^/]/%s", Host, Path) == 2) {
		   //Log("sscanf was 2 %s %s", Host, Path);
		   *pszPort = 80;
		   *pszHost = Host;
		   *pszPath = Path;

	     return 0;
   }
  
   SAFE_DELETE(Host);
   SAFE_DELETE(Path);

   return 1;
}

std::runtime_error CreateSocketError()
{
    int error = WSAGetLastError();
    char* msg = NULL;
    if(FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                     NULL, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                     reinterpret_cast<char*>(&msg), 0, NULL))
    {
        try
        {
			Log("CreateSocketError: %s", msg);
            LocalFree(msg);
        }
        catch(...)
        {
            LocalFree(msg);
            throw;
        }
    } 
  return std::runtime_error(msg);
}
