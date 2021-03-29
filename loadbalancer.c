#include<err.h>
#include<arpa/inet.h>
#include<netdb.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<sys/socket.h>
#include<sys/types.h>
#include<unistd.h>
#include<pthread.h>
#include<stdbool.h>

// definitions
#define DEFAULT_MAX_CONNECTIONS 4
#define DEFAULT_REQUESTS_BETWEEN_HC 5
#define PROBE_MESSAGE_SIZE 100
#define BUFFER_SIZE 8192
#define MAX_TIME_BETWEEN_CHECKS 5
#define REQUEST_TIME_OUT 3
const char HEALTHCHECK[] = "GET /healthcheck HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
const char ERR_RESPONSE[] = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";

// structs
struct connection {
	int connection_fd;
	struct connection* next;
};

struct thread_args {
		int thread_id;
		int* num_pending_connections;
		int num_slave_servers;
		pthread_mutex_t* connection_mutex;
		pthread_cond_t* got_connection;
		pthread_mutex_t* request_count_mutex;
		pthread_mutex_t* server_health_mutex;
		pthread_cond_t* health_check_due;
		size_t* connections_received_since_hc;
		size_t* requests_between_hc;
};

struct health_monitor_args {
	int* num_slave_servers;
	pthread_mutex_t* server_health_mutex;
	pthread_cond_t* health_check_due;
	struct scoreboard_entry* priority_server;
};

struct time_keeping_args {
	pthread_mutex_t* server_health_mutex;
	pthread_cond_t* health_check_due;
};

struct scoreboard_entry {
	uint16_t port;
	uint16_t total_requests;
	uint16_t total_errors;
	float request_error_ratio;
	bool problematic;
	struct scoreboard_entry* next;
};

struct connection* connections = NULL;
struct connection* last_connection = NULL;

struct scoreboard_entry* scoreboard = NULL;
struct scoreboard_entry* last_scoreboard_entry = NULL;
struct scoreboard_entry* cursor_entry = NULL;
struct scoreboard_entry* priority_server = NULL;

// prototypes
void add_connection(int connection_fd, pthread_mutex_t* p_mutex,
  pthread_cond_t* p_cond_var, int* num_pending_connections);
struct connection* get_connection(int* num_pending_connections);
void add_scoreboard_entry(struct scoreboard_entry* a_scoreboard_entry);
void probe_slaves(int num_slave_servers);
void process_reply(int port, int socket, uint8_t* buff);
void update_problematic(int port, bool problematic);
void update_scoreboard_entry(int port, int total_requests, int total_errors, float request_error_ratio, bool problematic);
void update_priority_server(int num_slave_servers);
int bridge_connections(int fromfd, int tofd);
int client_connect(uint16_t connectport);
void bridge_loop(int sockfd1, int sockfd2);

// -----------------------------------------------------------------------------
//  HEALTH-MONITORING THREAD
// -----------------------------------------------------------------------------
//  Threads responsible for
// -----------------------------------------------------------------------------
void* health_monitoring_thread(void* data) {
	// variables needed
	int rc;
	//struct scoreboard_entry* cursor_entry = NULL;

	// unpackage thread arguments
	struct health_monitor_args* hm_args = ((struct health_monitor_args*)(data));

	// set up a client connection between this thread and the number of servers
	printf(" [health-monitor] started...\n");
	printf(" [health-monitor] slave servers detected: %d\n", *(hm_args->num_slave_servers));

	// upon startup, the server health mutex must be locked until the first
	// healthcheck completes
	rc = pthread_mutex_lock(hm_args->server_health_mutex);
	if (rc) {
		printf(" [health-monitor] error locking sevrer health mutex!\n");
	}

	// probe the servers, update the contents, and select the best server
	probe_slaves(*(hm_args->num_slave_servers));
	update_priority_server(*(hm_args->num_slave_servers));

	if (priority_server) {
		printf("\nPRIORITY SERVER EXISTS...\n\n");
	} else {
		printf("\nPRIORITY SERVER DOES NOT EXIST...\n\n");
	}

	while(1) {
		// release the lock by waiting on the conditional
		rc = pthread_cond_wait((hm_args->health_check_due), (hm_args->server_health_mutex));
		if (rc) {
			printf(" [health-monitor] error locking sevrer health mutex!\n");
		}

		probe_slaves(*(hm_args->num_slave_servers));
		update_priority_server(*(hm_args->num_slave_servers));

		if (priority_server) {
			printf("\nPRIORITY SERVER EXISTS...\n\n");
		} else {
			printf("\nPRIORITY SERVER DOES NOT EXIST...\n\n");
		}

	}
}

