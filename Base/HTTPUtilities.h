
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

int initWSA()
{
  WSADATA w;
  if (int result = WSAStartup(MAKEWORD(2,2), &w) != 0)
  {
     Log("initWSA: Winsock 2 konnte nicht gestartet werden! Error %d", result);
     return 1;
  } 
 
  return 0;
}

void DownloaderThread_geturlpos(char **url, LONGLONG *startpos, string down_queue_entry)
{
  // this isn't perfect but the URL can't be bigger than the whole line :-)
  *url = new char[strlen(down_queue_entry.c_str())];

  sscanf(down_queue_entry.c_str(), "%s || %I64d", *url, &*startpos);
}

int Initialize_connection(char* szHost, int szPort)
{
	   int Socket = -1;

	   Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	   if (Socket == -1) {
		   Log("Initialize_connection: Socket konnte nicht erstellt werden! %d", Socket);
		   return -1;
       }   
       sockaddr_in service; // Normale IPv4 Struktur
       service.sin_family = AF_INET; // AF_INET für IPv4, für IPv6 wäre es AF_INET6
	   service.sin_port = htons(szPort); // Das HTTP-Protokoll benutzt Port 80
	   // szHost to IP
	   hostent* phe = gethostbyname(szHost);
	   if(phe == NULL) {
          Log("Initialize_connection: Hostname %s konnte nicht aufgelöst werden.", szHost);
		  return -1;
       }
	   if(phe->h_addrtype != AF_INET) {
          Log("Initialize_connection: Keine IPv4 Adresse gefunden!");
		  return -1;
       }
       if(phe->h_length != 4) {
          Log("Initialize_connection: Keine IPv4 Adresse gefunden!");
		  return -1;
       }
	   char *szIP = inet_ntoa(*reinterpret_cast<in_addr*>(*phe->h_addr_list));
	   Log("Initialize_connection: Host: %s mit IP: %s", szHost, szIP);
       service.sin_addr.s_addr = inet_addr(szIP);

	   int result = connect(Socket, reinterpret_cast<sockaddr*>(&service), sizeof(service));
	   if (result == -1) {
		  closesocket(Socket);
          Log("Initialize_connection: Connect fehlgeschlagen!");
		  return -1;
       }

   return Socket;
}

char* buildrequeststring(char* szHost, int szPort, char* szPath, LONGLONG startpos)
{
	// this is bad also cause we need another allocation at the buttom but how to determine the size?
	char request[2000];

	//int len = _snprintf(NULL, 999999, "GET /%s HTTP/1.1\r\nHost: %s:%d\r\nRange: Bytes=%I64d-\r\nConnection: close\r\n\r\n", szPath, szHost, szPort, startpos);
	//request = (char*) malloc (sizeof(char) * (len + 1));
	
    sprintf(request, "GET /%s HTTP/1.1\r\nHost: %s:%d\r\nRange: Bytes=%I64d-\r\nUser-Agent: Mozilla/5.0 (Disaster123 MP HTTP Filter)\r\nConnection: close\r\n\r\n", szPath, szHost, szPort, startpos);
 
	string request_logline = request;
    stringreplace(request_logline, "\r", "");
    stringreplace(request_logline, "\n\n", "");
    Log("Sending Request: %s", request_logline.c_str());
	
    char* rstr = (char*) malloc (sizeof(char) * (strlen(request) + 1));
	strcpy(rstr, request);

	return rstr;
}

void GetLineFromSocket(int socket, string& line) {
	line.clear();
    char c;
    while (recv(socket, &c, 1, 0) > 0)
    {
        if(c == '\r') {
            continue;
        }
        if(c == '\n')
        {
            return;
        }
        line += c;
    }
    throw CreateSocketError(); 
}

void GetHTTPHeaders(int Socket, LONGLONG* filesize, int* statuscode, string& headers)
{
       // Read Header and ignore
	   string HeaderLine;
	   LONGLONG tmp1,tmp2;
	   LONGLONG contlength = -1;
	   LONGLONG contrange = -1;
	   contlength=0;
       contrange=0;
       *statuscode = 999;
	   // get headerlines - max of 25
	   for (int loop = 0; loop < 25; loop++) {
		   try {
     	       GetLineFromSocket(Socket, HeaderLine);
               headers += HeaderLine;
               headers += "\n";

               if (loop == 0) {
                   // this MUST contain the statuscode
                   // HTTP/1.1 206 Partial Content
                   Log("GetHeaderHTTPHeaderData: Statusline: %s", HeaderLine.c_str());
                   if (sscanf(HeaderLine.c_str(), "HTTP/1.1 %d ", &*statuscode) != 1) {
                     Log("GetHeaderHTTPHeaderData: Statusline was unknown");
                     break;
                   }
               }
			   // Empty header (\n\n) finish the loop
               if (HeaderLine.length() == 0) {
				   *filesize = max(contrange, contlength);
				   break;
			   }

               Log("GetHeaderHTTPHeaderData: Header: %s", HeaderLine.c_str());

               // Content-Range: bytes 1555775744-1555808025/1555808026
			   sscanf(HeaderLine.c_str(), "Content-Length: %I64d", &contlength);
			   sscanf(HeaderLine.c_str(), "Content-Range: bytes %I64d-%I64d/%I64d", &tmp1, &tmp2, &contrange);
		   } catch(exception& ex) {
			   Log("GetHeaderHTTPHeaderData: Cannot get Headerlines: %s", ex);
		   }
	   }
	   // Loop was running too long but assign values
	   *filesize = max(contrange, contlength);
}

void send_to_socket(int socket, const char* const buf, const int size)
{
    int bytesSent = 0; // Anzahl Bytes die wir bereits vom Buffer gesendet haben
    do
    {
        int result = send(socket, buf + bytesSent, size - bytesSent, 0);
        if(result < 0) // Wenn send einen Wert < 0 zurück gibt deutet dies auf einen Fehler hin.
        {
            throw CreateSocketError();
        }
        bytesSent += result;
    } while(bytesSent < size);
}

HRESULT GetValueFromHeader(const char* head, const char* key, string& value)
{
    string::size_type pos = 0;
    string::size_type opos = 0;
    char* value_c = NULL;
    string headers = head;
    headers += "\n";
    stringreplace(headers, "\r", "");
    string searchkey = key;
    searchkey += ": %s";
    value = "";

    while ( (pos = headers.find("\n", opos)) != string::npos ) {
         string line = headers.substr(opos, pos-opos);
         SAFE_DELETE_ARRAY(value_c);
         value_c = (char*) malloc (sizeof(char) * strlen(line.c_str()) + 1);
         // Log("GetValueFromHeader: %s", line.c_str());
         if (sscanf(line.c_str(), searchkey.c_str(), value_c) == 1) {
             value = value_c;
             // Log("Found key: %s value: %s", searchkey.c_str(), value.c_str());
             break;
         }
         opos = pos+1;
    }
    SAFE_DELETE_ARRAY(value_c);

    if (value.length() == 0) {
        return E_FAIL;
    }

    return S_OK;
}

string GetLocationFromHeader(string headers)
{
   string ret;

   GetValueFromHeader(headers.c_str(), "Location", ret);

   //Log("GetLocationFromHeader: got new Location: %s", ret.c_str());

  return ret;
}