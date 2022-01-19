#include <sys/socket.h>
#include <sys/stat.h>
#include <stdio.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <fcntl.h>
#include <unistd.h>       // write and pwrite
#include <string.h>       // memset
#include <stdlib.h>       // atoi
#include <stdbool.h>      // true, false
#include <errno.h>
#include <pthread.h>        // pthread functions and data structures
#define BUFFER_SIZE 4096    // size of buffer for communication
#define LOG_LINE_LEN 69     // maximal length of a line in log file (=8+20*3+1)
#define NUM_LOG_LINES 100   // max number of log lines collected before pwriting
#define SIZE_LOG_LINES 8192 // size of log_lines buffer
#define MAX_RESPONSE_LENGTH 100 // maximal length of a response message
#define FILENAME_SIZE 27  // maximal length of file name
#define HEALTHCHECK "healthcheck"  // reserved filename for "HEALTH" command
#define HTTPVER "HTTP/1.1"
// int codes for the types of requests
#define HEAD 0
#define GET 1
#define PUT 2
  #define HEALTH 3
const char* request[] = {"HEAD", "GET", "PUT", "GET"};
const char* end_log = "\n========\n";

bool verbose = false;       // flag if to print info on run
// pthread mutex for access to stdout for verbose output
pthread_mutex_t v_mutex = PTHREAD_MUTEX_INITIALIZER;

int log_fd = -1;            // file descriptor of log-file or -1 if not logging

// pthread mutex for access to logging variables:
// log_offset, num_logged_entries and num_err_recorded
pthread_mutex_t l_mutex = PTHREAD_MUTEX_INITIALIZER;
int num_logged_entries = 0; // number of logged entries
int num_err_recorded = 0;   // number of recorded errors
long log_offset = 0;        // offset to use when writing to log-file, based on
                            // its current content

int rc;                     // return code from calls to pthread functions

// keep track of all components related to a HTTP message.
struct httpObject {
  int id;                           // id of worker handling client
  int cmd;                          // PUT, HEAD, GET, HEALTH
  char filename[FILENAME_SIZE + 1]; // what is the file we are worried about
  ssize_t content_length;           // example: 13
  uint8_t buffer[BUFFER_SIZE + 1];  // buffer passed between server & client
  char log_lines[SIZE_LOG_LINES + 1]; // line printed to log file
  long logged_bytes;                // number of bytes logged
  long offset;                      // offset in log file for current message
};

struct clientQ {
  int *p_client_sockd; // NULL or a pointer to a client socket
  struct clientQ* next;
};

// pthread mutex for access to the queue
pthread_mutex_t q_mutex = PTHREAD_MUTEX_INITIALIZER;
struct clientQ* first_client = NULL;
struct clientQ* last_client = NULL;
// pthread condition to alert the queue got new clients
pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER;

void enqueue(int* p_client) {
  struct clientQ* n = malloc(sizeof(struct clientQ));
  n->p_client_sockd = p_client;
  n->next = NULL;
  if (last_client == NULL) {
    first_client = n;
  } else {
    last_client->next = n;
  }
  last_client = n;
}

int* dequeue() {
  if (first_client == NULL) return NULL;
  struct clientQ* n = first_client;
  first_client = first_client->next;
  if (first_client == NULL) last_client = NULL;

  int* p_client = n->p_client_sockd;
  free(n);
  return p_client;
}

#define MIN(x, y) (((x) < (y)) ? (x) : (y))

void send_http_response(int id, ssize_t client_sockd,
                int err_code, char* err_str, int length) {
  char mymsg[MAX_RESPONSE_LENGTH];
  sprintf(mymsg, "%s %d %s\r\nContent-Length: %d\r\n\r\n",
                 HTTPVER, err_code, err_str, length);
  send(client_sockd, mymsg, strlen(mymsg), 0);
  if (verbose) {
    pthread_mutex_lock(&v_mutex);
    printf("%d\terror code: %d (%s) length %d\n", id, err_code, err_str, length);
    pthread_mutex_unlock(&v_mutex);
  }
}

