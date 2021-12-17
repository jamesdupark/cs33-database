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


/* 
 * client_control_wait - called by client threads in run_client each loop.
 * checks whether the server has stopped client thread execution, and blocks the
 * thread on the client control's conditional variable if it has. Waits until
 * the "go" signal is sent by client_control_release
 */
void client_control_wait() {
    pthread_mutex_lock(&stop_go.go_mutex);
    pthread_cleanup_push((void (*)(void *)) pthread_mutex_unlock, &stop_go.go_mutex);
    while (stop_go.stopped) {
        pthread_cond_wait(&stop_go.go, &stop_go.go_mutex);
    }

    pthread_cleanup_pop(1);
}

/*
 * client_control_stop - called when "s" is read in from STDIN in the server
 * REPL. Pauses but does not terminate execution of all client threads by
 * setting the client control struct's stopped flag to 1.
 */
void client_control_stop() {
    pthread_mutex_lock(&stop_go.go_mutex);
    stop_go.stopped = 1;
    pthread_mutex_unlock(&stop_go.go_mutex);
}

/*
 * client_control_release - called when "g" is read from STDIN in the server
 * REPL. Resumes execution of all client threads by setting the stopped flag to
 * 0 and broadcasting on the client control struct's cond variable.
 */
void client_control_release() {
    pthread_mutex_lock(&stop_go.go_mutex);
    stop_go.stopped = 0;
    pthread_cond_broadcast(&stop_go.go);
    pthread_mutex_unlock(&stop_go.go_mutex);
}

/*
 * client_constructor - called by listener thread upon recieving a new
 * connection to create a new client thread (see comm.c)
 * 
 * Arguments: cxstr: stream connected to new client to be read from/written to
 * 
 */
void client_constructor(FILE *cxstr) {
    // create new client struct
    client_t *client = checked_malloc(sizeof(client_t));

    // initialize stream field
    client->cxstr = cxstr;
    client->prev = client;
    client->next = client;
    
    // create and detach new client thread
    checked_pthr_create(&client->thread, 0, run_client, client);
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
    // cast arg
    client_t *client = (client_t *) arg;

    // check if server still accepting clients
    pthread_mutex_lock(&server_state.server_mutex);
    if (!accepting) {
        pthread_mutex_unlock(&server_state.server_mutex);

        // we don't call thread_cleanup because thread isn't in list
        client_destructor(client);
        pthread_exit(NULL);
    }

    // adding client to client list
    pthread_mutex_lock(&thread_list_mutex);

    if (thread_list_head != NULL) { // list is non-empty
        // get links to update
        client_t *next = thread_list_head;
        client_t *prev = thread_list_head->prev;
        
        // updating links for new node
        client->prev = prev;
        client->next = next;

        // updating links for old nodes
        next->prev = client;
        prev->next = client;
    }

    // update head (covers both empty and non-empty list cases)
    thread_list_head = client;
    
    // increment thread_num
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
        if (comm_serve(client->cxstr, response, command) < 0) {
            // exit loop if client terminated
            break;
        }

        // attempts to interpret command, set a response
        interpret_command(command, response, 1024);
    }

    pthread_cleanup_pop(1);
    return NULL;
}

/*
 * delete_all - loops through the thread list and calls pthread_cancel on each
 * thread. Must be called with thread_list_mutex locked.
 */
void delete_all() {
    client_t *current = thread_list_head;
    
    // empty list case - nothing to do
    if (current == NULL) {
        return;
    }

    // get next node
    client_t *next = thread_list_head->next;

    do {
        pthread_cancel(current->thread);
        current = next;
        next = current->next;
    } while (current != thread_list_head);
}

/*
 * thread_cleanup - cleanup handler for client threads - removes client from
 * thread_list and destroys client. Client must be in thread_list when this
 * function is called.
 * 
 * Arguments - arg: pointer to client_t struct to remove and destroy.
 */
void thread_cleanup(void *arg) {

    client_t *client = (client_t *) arg;

    pthread_mutex_lock(&thread_list_mutex);
    // get prev and next elts
    client_t *prev = client->prev;
    client_t *next = client->next;

    // update head
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

    // decrement thread_num
    pthread_mutex_lock(&server_state.server_mutex);
    server_state.num_client_threads -= 1;
    
    // resumes shutdown if thread_num = 0 and server is waiting on cond variable
    if (server_state.num_client_threads == 0) {
        pthread_cond_broadcast(&server_state.server_cond);
    }
    pthread_mutex_unlock(&server_state.server_mutex);
}

/*
 * monitor_signal - function executed by signal handler thread to catch and
 * process SIGINT. Waits for SIGINt and then cancels all current threads with
 * delete_all
 * 
 * Arguments: arg: pointer to sighandler_t struct containing the set of signals
 * (just SIGINT in this case) to wait for
 */
void *monitor_signal(void *arg) {
    sig_handler_t *handler = (sig_handler_t *) arg;

    int sig, err;
    
    // wait for SIGINTs
    while (1) {
        if ((err = sigwait(&handler->set, &sig))) {
            handle_error_en(err, "sigwait:");
        }

        printf("SIGINT recieved, canceling all clients\n");

        pthread_mutex_lock(&thread_list_mutex);
        delete_all();
        pthread_mutex_unlock(&thread_list_mutex);
    }

    return NULL;
}

/*
 * sig_handler_constructor - function that creates the signal handler thread.
 * initializes the handler struct and creates a thread calling monitor_signal.
 * 
 * Returns: pointer to the sig_handler_t struct containing the signal handler
 * thread
 */
sig_handler_t *sig_handler_constructor() {
    // initialize handler
    sig_handler_t *handler = checked_malloc(sizeof(sig_handler_t));
    
    // add SIGINT to handler set
    sigemptyset(&handler->set);
    sigaddset(&handler->set, SIGINT);

    // create thread
    checked_pthr_create(&handler->thread, 0, monitor_signal, handler);    

    return handler;
}

/*
 * sig_handler_destructor - frees the signal handler thread and any resources
 * associated with its corresponding struct
 * 
 * Arguments: sighandler: pointer to the sig_handler_t struct containing the
 * signal handler thread, previously returned by a call to 
 * sig_handler_constructor
 */
void sig_handler_destructor(sig_handler_t *sighandler) {
    pthread_cancel(sighandler->thread);
    pthread_join(sighandler->thread, NULL);
    free(sighandler);
}

// The arguments to the server should be the port number.
/*
 * main - runs the server. Creates listener and signal handler threads which
 * listen for incoming connections on the given port and monitor for SIGINTs, 
 * respectively. Then starts server REPL, which executes "s" (stop), "g" (go),
 * and "p" (print) commands until EOF (ctrl+D) is read in, at which point the
 * server stops accepting new connections and cleans up itself and the database.
 * 
 * Arguments - argc: number of args, argv: command-line arguments - should
 * number two, with the second being the port number to listen on (should be
 * greater than 1023).
 * 
 * Returns - exit code for the process
 * 
 * Usage - ./server <port>
 *          REPL commands:
 *              - s: stops execution of all client threads
 *              - g: resumes execution of all client threads
 *              - p <file>: print representation of current state of database
 *                          to the given file. If no file is provided, prints to
 *                          STDOUT
 */
int main(int argc, char *argv[]) {

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

        // execute commands
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
