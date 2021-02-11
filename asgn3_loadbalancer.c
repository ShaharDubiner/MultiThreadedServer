// to compile: make or gcc loadbalancer.cc -o lb
// to test:
// ./loadbalancer 8080 1234 5678
// in another terminal:
// ./httpserver 1234 -l /tmp/log1234 &
// ./httpserver 5678 -l /tmp/log5678 &
// and in a third terminal:
// curl -v http://localhost:8080/abc &
#include<err.h>
#include<arpa/inet.h>
#include<netdb.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<sys/socket.h>
#include<sys/types.h>
#include <sys/time.h>
#include<unistd.h>
#include <stdbool.h>

// information per server
struct ServerInfo {
  uint16_t port;  // the server's port number
  int num_err;    // number of errors based on latest health check response
  int num_req;    // number of requests given by latest health check response
  bool up;        // indicates whetehr the server is up (true) or down (false)
  int hc_id;      // health check request id
  struct timeval req_time;  // time request was sent
};

struct ServerInfo *servers;   // array of servers with their info
int max_servers = 4;          // maximal number of servers
int max_connections = 4;      // maximal number of parallel connections
int rate_checks = 5;          // frequency of healthchecks per regular queries
int freq_checks = 2;          // frequency of healthchecks in seconds
struct timeval max_timeout;   // timeout for servers response
fd_set all_ids;               // the set of all opened socket ids
int max_id;                   // maximal socket-id seen so far
int num_connections = 0;      // number of open connections
int num_id = 3;               // number of open ids (including standard 0,1,2),
                              // used to decline accepting new clients if
                              // exceeding FD_SETSIZE
int sid;                      // socket id of the load balancer

// buffer size and strings for communication
#define BUFF_SIZE 256
const char* err500 = "HTTP/1.1 500 INTERNAL SERVER ERROR\r\nContent-Length: 0\r\n\r\n";
const char* hcheck = "GET /healthcheck HTTP/1.1\r\n\r\n";

// hash map mechanism of client-ids and socket-ids
#define HASH_SIZE 15
typedef struct hashValue {
  int key;                      // socket id of client or server
  int match_id;                 // matching socket id of server or client
  struct ServerInfo* p_server;  // pointer to maching server or NULL
                                // if match_id is of a client
  struct timeval last_communication; // last time client-server communicated
  struct hashValue* next;       // pointer to next hashValue structure
} *pHashVal;

// hashmap (array of lists of hashValues)
pHashVal Map[HASH_SIZE];

// initialize the map: just set pointers to default NULL
void clearHash() {
  for (int i = 0; i < HASH_SIZE; ++i) {
    Map[i] = NULL;
  }
}

// empty the map from any existing content
void emptyHash() {
  for (int i = 0; i < HASH_SIZE; ++i) {
    for (pHashVal p = Map[i]; p != NULL; p = Map[i]) {
      Map[i] = p->next;
      free(p);
    }
  }
}

// for debugging and testing
void printHash() {
  for (int i = 0; i < HASH_SIZE; ++i) {
    printf("Map[%d] =", i);
    for (pHashVal pVal = Map[i]; pVal != NULL; pVal = pVal->next) {
      printf(" (%d, %d, %p)", pVal->key, pVal->match_id, pVal->p_server);
    }
    printf("\n");
  }
  printf("\n");
}

// key for a given id is simply the id modulo the size of the map
int hashKey(int id) {
  return id & HASH_SIZE;
}

// get a pointer to the hashValue of a given id or NULL
pHashVal getHashValue(int id) {
  pHashVal pVal = Map[hashKey(id)];
  while (pVal != NULL && pVal->key != id) pVal = pVal->next;
  return pVal;
}

// get the maching id of a given id or -1
int getMatchId(int id) {
  pHashVal pVal = getHashValue(id);
  if (pVal == NULL) return -1;
  return pVal->match_id;
}

// insert id to the hashmap with a macthing id and (optional) server info
void setHashValue(int id, int match_id, struct ServerInfo* p_server) {
  pHashVal pVal = getHashValue(id);
  if (pVal == NULL) {  // insert new entry to map
    int ikey = hashKey(id);
    pVal = malloc(sizeof(struct hashValue));
    pVal->key = id;
    pVal->next = Map[ikey];
    Map[ikey] = pVal;
  }
  pVal->match_id = match_id;
  pVal->p_server = p_server;
  gettimeofday(&(pVal->last_communication), NULL);
}