// write the log_lines in the buffer and the end-line to the log file
void LogLast(struct httpObject* message) {
  if (log_fd < 0) return;
  int len = strlen(message->log_lines);
  int end_len = strlen(end_log);
  if (len + end_len > SIZE_LOG_LINES) {
    pwrite(log_fd, message->log_lines, len, message->offset);
    message->offset += len;
    strcpy(message->log_lines, "");
    len = 0;
  }
  // close with end_log line
  strcpy(message->log_lines + len, end_log);
  pwrite(log_fd, message->log_lines, len + end_len, message->offset);
  strcpy(message->log_lines, "");
  // update counters
  pthread_mutex_lock(&l_mutex);
  ++num_logged_entries;
  pthread_mutex_unlock(&l_mutex);
}

// log an error message by simply writing it onto the log_lines buffer and
// pwrite()-ing the buffer to the log file.
void log_error(int cmd, int err_code, char* filename, char* log_line) {
  if (log_fd < 0) return;
  sprintf(log_line, "FAIL: %s /%s %s --- response %d%s",
          request[cmd], filename, HTTPVER, err_code, end_log);
  // needed amount of characters
  int len = strlen(log_line);
  // get offset in file and update shared offset variable
  pthread_mutex_lock(&l_mutex);
  long offset = log_offset;
  log_offset += len;
  pthread_mutex_unlock(&l_mutex);
  // log the error
  pwrite(log_fd, log_line, len, offset);
  strcpy(log_line, "");
  // update counters
  pthread_mutex_lock(&l_mutex);
  ++num_logged_entries;
  ++num_err_recorded;
  pthread_mutex_unlock(&l_mutex);
}

// log 400 error using the buffer until the first eol
void log_400_error(char* buf, char* log_line) {
  if (log_fd < 0) return;
  char* eol = strchr(buf, '\r');
  if (eol != NULL) {
    *eol = '\0';
    sprintf(log_line, "FAIL: %s --- response 400%s", buf, end_log);
    *eol = '\r';
  } else {
    sprintf(log_line, "FAIL: %s --- response 400%s", buf, end_log);
  }
  // needed amount of characters
  int len = strlen(log_line);
  // get offset in file and update shared offset variable
  pthread_mutex_lock(&l_mutex);
  long offset = log_offset;
  log_offset += len;
  pthread_mutex_unlock(&l_mutex);
  // log the error
  pwrite(log_fd, log_line, len, offset);
  strcpy(log_line, "");
  // update counters
  pthread_mutex_lock(&l_mutex);
  ++num_logged_entries;
  ++num_err_recorded;
  pthread_mutex_unlock(&l_mutex);
}

// initialize log-lines buffer content and get offset
void LogRequestUpdateOffset(struct httpObject * message) {
  if (log_fd < 0) return;
  //example:
  //GET /abcdefghij0123456789abcdefg length 32\n
  //00000000 48 65 6c 6c 6f 3b 20 74 68 69 73 20 69 73 20 61 20 73 6d 61\n
  //00000020 6c 6c 20 74 65 73 74 20 66 69 6c 65\n
  //========\n
  sprintf(message->log_lines, "%s /%s length %ld",
          request[message->cmd], message->filename, message->content_length);
  message->logged_bytes = 0; // no hex bytes yet
  long total = strlen(message->log_lines);
  if (message->cmd != HEAD) {
    int n = message->content_length;
    int l = n / 20;
    if (n % 20 > 0) ++l;
    total += n * 3 + l * 9;
  }
  total += strlen(end_log);

  pthread_mutex_lock(&l_mutex);
  message->offset = log_offset;
  log_offset += total;
  pthread_mutex_unlock(&l_mutex);
}

