# httpserver - A multi-threaded server with basic implementation of HEAD, GET, HEALTHCHECK and PUT with logging

_httpserver_ is a multithread server that implements HEAD, GET, HEALTHCHECK and PUT with logging.
_httpserver_ replies to client requests with either one of the following:

-   error code 400 (BAD REQUEST) for an improperly formatted request.
-   error code 403 (FORBIDDEN) for a request of a file without the proper permissions.
-   error code 404 (NOT FOUND) for a missing file.
-   error code 500 (INTERNAL SERVER ERROR) for any other problems.
-   return code 200 (OK) for a successful HEAD, HEALTHCHECK or GET.
-   return code 201 (CREATED) for a successful PUT.

_httpserver_ accepts several optional parameters (in any order) through the command line:

-   Port number. The default is 8080.
-   Number of threads. The default is 4.
-   Log file name. The default is NULL.

**Usage:**
_httpserver_ [port_number] [-N <number_of_threads>] [-l <log_file>]

**examples:**
- ./httpserver 3333 -N 8 -l logfile
  This creates a server with port number 3333, with 8 threads, and a log written to a file called logfile.

- ./httpserver 4444
  This creates a server with port number 4444 and 4 threads that does not do any logging.

- ./httpserver -l another_log -N 10
  This creates a server with port number 8080 and 4 threads, and a log written to a file called another_log.

Once running, clients may connect the server and send their requests.
Connection can be established, for example, using the unix curl command.

-  get the content of file abc: curl http://localhost:8080/abc

-  write the content of xyz to a file curl -T xyz http://localhost:8080/xyz

-  get the number of characters in file f curl -I http://localhost:8080/f

## Limitations and Highlights:

1.  The server only supports the standard errors 400, 403, 404, and 500.
2.  It is flexible in the number of threads that would be used.
3.  It enables full logging of all its communication. Note that the log file will grow very fast and take space on the hard disk. You should restart the server periodically to erase the log file, or avoid logging all-together.
4.  Being multithreaded, the server can still handle client requests in a timely manner, even when some clients require a large traffic.
5.  It can crash if there are too many simultaneous clients requests (several thousands), even if each request requires minimal handling. This has to do with the dispatcher struggling with the actual client-accpeting method.
6.  The server is somewhat inneficient when running with a small number of threads and easy requests.
    The fact that it has to lock and synchronize gives it an inherrent amount of work. Thus, when only one or two easy to handle requests
    arrive, they would actually be handled faster by the _httpserver_ from assignment 1 (not multithreaded).
7.  It can handle both binary and text files.
8.  It is written in C and doesn't use standard libraries for HTTP
    or any FILE * calls.
9.  _httpserver_ is implemented in UBUNTU version 18.04 and might not function in other versions of UNIX.
