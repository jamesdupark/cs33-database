# 8-database

## Project description:

Implements a multithreaded TCP server which handles connections from clients to
add to, delete from, and query a binary tree database of key-vaue pairs in a 
thread-safe manner. Server supports REPL commands such as "s" (stop), "g" (go), 
and "p <file>" (print) and exits upon recieving EOF. A signal handler thread is
used to catch SIGINTs and terminate all client threads upon reciept, but not the
server threads.


## Project structure:
  
**comm.c/.h:** contains basic utilities for communicating via TCP protocol. Code to
be executed by listener thread, which monitors the given port for incoming
client connections and creates client threads accordingly.

**client.c:** contains code for TCP clients.

**db.c/.h:** contains code for querying, adding to, deleting from, printing, and
cleaning up database in a thread-safe manner. Uses fine-grained locking.

**server.c:** contains code for setting up server, listener, and signal handler
threads, as well as starting and managing client threads.


### Additional functions/modifications:
**db.c:**  
*search():* modified funciton signature to add parameter int write, which is a
boolean representing whether the lock type held on the parent is a read or a
write lock (to distinguish between querying and add/removing).

**server.c:**  
*checked_malloc(size_t size):* performs an error-checked malloc which prints
error and exits upon erroring.

*checked_pthr_create(pthread_t *thread, const pthread_attr_t *attr, void *
(*start_routine)(void *), void *arg):* performs an error-checked pthread_create
with the given arguments

**Known bugs:**  
None. Compiling with Google's thread sanitizer sometimes causes crashes when
concurrently deleting and printing from database, but this does not affect
functionality (the thread sanitizer crashes).