// create hex log lines n chunks of 20 using buffer of bytes
void LogHexBytes(struct httpObject * message, int bytes) {
  if (log_fd < 0) return;
  int i = 0;
  // pointer to first unused char in log lines buffer
  char* p_lines = message->log_lines + strlen(message->log_lines);
  int bytes_in_line = message->logged_bytes % 20;
  if (bytes_in_line > 0) { // fill the incomplete line
    for ( ; i < bytes && bytes_in_line + i < 20; ++i, p_lines += 3) {
      sprintf(p_lines, " %02x", message->buffer[i]);
      ++message->logged_bytes;
    }
    if (bytes_in_line + i < 20) return;
  }
  // at the end of a hex-line
  int len = strlen(message->log_lines);
  if (len + LOG_LINE_LEN + 1 > SIZE_LOG_LINES) { // dump
    pwrite(log_fd, message->log_lines, len, message->offset);
    message->offset += len;
    strcpy(message->log_lines, "");
    p_lines = message->log_lines;
  }
  for (/* i = however many bytes we used above */ ; i < bytes; i += 20) {
    sprintf(p_lines, "\n%08ld", message->logged_bytes); p_lines += 9;
    int j;
    for (j = 0; j < 20 && i + j < bytes; ++j) {
      sprintf(p_lines, " %02x", message->buffer[i + j]); p_lines += 3;
      ++message->logged_bytes;
    }
    if (j < 20) return;
    len = strlen(message->log_lines);
    if (len + LOG_LINE_LEN + 1 > SIZE_LOG_LINES) { // dump
      pwrite(log_fd, message->log_lines, len, message->offset);
      message->offset += len;
      strcpy(message->log_lines, "");
      p_lines = message->log_lines;
    }
  }
}

// Checks format of arrived message
bool parseMessage(char* msgBuffer, char* filename,
                  int* cmd, long* content_length) {
    *content_length = -1;
    char* first = msgBuffer;
    // command header
    char* p = strchr(first, ' ');
    if (p == NULL) return false;
    *cmd = -1;
    if (strncmp(first, "HEAD", p - first) == 0) *cmd = HEAD;
    if (strncmp(first, "GET", p - first) == 0) *cmd = GET;
    if (strncmp(first, "PUT", p - first) == 0) *cmd = PUT;
    if (*cmd < 0) return false;
    first = p + 1;
    if (*first != '/') return false;
    ++first;
    p = strchr(first, ' ');
    if (p == NULL || p > first + 27) return false;
    for (char* c = first+1; c < p; ++c) {
        if (*c >= 'a' && *c <= 'z') continue;
        if (*c >= 'A' && *c <= 'Z') continue;
        if (*c >= '0' && *c <= '9') continue;
        if (*c == '-' || *c == '_') continue;
        return false;
    }
    strncpy(filename, first, p - first);
    filename[p-first] = '\0'; // null terminated
    // GET of healthcheck file is interpreted as a request to "get healthcheck"
    if (strcmp(filename, HEALTHCHECK) == 0 && *cmd == GET) *cmd = HEALTH;
    // just before the http part in the command header. handling client
    //  make sure there is \r\n after. handling client
    first = p + 1;
    p = strstr(first, "\r\n");
    if (p == NULL) return false;
    if (strncmp(first, "HTTP/1.1", p - first) != 0) return false;
    first = p + 2; // skip two characters ahead
    // loop on following key-value lines and final empty line
    bool empty_line = false;
    for (p = strstr(first, "\r\n"); p != NULL;) {
        char* n_first = p + 2;
        char* n_p = strstr(n_first, "\r\n");
        if (p == first) {
            empty_line = true;
            break;
        }
        char *q = strchr(first, ':');
        if (q == NULL || q-p >= 0) return false;
        if (*cmd == 2) { // PUT
            if (strncmp(first, "Content-Length", strlen("Content-Length")) == 0) {
                sscanf(q+1, "%ld", content_length);
            }
        }
        first = n_first;
        p = n_p;
    }
    // check that put got length
    if (*cmd == 2 && *content_length < 0) return false;
    // ends with empty line?
    return empty_line;
}

