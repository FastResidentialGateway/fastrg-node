#ifndef _CONTROLLER_H_
#define _CONTROLLER_H_

#include <pthread.h>

#include <common.h>

struct FastRG;

/* Controller heartbeat thread state */
typedef struct controller_heartbeat {
    pthread_t               thread;         /* joinable heartbeat thread */
    BOOL                    started;        /* TRUE once the thread is created */
    pthread_mutex_t         lock;           /* a lock for guarding stop_requested */
    pthread_cond_t          cond;           /* signalled on stop to wake the thread early */
    BOOL                    stop_requested; /* set by controller_cleanup to end the thread */
} controller_heartbeat_t;

/* Controller initialization and cleanup */
int controller_init(struct FastRG *fastrg_ccb);
void controller_cleanup(struct FastRG *fastrg_ccb);

/* Controller node registration */
int controller_register_this_node(struct FastRG *fastrg_ccb);

#endif /* _CONTROLLER_H_ */