// -----------------------------------------------------------------------------
//  TIME-KEEPING THREAD
// -----------------------------------------------------------------------------
//  Simple thread thread that ensure a health-check is done at least every 5
//  seconds as defined by this program.
// -----------------------------------------------------------------------------
void* time_keeping_thread(void* data) {
		int rc;

		// unpackage thread arguments
		struct time_keeping_args* tk_args = ((struct time_keeping_args*)data);

		while (1) {
			sleep(MAX_TIME_BETWEEN_CHECKS);

			rc = pthread_cond_signal(tk_args->health_check_due);
			if (rc) {
				printf("  [time-keeper] error signalling a due healthcheck!");
			}
		}
}

// -----------------------------------------------------------------------------
//  CONNECITON-HANDLING
// -----------------------------------------------------------------------------
//  Threads responsible for obtaining a connection, choosing a server to
//  forward it too, and then setting up a bridge loop until one of the sides
//  closes the connection
// -----------------------------------------------------------------------------
void* connection_handling_thread(void* data) {
	// variables needed
	int rc, socket_to_slave, port;
	struct connection* a_connection;


	// unpackage thread arguments
	struct thread_args* t_args = ((struct thread_args*)(data));

	printf(" [thread %d] started..\n", t_args->thread_id);
	while (1) {

		// fight for the lock
		rc = pthread_mutex_lock(t_args->connection_mutex);
		if (rc) {
			printf(" [thread %d] error locking connections list mutex!\n", t_args->thread_id);
		}
		rc = 0;
		if (*(t_args->num_pending_connections) == 0) {
			printf(" [thread %d] waiting...\n", t_args->thread_id);
			rc = pthread_cond_wait(t_args->got_connection, t_args->connection_mutex);
		}
		if (rc) {
			printf(" issue with pthread_cond_wait..\n");
		}

		// obtain next connection
		if (rc == 0 && (*(t_args->num_pending_connections) > 0)) {
			a_connection = get_connection(t_args->num_pending_connections);
			rc = pthread_mutex_unlock(t_args->connection_mutex);
			if (rc) {
				printf(" [thread %d] error unlocking assignment mutex!\n", t_args->thread_id);
			}

			// handle the connection
			if (a_connection) {
				printf("  [thread %d] handling connection!\n", t_args->thread_id);

				// send the connection to the port of the priority server
				rc = pthread_mutex_lock(t_args->server_health_mutex);

				if (priority_server) {

					port = priority_server->port;

					printf("\nFORWARDING CONNECTION...\n\n");
					rc = pthread_mutex_unlock(t_args->server_health_mutex);



					// increment total_connections_received
					rc = pthread_mutex_lock(t_args->request_count_mutex);
					if (rc) {
						printf(" [thread %d] error locking system health mutex!\n", t_args->thread_id);
					}
					rc = 0;

					// check if a healthcheck is due
					(*(t_args->connections_received_since_hc))++;
					if (*(t_args->connections_received_since_hc) == *(t_args->requests_between_hc)) {
						// notify the health-monitoring thread
						rc = pthread_cond_signal(t_args->health_check_due);
						*(t_args->connections_received_since_hc) = 0;
						printf("  [HEALTHCHECK DUE]\n");
					}
					rc = pthread_mutex_unlock(t_args->request_count_mutex);
					if (rc) {
						printf(" [thread %d] error unlocking request_count_mutex!\n", t_args->thread_id);
					}

					// connect to the priority server's port..
					socket_to_slave = client_connect(port);

					if (socket_to_slave < 0) {
						printf("  [thread %d] no priority server identified! Sending 500...\n", t_args->thread_id);
						char error_reply[100] = "";
						strcat(error_reply, ERR_RESPONSE);
						send(a_connection->connection_fd, error_reply, strlen(error_reply), 0);
						close(a_connection->connection_fd);
						free(a_connection);
						continue;
					}

					// forward the connection!
					bridge_loop(a_connection->connection_fd, socket_to_slave);

					// at this point, we have completed forwarding, and bridge_loop handled
					// everything. close the connection and free the connection object.
					close(a_connection->connection_fd);
					close(socket_to_slave);
					free(a_connection);

				} else {
					rc = pthread_mutex_unlock(t_args->server_health_mutex);

					printf("  [thread %d] no priority server identified! Sending 500...\n", t_args->thread_id);
					char error_reply[100] = "";
					strcat(error_reply, ERR_RESPONSE);
					send(a_connection->connection_fd, error_reply, strlen(error_reply), 0);
					close(a_connection->connection_fd);
					free(a_connection);
					continue;
				}
			} // if the connection exists
		} // if there is a connection
	} // while true do
}