// remove id from the hashmap.
void removeFromHash(int id) {
  int i = hashKey(id);
  pHashVal prev = NULL;
  pHashVal pVal = Map[i];
  while (pVal != NULL && pVal->key != id) {
    prev = pVal;
    pVal = pVal->next;
  }
  if (pVal == NULL) return;  // not found
  if (prev != NULL) prev->next = pVal->next;
  else Map[i] = pVal->next;
  free(pVal);
}

void updateTime(int id, int mid) {
  pHashVal pVal = getHashValue(id);
  if (pVal != NULL) {
    gettimeofday(&(pVal->last_communication), NULL);
  }
  pVal = getHashValue(mid);
  if (pVal != NULL) {
    gettimeofday(&(pVal->last_communication), NULL);
  }
}

// close the server-id used for healthcheck for a server and return true.
bool closeHealthcheckId(int id, bool up, struct ServerInfo* serv) {
  serv->up = up;
  printf("server on port %d, health %d/%d, and is %s\n", serv->port,
          serv->num_err, serv->num_req, (serv->up) ? "up" : "down");
  serv->hc_id = -1;
  FD_CLR(id, &all_ids);
  // printf("=====> closing %d (hc)\n", id);
  --num_id;
  if (id == max_id) --max_id;
  close(id);
  return true;
}

// add a socket id to the set of ids
void addId(int id) {
  ++num_id;
  FD_SET(id, &all_ids);
  if (id > max_id) max_id = id;
}

// healthcheck id: clear the server's field, remove from set, and close socket
// client-socket pair: remove id and matching id from hashmap, close the ids
// and remove from set
// unmatched id: close socket and remove from hash and set, if it is there
void closeId(int id) {
  int mid = getMatchId(id);
  if (mid > 0) {  // close client-server pair
    removeFromHash(mid);
    removeFromHash(id);
    // printf("=====> closing %d\n", id);
    if (id == max_id) --max_id;
    --num_id;
    close(id);
    printf("=====> closing %d\n", mid);
    if (mid == max_id) --max_id;
    --num_id;
    --num_connections;
    close(mid);
    FD_CLR(id, &all_ids);
    FD_CLR(mid, &all_ids);
  } else {
    // is it a healthcheck id?
    for (int i = 0; i < max_servers; ++i) {
      if (id != servers[i].hc_id) continue;
      closeHealthcheckId(id, false, &servers[i]);
      return;
    }
    if (id != sid) { // do not close our own socket!
      // single id, but not a healthcheck id
      removeFromHash(id);
      //printf("=====> closing %d (single) sid is %d\n", id, sid);
      //exit(0);
      --num_id;
      if (id == max_id) --max_id;
      close(id);
      FD_CLR(id, &all_ids);
    }
  }
}

// compare two servers
bool compareServers(struct ServerInfo* pserver1, struct ServerInfo* pserver2) {
  if (pserver1->up && !pserver2->up) return true;
  if (!pserver1->up && pserver2->up) return false;
  if (pserver1->num_req == pserver2->num_req)
    return pserver1->num_err < pserver2->num_err;
  return pserver1->num_req < pserver2->num_req;
}

/*
 * client_connect takes a port number and establishes a connection as a client.
 * connectport: port number of server to connect to
 * returns: valid socket if successful, -1 otherwise
 */
int client_connect(uint16_t connectport) {
    int connfd;
    struct sockaddr_in servaddr;

    connfd = socket(AF_INET, SOCK_STREAM, 0);
    if (connfd < 0)
        return -1;
    memset(&servaddr, 0, sizeof servaddr);

    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(connectport);

    /* For this assignment the IP address can be fixed */
    inet_pton(AF_INET, "127.0.0.1", &(servaddr.sin_addr));

    if (connect(connfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
        return -1;
    return connfd;
}

/*
 * server_listen takes a port number and creates a socket to listen on
 * that port.
 * port: the port number to receive connections
 * returns: valid socket if successful, -1 otherwise
 */
int server_listen(int port) {
    int listenfd;
    int enable = 1;
    struct sockaddr_in servaddr;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0)
        return -1;
    memset(&servaddr, 0, sizeof servaddr);
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htons(INADDR_ANY);
    servaddr.sin_port = htons(port);

    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0)
        return -1;
    if (bind(listenfd, (struct sockaddr*) &servaddr, sizeof servaddr) < 0)
        return -1;
    if (listen(listenfd, 500) < 0)
        return -1;
    return listenfd;
}

