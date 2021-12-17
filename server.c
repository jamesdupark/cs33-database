#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include "./comm.h"
#include "./db.h"

/*
 * Use the variables in this struct to synchronize your main thread with client
 * threads. Note that all client threads must have terminated before you clean
 * up the database.
 */
typedef struct server_control {
    pthread_mutex_t server_mutex;
    pthread_cond_t server_cond;
    int num_client_threads;
} server_control_t;

/*
 * Controls when the clients in the client thread list should be stopped and
 * let go.
 */
typedef struct client_control {
    pthread_mutex_t go_mutex;
    pthread_cond_t go;
    int stopped;
} client_control_t;

/*
 * The encapsulation of a client thread, i.e., the thread that handles
 * commands from clients.
 */
typedef struct client {
    pthread_t thread;
    FILE *cxstr;  // File stream for input and output

    // For client list
    struct client *prev;
    struct client *next;
} client_t;

/*
 * The encapsulation of a thread that handles signals sent to the server.
 * When SIGINT is sent to the server all client threads should be destroyed.
 */
typedef struct sig_handler {
    sigset_t set;
    pthread_t thread;
} sig_handler_t;

client_t *thread_list_head;
pthread_mutex_t thread_list_mutex = PTHREAD_MUTEX_INITIALIZER;
int accepting = 1;
pthread_mutex_t accepting_mutex = PTHREAD_MUTEX_INITIALIZER;
server_control_t server_state = { PTHREAD_MUTEX_INITIALIZER, 
                                  PTHREAD_COND_INITIALIZER, 0 };
client_control_t stop_go = { PTHREAD_MUTEX_INITIALIZER, 
                              PTHREAD_COND_INITIALIZER, 0 };

void *run_client(void *arg);
void *monitor_signal(void *arg);
void thread_cleanup(void *arg);
void *checked_malloc(size_t size);

/*
 * checked_malloc() - performs an error-checked version of malloc and exits if
 * out of memory. Will also exit if 0 is passed in as an argument.
 * 
 * Arguments: size: size of memory to be allocated by malloc
 * 
 * Returns: pointer to newly allocated memory
 * 
 */
void *checked_malloc(size_t size) {
    void *ptr;
    if ((ptr = malloc(size)) == NULL) {
        errno = ENOMEM;
        perror("malloc:");
        exit(1);
    }

    return ptr;
}

/*
 * checked_pthr_create() - performs an error-checked version of pthread_create 
 * and exits if it errors.
 * 
 * Arguments: thread: pointer to where new thread should be initialized, attr:
 * attributes argument for pthread_create, start_routine: function taking in a
 * single argument, to be run upon creation of thread, arg: argument to be
 * passed into start_routine. 
 */
void checked_pthr_create(pthread_t *thread, const pthread_attr_t *attr, 
                         void *(*start_routine)(void *), void *arg) {
    int en;
    if ((en = pthread_create(thread, attr, start_routine, arg))) {
        handle_error_en(en, "pthread_create:");
    }
}


// Called by client threads to wait until progress is permitted
void client_control_wait() {
    // TODO: Block the calling thread until the main thread calls
    // client_control_release(). See the client_control_t struct.
    pthread_mutex_lock(&stop_go.go_mutex);
    pthread_cleanup_push((void (*)(void *)) pthread_mutex_unlock, &stop_go.go_mutex);
    while (stop_go.stopped) {
        pthread_cond_wait(&stop_go.go, &stop_go.go_mutex);
    }

    pthread_cleanup_pop(1);
}

// Called by main thread to stop client threads
void client_control_stop() {
    // TODO: Ensure that the next time client threads call client_control_wait()
    // at the top of the event loop in run_client, they will block.
    pthread_mutex_lock(&stop_go.go_mutex);
    stop_go.stopped = 1;
    pthread_mutex_unlock(&stop_go.go_mutex);
}

// Called by main thread to resume client threads
void client_control_release() {
    // TODO: Allow clients that are blocked within client_control_wait()
    // to continue. See the client_control_t struct.
    pthread_mutex_lock(&stop_go.go_mutex);
    stop_go.stopped = 0;
    pthread_cond_broadcast(&stop_go.go);
    pthread_mutex_unlock(&stop_go.go_mutex);
}

// Called by listener (in comm.c) to create a new client thread
/*
 * client_constructor - called by listener thread upon recieving a new
 * connection to create a new client thread (see comm.c)
 * 
 * Arguments: cxstr: stream connected to new client to be read from/written to
 * 
 */
