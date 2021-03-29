# loadbalancer.c

### Jay Montoya
### 1742317 | jaanmont @ucsc.edu

#### Instructions for building "loadbalancer":
* make: builds the project 
* make clean: removes built artifacts except for the final executable
* make spotless: removes all built artifacts including the final executable

#### Run "loadbalancer" in the following format: 
#### "./loadbalancer [listen-port] [slave-port-1]...[slave-port-N]"
* *listen-port* is where clients will send their requests
* *slave-port-N* is a port where a slave server is listening, for use with the load balancer.
* "-N" is an optional flag that specifies the maximum number of connections the load balancer can hold. (defaultly 5)
* "-R" is an optional flag that specifies the number of requests the system must receive between healthcheck probes.
* Optional flags may appear in any order according to the assignment specification.

#### Known bugs: 
- No currently known bugs.