// check whether a server is up or down
void checkServer(struct ServerInfo* serv) {
  // connect to server
  serv->hc_id = client_connect(serv->port);
  if (serv->hc_id < 0) {
    fprintf(stderr, "cannot connect to port %d\n", serv->port);
    serv->up = false;
    return;
  }
  // printf("=====> opened %d (client_connect check server)\n", serv->hc_id);
  // send healthcheck request
  int n = send(serv->hc_id, hcheck, strlen(hcheck), 0);
  if (n < 0) {
    fprintf(stderr, "error sending to port %d\n", serv->port);
    // printf("=====> closing %d (check server %d)\n", serv->hc_id, n);
    close(serv->hc_id);
    serv->hc_id = -1;
    serv->up = false;
    return;
  } else if (n == 0) {
    fprintf(stderr, "connection to port %d terminated\n", serv->port);
    // printf("=====> closing %d (check server %d)\n", serv->hc_id, n);
    close(serv->hc_id);
    serv->hc_id = -1;
    serv->up = false;
    return;
  }
  // keep the healthcheck socket id in the set of all socket ids
  gettimeofday(&(serv->req_time), NULL);
  addId(serv->hc_id);
}

// check if socket id has a healthcheck response
bool healthcheck_response(int id, char* buffer, int nbytes) {
  for (int i = 0; i < max_servers; ++i) {
    if (id != servers[i].hc_id) continue;
    buffer[nbytes] = '\0';
    // buffer should resemble: "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n0\n12"
    float ver;
    int status, len;
    sscanf(buffer, "HTTP/%g %d OK\r\nContent-Length: %d", &ver, &status, &len);
    char* content = strstr(buffer, "\r\n\r\n");
    if (status != 200 || len < 3 || content == NULL) {
      fprintf(stderr, "unexpected reponse: %s\n", buffer);
      return closeHealthcheckId(id, false, &servers[i]);
    }
    content += strlen("\r\n\r\n");
    // the content might not have fully arrived yet
    int len_so_far = strlen(content);
    while (len_so_far < len) {
      // read missing characters and allow one extra to notice format errors
      int n = recv(servers[i].hc_id, content + len_so_far,
                   len - len_so_far + 1, 0);
      if (n <= 0) {
        fprintf(stderr, "error receiving from port %d\n", servers[i].port);
        return closeHealthcheckId(id, false, &servers[i]);
      }
      len_so_far += n;
    }
    if (len_so_far > len) {
      fprintf(stderr, "unexpected response %s\n", buffer);
      return closeHealthcheckId(id, false, &servers[i]);
    }
    *(content + len_so_far) = '\0';
    if (sscanf(content, "%d\n%d",
               &servers[i].num_err, &servers[i].num_req) != 2) {
      fprintf(stderr, "unexpected response %s\n", buffer);
      return closeHealthcheckId(id, false, &servers[i]);
    }
    return closeHealthcheckId(id, true, &servers[i]);
  }
  return false;
}

// send all len bytes in buff, if possible. return number of bytes sent.
int send_all(int id, char* buf, int len) {
  int len_so_far = 0;
  while (len_so_far < len) {
    int n = send(id, buf + len_so_far, len - len_so_far, 0);
    if (n <= 0) break;
    len_so_far += n;
  }
  return len_so_far;
}

// select a server to handle a new client
void selectServer(int id) {
  // select the best server available
  int selected = 0;
  for (int i = 0; i < max_servers; ++i) {
    if (compareServers(&servers[i], &servers[selected])) selected = i;
  }
  // become a client of the selected server
  int connfd = client_connect(servers[selected].port);
  printf("=====> opened %d (client_connect select server)\n", connfd);
  setHashValue(id, connfd, &servers[selected]);
  setHashValue(connfd, id, NULL);
  addId(connfd);
}