// -----------------------------------------------------------------------------
//  START-UP / RECIEVE CONNECTION
// -----------------------------------------------------------------------------
//  Thread responsible for initializing the load balancer and distributing
//  received connections amongst the connection-handling threads.
// -----------------------------------------------------------------------------
int main(int argc,char **argv) {

    // variables needed
    uint16_t port;
    int opt;
    size_t num_connections =  DEFAULT_MAX_CONNECTIONS;
    size_t requests_between_hc = DEFAULT_REQUESTS_BETWEEN_HC;
    pthread_mutex_t connection_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t got_connection = PTHREAD_COND_INITIALIZER;
		pthread_mutex_t request_count_mutex = PTHREAD_MUTEX_INITIALIZER;
		pthread_mutex_t server_health_mutex = PTHREAD_MUTEX_INITIALIZER;
		pthread_cond_t health_check_due = PTHREAD_COND_INITIALIZER;
    int num_pending_connections = 0;
		int num_slave_servers = 0;
		size_t connections_received_since_hc = 0;
		bool listener_port_found = 0;

    // parse optional flags
  	while (1) {
  		opt = getopt(argc, argv, "N:R:");
  		if (opt == -1) { break; }
  		if (opt == 'N') {
  			if (atoi(optarg) > 0) {
  				num_connections = atoi(optarg);
          printf("  max connections: %ld\n", num_connections);
  			} else {
  				printf("Invalid flag argument for -N\n");
  				return 1;
  			}
  		} else if (opt == 'R') {
        if (atoi(optarg) > 0) {
          requests_between_hc = atoi(optarg);
          printf("  request between healthchecks: %ld\n", requests_between_hc);
        } else {
          printf("Invalid flag argument for -R\n");
        }
  		} else {
  			break;
  		}
  	}

		// parse ports
		struct scoreboard_entry* a_scoreboard_entry = NULL;
    while (optind != argc) {
  		if (atoi(argv[optind]) > 0) {
				if (!listener_port_found) {
					port = atoi(argv[optind]);
					listener_port_found = 1;
				} else {
					// create and initialize a new object representing the server's health
					a_scoreboard_entry = (struct scoreboard_entry*)malloc(sizeof(struct scoreboard_entry));
					a_scoreboard_entry->port = atoi(argv[optind]);
					a_scoreboard_entry->total_requests = 0;
					a_scoreboard_entry->total_errors = 0;
					a_scoreboard_entry->request_error_ratio = 0.0;
					a_scoreboard_entry->problematic = 1;
					a_scoreboard_entry->next = NULL;

					add_scoreboard_entry(a_scoreboard_entry);
					num_slave_servers++;
					printf("  added scoreboard entry for server on port %d\n", a_scoreboard_entry->port);
				}
  		} else {
  			printf("Invalid port!\n");
  			return 1;
  		}
      optind++;
  	}

    if (argc < 3) {
        printf("missing argument(s)! see README\n");
        return 1;
    }

    // start the health monitor thread
		pthread_t hm_thread;
		struct health_monitor_args* hm_args = NULL;
		hm_args = (struct health_monitor_args*)malloc(sizeof(struct health_monitor_args));
		hm_args->num_slave_servers = &num_slave_servers;
		hm_args->server_health_mutex = &server_health_mutex;
		hm_args->health_check_due = &health_check_due;
		//hm_args->priority_server = priority_server;
		pthread_create(&hm_thread, NULL, health_monitoring_thread, (void*)hm_args);

		sleep(1);

		// start the time-keeping thread which
		pthread_t tk_thread;
		struct time_keeping_args* tk_args = NULL;
		tk_args = (struct time_keeping_args*)malloc(sizeof(struct time_keeping_args));
		tk_args->server_health_mutex = &server_health_mutex;
		tk_args->health_check_due = &health_check_due;
		pthread_create(&tk_thread, NULL, time_keeping_thread, (void*)tk_args);

		//start the connection-handling threads
		struct thread_args* t_args = NULL;
		pthread_t p_threads[num_connections];

		for (int i = 0; i < (ssize_t)num_connections; i++) {
			t_args = (struct thread_args*)malloc(sizeof(struct thread_args));
			t_args->thread_id = i;
			t_args->connection_mutex = &connection_mutex;
			t_args->got_connection = &got_connection;
			t_args->num_pending_connections = &num_pending_connections;
			t_args->connections_received_since_hc = &connections_received_since_hc;
			t_args->requests_between_hc = &requests_between_hc;
			t_args->request_count_mutex = &request_count_mutex;
			t_args->health_check_due = &health_check_due;
			t_args->num_slave_servers = num_slave_servers;
			t_args->server_health_mutex = &server_health_mutex;
			pthread_create(&p_threads[i], NULL, connection_handling_thread, (void*)t_args);
		}

    // Set up listening socket, listen, and add connections to the connection list
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    socklen_t addrlen = sizeof(server_addr);
    int server_sockd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sockd < 0) {
        perror("socket");
    }
    int enable = 1;
    int ret = setsockopt(server_sockd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    ret = bind(server_sockd, (struct sockaddr *) &server_addr, addrlen);

    struct sockaddr client_addr;
    socklen_t client_addrlen = sizeof(client_addr);

    ret = listen(server_sockd, 5);

  	if (ret == -1) {
  		printf("  issue with socket set up!\n");
  		return 1;
  	}

  	// listen and dispatch
  	while (1) {
  		printf("  dispatcher waiting...\n");
  		int connection_fd = accept(server_sockd, &client_addr, &client_addrlen);
  		if (connection_fd == -1) {
  			printf("  error accepting connection!");
  			continue;
  		}

  		// creatre assignment
  		add_connection(connection_fd, &connection_mutex, &got_connection,
        &num_pending_connections);
  		printf("  dispatcher created an assignment...\n");
  	}
  	return 1;
}