// Process messages recieved by the worker
void process_request(int client_sockd, struct httpObject* message) {
  ssize_t bytes = recv(client_sockd, message->buffer, BUFFER_SIZE, 0);
  message->buffer[bytes] = '\0'; // null terminate
  // parse header of request
  bool ok = parseMessage((char*) (message->buffer), message->filename,
                         &(message->cmd), &(message->content_length));
  if (!ok) {
    send_http_response(message->id, client_sockd, 400, "BAD REQUEST", 0);
    log_400_error(message->buffer, message->log_lines);
    return;
  }
  if (verbose) {
    pthread_mutex_lock(&v_mutex);
    printf("%d\trequest : %s %s (%ld)\n", message->id, request[message->cmd],
           message->filename, message->content_length);
    pthread_mutex_unlock(&v_mutex);
  }
  // healthcheck request
  if (message->cmd == HEALTH) {
    if (log_fd < 0) {
      send_http_response(message->id, client_sockd, 404, "NOT FOUND", 0);
      // no need, because log_fd<0
      // log_error(message->cmd, 404, message->filename, message->log_line);
      return;
    }
    // return healthcheck report
    char mymsg[MAX_RESPONSE_LENGTH];
    pthread_mutex_lock(&l_mutex);
    // no additional \n at the end???
    sprintf(mymsg, "%d\n%d", num_err_recorded, num_logged_entries);
    pthread_mutex_unlock(&l_mutex);
    send_http_response(message->id, client_sockd, 200, "OK", strlen(mymsg));
    send(client_sockd, mymsg, strlen(mymsg), 0);
    message->content_length = strlen(mymsg);
    LogRequestUpdateOffset(message);
    strcpy((char*) (message->buffer), mymsg);
    LogHexBytes(message, strlen(mymsg));
    LogLast(message);
    return;
  }
  // special reserved filename
  if (strcmp(message->filename, HEALTHCHECK) == 0) {
    send_http_response(message->id, client_sockd, 403, "FORBIDDEN", 0);
    log_error(message->cmd, 403, message->filename, message->log_lines);
    return;
  }
  // check stat of file
  struct stat stat_buffer;
  int statOK = stat(message->filename, &stat_buffer);
  if (statOK == 0) {
    if ((stat_buffer.st_mode & S_IRUSR) == 0) {
      send_http_response(message->id, client_sockd, 403, "FORBIDDEN", 0);
      log_error(message->cmd, 403, message->filename, message->log_lines);
      return;
    }
  } else if (message->cmd != PUT) { // HEAD or GET
    send_http_response(message->id, client_sockd, 404, "NOT FOUND", 0);
    log_error(message->cmd, 404, message->filename, message->log_lines);
    return;
  }
  // get file descriptor
  int fd;
  if (message->cmd == PUT) {
    fd = open(message->filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  } else { // HEAD or GET
    fd = open(message->filename, O_RDONLY);
  }
  // check file descriptor
  if (fd < 0) {
    switch (errno) {
      case EACCES: // permission denied
        send_http_response(message->id, client_sockd, 403, "FORBIDDEN", 0);
        log_error(message->cmd, 403, message->filename, message->log_lines);
        break;
      case 2:
        if (message->cmd != PUT) {
          send_http_response(message->id, client_sockd, 404, "NOT FOUND", 0);
          log_error(message->cmd, 404, message->filename, message->log_lines);
          break;
        }
        // if PUT, continue to next case
      default:
        send_http_response(message->id, client_sockd, 500, "INTERNAL SERVER ERROR", 0);
        log_error(message->cmd, 500, message->filename, message->log_lines);
        break;
    }
    return;
  }
  strcpy(message->log_lines, "");
  message->logged_bytes = 0;
  // receive or send bytes
  if (message->cmd == PUT) {
    long nBytes = message->content_length;
    LogRequestUpdateOffset(message);
    while (nBytes > 0) {
      int bytes = recv(client_sockd, message->buffer, MIN(BUFFER_SIZE, nBytes), 0);
      if (bytes == 0) { // connection close
        break;
      }
      if (bytes < 0) { // connection close
        send_http_response(message->id, client_sockd, 400, "BAD REQUEST", 0);
        log_error(message->cmd, 400, message->filename, message->log_lines);
        break;
      }
      message->buffer[bytes] = 0; // null terminate
      write(fd, message->buffer, bytes);
      LogHexBytes(message, bytes);
      nBytes -= bytes;
    }
    LogLast(message);
    if (nBytes <= 0) send_http_response(message->id, client_sockd, 201, "CREATED", 0);
    if (verbose) {
      pthread_mutex_lock(&v_mutex);
      printf("%d\tcreated %s with %ld bytes\n", message->id, message->filename, message->content_length);
      pthread_mutex_unlock(&v_mutex);
    }
  } else { // HEAD or GET
    // get the size of our file
    message->content_length = 0;
    int n = 0;
    while ((n = read(fd, message->buffer, BUFFER_SIZE))) {
      message->content_length += n;
    }
    send_http_response(message->id, client_sockd, 200, "OK", message->content_length);
    LogRequestUpdateOffset(message);
    if (message->cmd == GET) { // for HEAD we only need to log request
      // close and re-open for actual read
      close(fd);
      fd = open(message->filename, O_RDONLY);
      long nBytes = message->content_length;
      while (nBytes > 0) {
        int bytes = read(fd, message->buffer, MIN(BUFFER_SIZE, nBytes));
        message->buffer[bytes] = 0; // null terminate
        nBytes -= bytes;
        send(client_sockd, message->buffer, bytes, 0);
        LogHexBytes(message, bytes);
      }
    }
    LogLast(message);
    if (verbose) {
      pthread_mutex_lock(&v_mutex);
      printf("%d\tsent %s with %ld bytes\n", message->id, message->filename, message->content_length);
      pthread_mutex_unlock(&v_mutex);
    }
  }
  close(fd);
}

// Parse the flags in the command line and set the port, number of workers
// and the log-file, if specified.
bool ParseCommandLine(int argc, char** argv,
                      char** port, int* num_threads, char** log_file) {
  extern char *optarg;
  extern int optind;
  static char usage[] = "usage: %s [-N num_threads] [-l log_file] [port]\n";
  int c;

  // "N:l:" indicates that there are two flags and each requires a value
  // following it.
  bool n_flag = false;
  bool l_flag = false;
  while ((c = getopt (argc, argv, "N:l:")) != -1) {
    switch (c) {
      case 'N':
        if (n_flag) {
            fprintf(stderr, usage, argv[0]);
            return false;
        }
        n_flag = true;
        sscanf(optarg, "%d", num_threads);
        if (*num_threads < 1) {
          fprintf(stderr, "Error: N must be at least 1\n");
          return false;
        }
        break;
      case 'l':
        if (l_flag) {
          fprintf(stderr, usage, argv[0]);
          return false;
        }
        l_flag = true;
        *log_file = optarg;
        break;
      default:
        fprintf(stderr, usage, argv[0]);
        return false;
    }
  }

  if (optind < argc) {
    *port = argv[optind++];
    int port_num;
    sscanf(*port, "%d", &port_num);
    // TODO replace 1 with minimal port number, and 1<<16 with max+1 if exists
    if (port_num < 1 || port_num >= (1<<16)) {
      fprintf(stderr,
              "Error: port must be a number between 1 and %d\n", (1<<16));
      return false;
    }
  }
  if (optind < argc) {
    fprintf(stderr, usage, argv[0]);
    return false;
  }
  return true;
}

// thread flow:
// repeatedly wait for a request in the queue
// once it has a client, process the request similarly to asg1
void* thread_function(void* data) {
  // each worker has its own work area (message)
  struct httpObject message;
  message.id = *((int*)data);

  while (true) {
    int* p_client_sockd;
    // first try to dequeue - we might have missed the fact that the queue was
    // updated because too many clients made requests too fast.
    pthread_mutex_lock(&q_mutex);         // safely access the queue
    p_client_sockd = dequeue();
    if (p_client_sockd == NULL) {         // queue is/was empty; wait for q_cond
      // unlock until q_cond wakes us
      pthread_cond_wait(&q_cond, &q_mutex); // zzz...
      // q_mutex is locked again, so dequeue
      p_client_sockd = dequeue();
    }
    pthread_mutex_unlock(&q_mutex);       // release q_mutex
    if (p_client_sockd != NULL) {
      int client_sockd = *p_client_sockd;
      free(p_client_sockd); // free the pointer as soon as it is dequeued.

      printf("[+] %d...(%d)\n", client_sockd, message.id);
      // we have a client, handle its request
      process_request(client_sockd, &message);
    }
  }
}

void CreateServerSocket(char *port, int* p_server_sockd) {
  // define a sockaddr_in with the server information
  struct sockaddr_in server_addr;
  socklen_t addrlen = sizeof(server_addr);
  memset(&server_addr, 0, addrlen); // start with 0 as default for all fields
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(atoi(port));
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

  // Create a server socket
  *p_server_sockd = socket(AF_INET, SOCK_STREAM, 0);
  if (*p_server_sockd < 0) {
    perror("socket");
  }

  // Configure server socket
  int enable = 1;

  // This allows you to avoid: 'Bind: Address Already in Use' error
  int ret = setsockopt(*p_server_sockd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

  // Bind server address to socket that is open
  ret = bind(*p_server_sockd, (struct sockaddr *) &server_addr, addrlen);

  // Listen for incoming connections
  ret = listen(*p_server_sockd, SOMAXCONN);
  if (ret < 0) *p_server_sockd = -1;
}

// main flow:
// 1. parse command line
// 2. create a server (using the port #)
// 3. open the log-file, if specified
// 4. create a pool of workers (N, by default 4)
// 5. repeatedly wait for requests and enqueue them for threads to handle.
int main(int argc, char** argv) {
  int num_threads = 4;    // default number of workers is 4
  char* port = "8080";    // default port
  char* log_file = NULL;  // default log_file is NULL, i.e. no logging

  // Parse flags in command line.
  if (!ParseCommandLine(argc, argv, &port, &num_threads, &log_file)) return 1;
  if (verbose)
    printf("Running with port %s, N %d, l %s\n", port, num_threads, log_file);

  // Create the server_sockd with all that it entails
  int server_sockd;
  CreateServerSocket(port, &server_sockd);
  if (server_sockd < 0) {
    fprintf(stderr, "Could not create server\n");
    return EXIT_FAILURE;
  }

  // open log-file
  if (log_file != NULL) {
    log_fd = open(log_file, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (log_fd < 0) {
      perror("open log_file");
      return EXIT_FAILURE;
    }
  }

  // allocate a pool of workers (pthreads)
  int* id = malloc(num_threads * sizeof(int));
  pthread_t* p_threads = malloc(num_threads * sizeof(pthread_t));
  // create each of the request-handling threads
  for(int i = 0; i < num_threads; ++i) {
    id[i] = i;
    pthread_create(&p_threads[i], NULL, thread_function, (void*)&id[i]);
  }

  // client info
  struct sockaddr client_addr;
  socklen_t client_addrlen;

  // wait for requests
  while (true) {
    printf("[+] server is waiting...\n");
    // allocate an int for the value, so it does not get lost
    // Accept Connection
    int client_sockd = accept(server_sockd, &client_addr, &client_addrlen);
    if (client_sockd < 0) {
      fprintf(stderr, "accept failed.");
      continue;
    }
    int* p_client_sockd = malloc(sizeof(int));
    *p_client_sockd = client_sockd;
    // enqueue the client.
    pthread_mutex_lock(&q_mutex);   // wait to lock q_mutex
    enqueue(p_client_sockd);
    pthread_cond_signal(&q_cond);   // signal one of the threads to work
    pthread_mutex_unlock(&q_mutex); // release q_mutex
  }

  // should not reach this point, but... good habits
  free(p_threads);
  free(id);
  if (log_fd >= 0) close(log_fd);
  return EXIT_SUCCESS;
}