bool ParseCommandLine(int argc, char **argv) {
  extern char *optarg;
  extern int optind;
  static char usage[] =
      "usage: %s [-N max_parallel_connections] [-R healthcheck_rate] "
      "port server_port [server_port*]\n";
  int c;

  max_timeout.tv_sec = 4;  // default timeout TODO: check val
  max_timeout.tv_usec = 0;

  bool n_flag = false;
  bool r_flag = false;
  // TODO: remove X and T from the flags
  while ((c = getopt (argc, argv, "N:R:X:T:")) != -1) {
    switch (c) {
      case 'N': // TODO: what is this??
        if (n_flag) {
            fprintf(stderr, usage, argv[0]);
            return false;
        }
        n_flag = true;
        max_connections = atoi(optarg);
        if (max_connections < 1) {
          fprintf(stderr, "Error: N must be at least 1\n");
          return false;
        }
        break;
      case 'R':
        if (r_flag) {
          fprintf(stderr, usage, argv[0]);
          return false;
        }
        r_flag = true;
        rate_checks = atoi(optarg);
        if (rate_checks < 1) {
          fprintf(stderr, "Error: R must be at least 1\n");
          return false;
        }
        break;
      case 'T':
        max_timeout.tv_sec = atoi(optarg);
        max_timeout.tv_usec = 0;
        break;
      case 'X':
        freq_checks = atoi(optarg);
        break;
      default:
        fprintf(stderr, usage, argv[0]);
        return false;
    }
  }

  // port
  if (optind >= argc) {
    fprintf(stderr, usage, argv[0]);
    return false;
  }
  uint16_t port = atoi(argv[optind++]);
  if (port < 1) {
    fprintf(stderr, "Error: port must be positive\n");
    return false;
  }
  sid = server_listen(port);
  if (sid < 0) {
    fprintf(stderr, "Error: failed to connect to %d\n", port);
    return false;
  }
  // printf("=====> opened %d (sid)\n", sid);
  addId(sid);
  // at least one server port
  if (argc - optind <= 0) {
    fprintf(stderr, usage, argv[0]);
    return false;
  }
  // number of server ports coming up
  max_servers = argc - optind;
  servers = malloc(max_servers * sizeof(struct ServerInfo));
  for (int i = 0; i < max_servers; ++i) {
    servers[i].port = atoi(argv[optind++]);
    if (servers[i].port < 1) {
      fprintf(stderr, "Error: port must be positive\n");
      free(servers);
      return false;
    }
    servers[i].num_err = servers[i].num_req = 0;
    servers[i].up = true;
    servers[i].hc_id = -1;
  }
  return true;
}

void checkTimeout(int id) {
  if (id == sid) return;
  struct timeval t;
  gettimeofday(&t, NULL);
  pHashVal pVal = getHashValue(id);
  if (pVal == NULL) {  // could be a healthcheck socket id
    for (int i = 0; i < max_servers; ++i) {
      if (id != servers[i].hc_id) continue;
      long dt = (t.tv_sec - servers[i].req_time.tv_sec)*1000000L +
                (t.tv_usec - servers[i].req_time.tv_usec);
      if (dt > 1000000L * max_timeout.tv_sec) {
        closeHealthcheckId(id, false, &(servers[i]));
      }
      return;
    }
  } else if (pVal != NULL && pVal->p_server != NULL) {  // client
    long dt = (t.tv_sec - pVal->last_communication.tv_sec)*1000000L +
              (t.tv_usec - pVal->last_communication.tv_usec);
    if (dt > 1000000L * max_timeout.tv_sec) {
      printf("sending err500 to %d\n", id); // TODO: remove
      (void) send(id, err500, strlen(err500), 0);
      // mark server as being down
      pVal->p_server->up = false;
      closeId(id);
    }
  }
}

// perform health checks for all servers if either R requests were sent or X
// seconds have passed since the last health check
// req_counter's value is normally a positive number, except for the very first
// call to this method when it is 0.
void healthchecks(struct timeval* t1, int* req_counter) {
  struct timeval t2;
  gettimeofday(&t2, NULL);
  if (*req_counter < 0 ||  // first time
      *req_counter >= rate_checks || (t2.tv_sec - t1->tv_sec) >= freq_checks) {
    *req_counter = 0;
    *t1 = t2;
    for (int i = 0; i < max_servers; ++i) {
      checkServer(&servers[i]);
    }
  }
}