// -----------------------------------------------------------------------------
//  HELPER-FUNCTIONS
// -----------------------------------------------------------------------------
//  Helping functions used by the threads.
// ----------------------------------------------------------------------------

void add_connection(int connection_fd, pthread_mutex_t* p_mutex,
  pthread_cond_t* p_cond_var, int* num_pending_connections) {

  int rc;
  struct connection* a_connection;

  // create connection
  a_connection = (struct connection*)malloc(sizeof(struct connection));
  if (!a_connection) {
  	printf("[+] issue creating connection!");
  }

  a_connection->connection_fd = connection_fd;
  a_connection->next = NULL;

  // lock mutex over the list
  rc = pthread_mutex_lock(p_mutex);
  if (rc) {
  	printf(" [dispatcher] error locking connection mutex!\n");
  }

  // add the request to the list
  if ((*num_pending_connections) == 0) {
  	connections = a_connection;
  	last_connection = a_connection;
  } else {
  	last_connection->next = a_connection;
  	last_connection = a_connection;
  }
  (*num_pending_connections)++;

  // unlock the mutex
  rc = pthread_mutex_unlock(p_mutex);
  if (rc) {
  	printf(" [dispatcher] error unlocking connection mutex!\n");
  }

  // signal the condition variable
  rc = pthread_cond_signal(p_cond_var);
}

struct connection* get_connection(int* num_pending_connections) {
  struct connection* a_connection;