void client_constructor(FILE *cxstr) {
    // You should create a new client_t struct here and initialize ALL
    // of its fields. Remember that these initializations should be
    // error-checked.
    //
    // TODO:
    // Step 1: Allocate memory for a new client and set its connection stream
    // to the input argument.
    // Step 2: Create the new client thread running the run_client routine.
    // Step 3: Detach the new client thread

    // create new client struct
    client_t *client = checked_malloc(sizeof(client_t));

    // initialize stream field
    client->cxstr = cxstr;
    client->prev = client;
    client->next = client;
    
    // create and detach new client thread
    checked_pthr_create(&client->thread, 0, run_client, client); // replace arg with something meaningful
    int err;
    if ((err = pthread_detach(client->thread))) {
        handle_error_en(err, "pthread_detach:");
    }
}

/*
 * client_destructor - takes in a client object and frees all resources
 * associated with it.
 * 
 * Arguments - client: pointer to the client object to be destroyed
 */
void client_destructor(client_t *client) {
    // TODO: Free and close all resources associated with a client.
    // Whatever was malloc'd in client_constructor should
    // be freed here!

    // close file
    comm_shutdown(client->cxstr);

    // free client
    free(client);
}

/*
 * run_client - function to be executed by client thread. If server is still
 * accepting clients, adds client to client list and then repeatedly calls 
 * comm_serve and intepret_command until client closes the connection.
 * 
 * Arguments: arg: pointer to the client struct that represents this thread's
 * client.
 */
void *run_client(void *arg) {
    // TODO:
    // Step 1: Make sure that the server is still accepting clients. This will
    //         will make sense when handling EOF for the server.
    // Step 2: Add client to the client list and push thread_cleanup to remove
    //       it if the thread is canceled.
    // Step 3: Loop comm_serve (in comm.c) to receive commands and output
    //       responses. Execute commands using interpret_command (in db.c)
    // Step 4: When the client is done sending commands, exit the thread
    //       cleanly.
    //
    // You will need to modify this when implementing functionality for stop and
    // go!

    client_t *client = (client_t *) arg;

    // check if server still accepting clients
    pthread_mutex_lock(&server_state.server_mutex);
    if (!accepting) {
        pthread_mutex_unlock(&server_state.server_mutex);
        client_destructor(client);
        pthread_exit(NULL); // TODO: modify to clean up nicely
    }

    // adding client to client list
    pthread_mutex_lock(&thread_list_mutex);

    if (thread_list_head != NULL) { // list is non-empty
        // update links
        client_t *next = thread_list_head;
        client_t *prev = thread_list_head->prev;
        
        // updating links for new node
        client->prev = prev;
        client->next = next;

        // updating links for old nodes
        next->prev = client;
        prev->next = client;
    }

    thread_list_head = client;
    // TODO: increment thread_num
    server_state.num_client_threads++;

    pthread_mutex_unlock(&thread_list_mutex);
    pthread_mutex_unlock(&server_state.server_mutex);

    // push cleanup handler
    pthread_cleanup_push(thread_cleanup, client);
    
    // Loop comm_serve
    char response[1024];
    memset(response, 0, 1024);
    char command[1024];
    memset(command, 0, 1024);
    while (1) {
        // wait on client cond if applicable
        client_control_wait();

        // read commands in
        if (comm_serve(client->cxstr, response, command) < 0) { // client closed
            break;
        }

        // attempts to interpret command, set a response
        interpret_command(command, response, 1024);
    }

    pthread_cleanup_pop(1);
    return NULL;
}

void delete_all() {
    // TODO: Cancel every thread in the client thread list with the
    // pthread_cancel function.

    client_t *current = thread_list_head;
    if (current == NULL) { // empty list case - nothing to do
        return;
    }

    client_t *next = thread_list_head->next;

    do {
        pthread_cancel(current->thread);
        current = next;
        next = current->next;
    } while (current != thread_list_head);
}

// Cleanup routine for client threads, called on cancels and exit.
void thread_cleanup(void *arg) {
    // TODO: Remove the client object from thread list and call
    // client_destructor. This function must be thread safe! The client must
    // be in the list before this routine is ever run.

    client_t *client = (client_t *) arg;

    pthread_mutex_lock(&thread_list_mutex);
    // get prev and next elts
    client_t *prev = client->prev;
    client_t *next = client->next;

    // if thread is current head of list, update head
    if (next == client) { // edge case - client is last element in list
        thread_list_head = NULL;
    } else {
        thread_list_head = next;
    }

    // close links
    next->prev = prev;
    prev->next = next;

    // destroy thread
    client_destructor(client);
    pthread_mutex_unlock(&thread_list_mutex);

    // TODO: decrement thread_num
    // check if 0 and then destroy database
    pthread_mutex_lock(&server_state.server_mutex);
    server_state.num_client_threads -= 1;
    if (server_state.num_client_threads == 0) {
        pthread_cond_broadcast(&server_state.server_cond);
    }
    pthread_mutex_unlock(&server_state.server_mutex);
}

