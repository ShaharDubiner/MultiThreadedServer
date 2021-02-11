# Load Balancer - Assignment 3

loadbalancer redirects client requests to one of several servers based on the servers' health and estimate of their current load.

Usage:
./loadbalancer port_num server_port_num [server_port_num]* [-N max_parallel_connections] [-R rate_healthcheck_per_num_requests]

The input parameters may appear in any order, including mixing the port_numbers between the flag arguments.

For example:

```shell
./loadbalancer 8080 -N 8 1234 -R 7 5678 1248
```

Sets loadbalancer port to be 8080, has it communicate with 3 servers whose ports are 1234, 5678, and 1248. The maximal number of parallel connections it is expected to handle is 8. It would perform health-checks of the servers at least once per 7 requests.

To use the loadbalancer
-  In one terminal run it. For example:
   ```shell
   ./loadbalancer 8080 1234 5678
   ```
-  In another terminal run the servers:
   ```shell
   ./httpserver 1234 -l /tmp/log1234 &
   ./httpserver 5678 -l /tmp/log5678 &
   ```
-  In a third terminal send client requests:
   ```shell
   curl -v http://localhost:8080/abc &
   curl -T xyz http://localhost:8080/xyz &
   ```

## Limitations
loadbalancer can not really be used as a good load balancer for several reasons.
1. It is assumed all the servers can respond to health check queries (using a format identical to getting a file named "healthcheck").
2. It is assumed all servers have access to the same set of resources.
3. The servers' health is checked in a somewhat simplified way, initiating healthcheck requests and checking for timeouts. Once a server is assumed to be down, an error is sent to any clients waiting for it.
4. The servers' load is estimated in a simplified way, based on a statistics of total number of requests and total number of errors. This does not take into account the actual load of requests the servers are handling at the current moment, their average response time, what is the frequency they have been down etc.
5. No attempt is being made to keep the same client-server matched pairs for more than a single request, although for some applications this could be useful.