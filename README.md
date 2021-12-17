# 8-database












Additional functions/modifications:

db.c:
search(): modified funciton signature to add parameter int write, which is a
boolean representing whether the lock type held on the parent is a read or a
write lock (to distinguish between querying and add/removing).

server.c:
checked_malloc(size_t size) - performs an error-checked malloc which prints
error and exits upon erroring.

checked_pthr_create(pthread_t *thread, const pthread_attr_t *attr, void *
(*start_routine)(void *), void *arg) - performs an error-checked pthread_create
with the given arguments