// Code executed by the signal handler thread. For the purpose of this
// assignment, there are two reasonable ways to implement this.
// The one you choose will depend on logic in sig_handler_constructor.
// 'man 7 signal' and 'man sigwait' are both helpful for making this
// decision. One way or another, all of the server's client threads
// should terminate on SIGINT. The server (this includes the listener
// thread) should not, however, terminate on SIGINT!
void *monitor_signal(void *arg) {
    // TODO: Wait for a SIGINT to be sent to the server process and cancel
    // all client threads when one arrives.

    sig_handler_t *handler = (sig_handler_t *) arg;

    int sig, err;
    while (1) {
        if ((err = sigwait(&handler->set, &sig))) {
            handle_error_en(err, "sigwait:");
        }

        printf("SIGINT recieved, canceling all clients\n"); // TODO: remove

        pthread_mutex_lock(&thread_list_mutex);
        delete_all();
        pthread_mutex_unlock(&thread_list_mutex);
    }

    return NULL;
}

sig_handler_t *sig_handler_constructor() {
    // TODO: Create a thread to handle SIGINT. The thread that this function
    // creates should be the ONLY thread that ever responds to SIGINT.
    
    // initialize handler
    sig_handler_t *handler = checked_malloc(sizeof(sig_handler_t));
    
    // add SIGINT to handler set
    sigemptyset(&handler->set);
    sigaddset(&handler->set, SIGINT);

    // create thread
    checked_pthr_create(&handler->thread, 0, monitor_signal, handler);    

    return handler;
}

void sig_handler_destructor(sig_handler_t *sighandler) {
    // TODO: Free any resources allocated in sig_handler_constructor.
    // Cancel and join with the signal handler's thread.

    pthread_cancel(sighandler->thread);
    pthread_join(sighandler->thread, NULL);
    free(sighandler);
}

// The arguments to the server should be the port number.
int main(int argc, char *argv[]) {
    // TODO:
    // Step 1: Set up the signal handler for handling SIGINT.
    // Step 2: ignore SIGPIPE so that the server does not abort when a client
    // disocnnects 
    // Step 3: Start a listener thread for clients (see start_listener in
    //       comm.c).
    // Step 4: Loop for command line input and handle accordingly until EOF.
    // Step 5: Destroy the signal handler, delete all clients, cleanup the
    //       database, cancel and join with the listener thread
    //
    // You should ensure that the thread list is empty before cleaning up the
    // database and canceling the listener thread. Think carefully about what
    // happens in a call to delete_all() and ensure that there is no way for a
    // thread to add itself to the thread list after the server's final
    // delete_all().

    int port, rd_len;
    sigset_t set;
    pthread_t listener;
    char *buf, *filename;

    if (argc != 2) {
        printf("Usage: ./server <port>\n");
        exit(1);
    } else {
        port = atoi(argv[1]);
    }

    // ignore SIGINT and SIGPIPE in main thread
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGPIPE);

    pthread_sigmask(SIG_SETMASK, &set, NULL);

    // set up sigint handler thread
    sig_handler_t *handler = sig_handler_constructor();

    thread_list_head = NULL;

    // set up listener thread
    listener = start_listener(port, client_constructor);

    // start REPL
    buf = checked_malloc(1024);
    rd_len = 1;
    while (rd_len) { // exits if len = 0 (EOF read)
        memset(buf, 0, 1024);
        if ((rd_len = read(STDIN_FILENO, buf, 1024)) < 0) {
            perror("read:");
            free(buf);
            return -1;
        }

        switch (buf[0]) {
            case 's':
                // stop
                fprintf(stderr, "stopping all clients\n");
                
                client_control_stop();
                continue;
            
            case 'g':
                // go
                fprintf(stderr, "releasing all clients\n");
                
                client_control_release();
                continue;

            case 'p':
                //print
                filename = strtok((buf + 1), " \t");
                size_t len = strlen(filename); 
                
                // get rid of trailing newline
                if (filename[len - 1] == '\n') {
                    filename[len - 1] = '\0';
                }
                db_print(filename);
                continue;
        }
    }

    // wait on server condition variable
    pthread_mutex_lock(&server_state.server_mutex);
    
    printf("exiting database\n");
    accepting = 0;
    
    pthread_mutex_lock(&thread_list_mutex);
    
    delete_all();
    pthread_mutex_unlock(&thread_list_mutex);
    while(server_state.num_client_threads > 0) {
        pthread_cond_wait(&server_state.server_cond, &server_state.server_mutex);
    }

    // check thread list empty
    assert(server_state.num_client_threads == 0);
    assert(thread_list_head == NULL);

    // clean up db
    db_cleanup();
    
    // clean up other threads
    sig_handler_destructor(handler);
    pthread_cancel(listener);
    pthread_join(listener, NULL);

    pthread_mutex_unlock(&server_state.server_mutex);

    // clean up resources
    free(buf);
    return 0;
}