	if ((*num_pending_connections) > 0) {
		a_connection = connections;
		connections = connections->next;
		if (connections == NULL) {
				last_connection = NULL;
        if (last_connection) {
          // get rid of compiler warning
        }
		}
		(*num_pending_connections)--;
	}  else {
		a_connection = NULL;
	}
	return a_connection;
}

void add_scoreboard_entry(struct scoreboard_entry* a_scoreboard_entry) {
	if (scoreboard == NULL) {
		scoreboard = a_scoreboard_entry;
		last_scoreboard_entry = a_scoreboard_entry;
	} else {
		last_scoreboard_entry->next = a_scoreboard_entry;
		last_scoreboard_entry = a_scoreboard_entry;
	}
}

/*
 * bridge_loop forwards all messages between both sockets until the connection
 * is interrupted. It also prints a message if both channels are idle.
 * sockfd1, sockfd2: valid sockets
 */
void bridge_loop(int sockfd1, int sockfd2) {
    fd_set set;
    struct timeval timeout;
		bool request_sent = 0;
		bool response_sent = 0;

    int fromfd, tofd;
    while(1) {
        // set for select usage must be initialized before each select call
        // set manages which file descriptors are being watched
        FD_ZERO (&set);
        FD_SET (sockfd1, &set);
        FD_SET (sockfd2, &set);

        // same for timeout
        // max time waiting, 5 seconds, 0 microseconds
        timeout.tv_sec = REQUEST_TIME_OUT;
        timeout.tv_usec = 0;

        // select return the number of file descriptors ready for reading in set
        switch (select(FD_SETSIZE, &set, NULL, NULL, &timeout)) {
            case -1:
                printf("error during select in bridge_loop! exiting...\n");
                return;
            case 0:
                printf("both channels are idle, waiting again\n");
                continue;
            default:
                if (FD_ISSET(sockfd1, &set)) {
                    fromfd = sockfd1;
                    tofd = sockfd2;
                } else if (FD_ISSET(sockfd2, &set)) {
                    fromfd = sockfd2;
                    tofd = sockfd1;
                } else {
                    printf("this should be unreachable\n");
                    return;
                }
        }
        if (bridge_connections(fromfd, tofd) <= 0) {
					// make sure the server replied..
					if ( ( (!response_sent) && request_sent ) ) {
						printf("server never replied to client.. sending 500...\n");
						char error_reply[100] = "";
						strcat(error_reply, ERR_RESPONSE);
						send(sockfd1, error_reply, strlen(error_reply), 0);
						close(sockfd1);
						close(sockfd2);
					}
          return;
				} else {
					if (fromfd == sockfd1) {
						// client did send something..
						request_sent = 1;
					} else if (fromfd == sockfd2) {
						// server did send something..
						response_sent = 1;
					}
				}
    }
}

/*
 * bridge_connections send up to 100 bytes from fromfd to tofd
 * fromfd, tofd: valid sockets
 * returns: number of bytes sent, 0 if connection closed, -1 on error
 */
int bridge_connections(int fromfd, int tofd) {
    char recvline[BUFFER_SIZE + 1];
    int n = recv(fromfd, recvline, BUFFER_SIZE, 0);
    if (n < 0) {
        printf("connection error receiving\n");
        return -1;
    } else if (n == 0) {
        printf("receiving connection ended\n");
				close(tofd);
        return 0;
    }
    recvline[n] = '\0';
    //sleep(1);
    n = send(tofd, recvline, n, 0);
    if (n < 0) {
        printf("connection error sending\n");
        return -1;
    } else if (n == 0) {
        printf("sending connection ended\n");
				close(fromfd);
        return 0;
    }
    return n;
}

/*
 * client_connect takes a port number and establishes a connection as a client.
 * connectport: port number of server to connect to
 * returns: valid socket if successful, -1 otherwise
 */
int client_connect(uint16_t connectport) {
    int connfd;
    struct sockaddr_in servaddr;

    connfd=socket(AF_INET,SOCK_STREAM,0);
    if (connfd < 0)
        return -1;
    memset(&servaddr, 0, sizeof servaddr);

    servaddr.sin_family=AF_INET;
    servaddr.sin_port=htons(connectport);

    /* For this connection the IP address can be fixed */
    inet_pton(AF_INET,"127.0.0.1",&(servaddr.sin_addr));

    if(connect(connfd,(struct sockaddr *)&servaddr,sizeof(servaddr)) < 0) {
			close(connfd);
			return -1;
		}
    return connfd;
}