int main(int argc,char **argv) {
  // initialize the set of all socket ids and hash table
  FD_ZERO(&all_ids);
  max_id = -1;
  clearHash();

  // Parse flags in command line.
  if (!ParseCommandLine(argc, argv)) return EXIT_FAILURE;

  // printf("server id %d, N %d, R %d, X %d, T %ld, client ids",
  //        sid, max_servers, rate_checks, freq_checks, max_timeout.tv_sec);
  // for (int i = 0; i < max_servers; ++i) printf(" %d", servers[i].port);
  // printf("\n");

  // read_ids is a set of the socket ids that can be read, which would be
  // repeatedly initialized using all socket ids and modified by a call to
  // select after which it becomes the subset of ids that we can read from.
  fd_set read_ids;

  //struct sockaddr c_addr; // client address
  //socklen_t addrlen;

  // keep time
  struct timeval t1; gettimeofday(&t1, NULL);
  struct timeval t_debug1; gettimeofday(&t_debug1, NULL);
  // initialize req_counter to -1 indicating first call to healthcheck required
  int req_counter = -1;

  // printf("FD_SETSIZE = %d\n", FD_SETSIZE);
  while (true) {
    // send health checks, depending on number of requests (and R) and elapsed
    // time (and X)
    healthchecks(&t1, &req_counter);
    // after a call to select, read_ids would be a subset of all_ids
    read_ids = all_ids;
    struct timeval timeout = max_timeout;
    int nid = select(max_id + 1,
                     &read_ids, /* writing */ NULL, /* error */ NULL, &timeout);
    // nid is the number of ids we can read from
    if (nid < 0) {
      perror("select");
      return EXIT_FAILURE;
    }
    // go over all ids to see if we can read from them or if we reached timeout
    for (int i = 0; i <= max_id; ++i) {
      char buffer[BUFF_SIZE + 1];
      if (FD_ISSET(i, &read_ids)) {
        if (i == sid && num_connections < max_connections) {
          // accept new connection (client request)
          int cid = accept(sid, NULL, NULL);
          if (cid < 0) {
            // printf("=======> %d = accept(%d,...) \n", cid, sid);
            perror("accept");
            continue;
          }
          // increment number of requests
          ++req_counter;
          ++num_connections;
          printf("new client %d (%d)\n", cid, num_connections); // TODO: remove
          addId(cid);
        } else {
          // receive data from existing socket
          int nbytes = recv(i, buffer, BUFF_SIZE, 0);
          // printf("received %d bytes from %d\n", nbytes, i); // TODO: remove
          if (nbytes <= 0) {
            // client is gone
            if (nbytes < 0) {
              perror("recv");
            } else { // (nbytes == 0)
              printf("socket %d disconnected\n", i);
            }
            closeId(i);
          } else if (!healthcheck_response(i, buffer, nbytes)) {
            // got client-server data - send to correct id
            // if client/server already matched with server/client, the match is
            // found in pVal
            pHashVal pVal = getHashValue(i);
            if (pVal == NULL) {
              // a new client; select a server to handle it
              selectServer(i);
            }
            int mid = getMatchId(i);
            if (mid < 0) {  // no server able to handle client
              (void) send(i, err500, strlen(err500), 0);
              closeId(i);
              continue;
            }
            // printf("sending %d bytes to %d\n", nbytes, mid); // TODO: remove
            int n_sent = send_all(mid, buffer, nbytes);
            if (n_sent != nbytes) {
              fprintf(stderr,
                      "sent only %d bytes out of %d from client %d to server %d\n",
                      n_sent, nbytes, i, mid);
            }
            updateTime(i, mid);
          }
        }
      } else if (i != sid && FD_ISSET(i, &all_ids)) {
        // check for timeout
        // if an id is waiting too long, send it err500 and close
        checkTimeout(i);
      }
    }
  }
  // should not reach, but it is a good habit
  free(servers);
  emptyHash();
}