/*  probe_slaves()
 *
 *  function responsible for setting up client connections to the servers and
 *  issuing a health check to each, documenting the changes
 *  PRE-CONDITIONS: num_slave_servers represents the total number of slave
 *     servers determined at the beginning of program and is not 0
 * 	POST-CONDITIONS: the scoreboard_entry struct representing each slave server
 *     is updated with the most recent information on that server.
 */
void probe_slaves(int num_slave_servers) {
	fd_set set, unverified_set;
	int sockets_to_slaves[num_slave_servers];
	struct timeval timeout;
	cursor_entry = scoreboard;
	uint8_t buff[BUFFER_SIZE + 1];

	// zero the FD set
	FD_ZERO (&set);
	FD_ZERO (&unverified_set);

	// create all sockets to sent information to the servers
	printf("   [health-monitor] generating client sockets\n");
	for (int i  = 0; i < (num_slave_servers); i++) {

		// create a client socket to that port, add it to the FD_SET, advance cursor
		sockets_to_slaves[i] = client_connect(cursor_entry->port);
		if (sockets_to_slaves[i] < 0) {
			printf("   [health-monitor] error creating client sock to port %d\n", cursor_entry->port);
			printf("      marking problematic...\n");
			update_problematic(cursor_entry->port, 1);
		} else {
			printf("\nADDING %d TO SOCKET TO CLIENT DESCRIPTOR SET..\n\n", sockets_to_slaves[i]);
			FD_SET ( sockets_to_slaves[i], &set );
			FD_SET ( sockets_to_slaves[i], &unverified_set);
		}

		cursor_entry = cursor_entry->next;
	}

	// now send the healthchecks to each socket
	for (int i = 0; i < num_slave_servers; i++) {

		if (sockets_to_slaves[i] < 0) {
			continue;
		}

		char probe_message[PROBE_MESSAGE_SIZE] = "";
		strcat(probe_message, HEALTHCHECK);
		send(sockets_to_slaves[i], probe_message, strlen(probe_message), 0);
	}
	printf("   [health-monitor] healthchecks sent!\n");

	//return;
	timeout.tv_sec = REQUEST_TIME_OUT;
	timeout.tv_usec = 0;

	int servers_handled = 0;
	int ret;
	bool expired_timeout = 0;
	while (servers_handled < num_slave_servers) {

		// listen for responses and update values in the scoreboard
		ret = select(FD_SETSIZE, &set, NULL, NULL, &timeout);
		switch(ret) {
			case -1:
				printf("   [health-monitor] error with select!\n");
				char err_msg[100] = "select error";
				perror(err_msg);

				return;
			case 0:
				 //timeout expired..
				 expired_timeout = 1;
			default:
				// fd's now ready for reading are now the only fd's in the set.
				// others did not respond in the 5 seconds and should be marked dead
				cursor_entry = scoreboard;
				for (int i = 0; i < num_slave_servers; i++) {

					if (sockets_to_slaves[i] < 0) {
						cursor_entry = cursor_entry->next;
						servers_handled++;
						continue;
					}

					if ( FD_ISSET(sockets_to_slaves[i], &set ) ) {
						// information is ready for reading
						process_reply(cursor_entry->port, sockets_to_slaves[i], buff);
						FD_CLR(sockets_to_slaves[i] , &set);
						FD_CLR(sockets_to_slaves[i] , &unverified_set);
						servers_handled++;
					} else {
						// mark as problematic
						if (expired_timeout && FD_ISSET(sockets_to_slaves[i], &unverified_set)) {
							update_problematic(cursor_entry->port,1);
							servers_handled++;
							close(sockets_to_slaves[i]);
						} else if (!expired_timeout && FD_ISSET(sockets_to_slaves[i], &unverified_set)) {
							FD_SET(sockets_to_slaves[i] , &set);
						}
					}
					cursor_entry = cursor_entry->next;
				} // end of for
				printf("   [health-monitor] done processing replies...\n");
		}// end of switch
	}// end of while

	for (int i = 0; i < num_slave_servers; i++) {
		if (sockets_to_slaves[i] > 0) {
			close(sockets_to_slaves[i]);
		}
	}

}

/*  process_reply()
 *
 *  function that takes a client socket descriptor to the server identified by
 *     'port' and uses 'buff' to read the response and process it's contents.
 *  PRE-CONDITIONS: port is a valid port, socket is not -1.
 * 	POST-CONDITIONS: The reply from the server is processed, calculations are
 *      made, and the scoreboard entry for the server on 'port' has its fields
 *      updated by update_scoreboard_entry().
 */
void process_reply(int port, int socket, uint8_t* buff ) {
	// variables needed
	int recv_status;
	int total_bytes_received = 0;
	char dummy[5] = "";
	char* pos = dummy;
	int num_errors;
	int num_requests;
	size_t content_length = -1;
	float request_error_ratio;

	// clear buffer
	//printf("    [process-reply] processing probe results for port %d..\n", port);
	memset(buff, 0, BUFFER_SIZE+1);

	do {
		// receiving (assumes that a healthcheck reply is received in full)
		recv_status = recv(socket, buff + total_bytes_received, BUFFER_SIZE, 0);

		if (recv_status == -1) {
			printf("Error with receiving..\n");
		} else {
			total_bytes_received+= recv_status;
		}
		buff[total_bytes_received] = '\0';

		//close(socket);

		// copy contents to a char* buffer to perform string operations on
		char response[BUFFER_SIZE+1];
	  strcpy(response, (char*)buff);

		//printf("\tresponse is %s\n\n", response);

		if (strstr(response, "HTTP/1.1 200 OK") == NULL) {
			// not a valid healthcheck response...
			update_problematic(port, 1);
			return;
		}

		if (sscanf(response, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n%d\n%d", &content_length, &num_errors, &num_requests) != EOF) {
			printf("\nGOOD HEALTHCHECK RETURNED: %d %d content-length:  %ld\n\n", num_requests, num_errors, content_length);
			pos = strstr(response, "\r\n\r\n") + 4;

		} else {
			printf("\nBAD HEALTHCHECK RETURNED\n\n");
		}
	} while (strlen(pos) != content_length);

	/*if (strstr(response, "\r\n\r\n") != NULL) {
		pos = strstr(response, "\r\n\r\n");
	} else {
		printf("    [process-reply] double CRLF not detected in response for port %d!\n", port);
		return;
	}

	//printf("    [process-reply] tokenizing reply..\n");

	char * token;
	char * rest = NULL;
	token = strtok_r(pos+4, "\n", &rest);

	//printf("\ttoken = %s\n", token);

	// process the number of errors
	num_errors = atoi(token);

	// process the number of reuests
	token = strtok_r(NULL, "\n", &rest);

	//printf("\ttoken = %s\n", token);
	num_requests = atoi(token);*/

	// calculate error response ratio
	//printf("    [process-reply] received %d and %d\n", num_errors, num_requests);

	if (num_requests == 0) {
		//printf("    zero load server detected!\n");
		request_error_ratio = 0.0;
	} else if (num_errors == 0) {
		//printf("    undefeated server detected!\n");
		request_error_ratio = -1.0;
	} else {
		//printf("    normal health calculation detected!\n");
		request_error_ratio = num_requests / (float)num_errors;
	}


	// everything finished for this server, update it's scoreboard entry
	update_scoreboard_entry(port, num_requests, num_errors, request_error_ratio, 0);
}


/*  update_problematic()
 *
 *  function responsible for updating the 'problematic' field for a server
 *     scoreboard_entry struct which is identified by the port
 *  PRE-CONDITIONS: 'port' is a valid port of a scoreboard_entry struct in the
 *     linked list.
 * 	POST-CONDITIONS: the problematic field for the target server is updated to
 *     paramter 'problematic'.
 */
void update_problematic(int port, bool problematic) {

	//struct scoreboard_entry* cursor_entry = scoreboard;
	cursor_entry = scoreboard;
	if (!scoreboard) {
		printf("  [update-problematic] error updating scoreboard entry!\n");
		return;
	}

	while (cursor_entry != NULL) {
		if (cursor_entry->port == port) {
			cursor_entry->problematic = problematic;
			printf("  [update-problematic] updated problematic to %d for port %d..\n", problematic, port);
			return;
		} else {
			cursor_entry = cursor_entry->next;
		}
	}

	printf("  [update-problematic] could not find a scoreboard entry! given %d\n", port);
}

/*  update_scoreboard_entry()
 *
 *  function responsible for updating all of the fields for a server scoreboard
 *     entry which is identified by the 'port' paramter.
 *  PRE-CONDITIONS: 'port' is a valid port of a scoreboard_entry struct in the
 *     linked list.
 * 	POST-CONDITIONS: the four fields described in the function parameters have
 *     been updated.
 */
void update_scoreboard_entry(int port, int total_requests, int total_errors, float request_error_ratio, bool problematic) {
		//struct  scoreboard_entry* cursor_entry = scoreboard;
		cursor_entry = scoreboard;
		if (!scoreboard) {
			printf("  [UPDATE] error updating scoreboard entry!\n");
			return;
		}

		while (cursor_entry != NULL) {
			if (cursor_entry->port == port) {
				cursor_entry->total_requests = total_requests;
				cursor_entry->total_errors = total_errors;
				cursor_entry->request_error_ratio = request_error_ratio;
				cursor_entry->problematic = problematic;
				printf("      [UPDATE] updated scoreboard entry for port %d...\n", port);
				return;
			} else {
				cursor_entry = cursor_entry->next;
			}
		}

		printf("  [UPDATE] could not find a scoreboard entry!\n");
}

/*
 * examines the server scoreboard entry and sets priority_server to the
 * healthiest entry.
 *
 */
void update_priority_server(int num_slave_servers) {

	// variables needed
	cursor_entry = scoreboard;
	priority_server = NULL;
	int num_qualified_servers = 0;


	// compare the rest of the servers to the first, moving the priority if necessary
	for (int i = 0; i < num_slave_servers; i++) {

		// find a server thats up first
		if (priority_server == NULL) {
			if (cursor_entry->problematic == 0) {
				priority_server = cursor_entry;
				cursor_entry = cursor_entry->next; // set qualified == 1?
				num_qualified_servers = 1;
				continue;
			} else {
				cursor_entry = cursor_entry->next;
				continue;
			}
		}

		// compare the cursor to the current recordholder
		if ((cursor_entry->total_requests < priority_server->total_requests) && (cursor_entry->problematic == 0)) {
			priority_server = cursor_entry;
			num_qualified_servers = 1;
		} else if ((cursor_entry->total_requests == priority_server->total_requests) && (cursor_entry->problematic == 0)) {
			num_qualified_servers++;
		}
		cursor_entry = cursor_entry->next;
	}

	// if there is still more than 1 qualified server, use the ratio as the tie breaker
	if (num_qualified_servers > 1) {
		cursor_entry = priority_server->next;
		while (cursor_entry != NULL) {

			// is this a contender?
			if ((cursor_entry->total_requests == priority_server->total_requests) && (cursor_entry->problematic == 0)) {
				// break tie if you can..
				if ((cursor_entry->request_error_ratio > priority_server->request_error_ratio) && (priority_server->request_error_ratio != -1.0)) {
					priority_server = cursor_entry;
					num_qualified_servers = 1;
				} else if (cursor_entry->request_error_ratio < priority_server->request_error_ratio) {
					// doesnt win.. unless it's -1
					if (cursor_entry->request_error_ratio == -1.0) {
						priority_server = cursor_entry;
						num_qualified_servers = 1;
						break;
					} else {
						num_qualified_servers--;
					}
				} else {
					// their ratios are the same too!...
					// they are both equally qualified
				}
			}
			cursor_entry = cursor_entry->next;
		} // end of while
	}
	// priority server selected
	if (priority_server) {
		printf("   [PRIORITY CHANGE] server on port %d has been declared the healthiest!\n", priority_server->port);
	} else {
		printf("   [PRIORITY CHANGE] all servers are down! uwu\n");
	}
}
