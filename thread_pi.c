#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>


#include "thread_pi.h"

enum __future_flags {
    __FUTURE_RUNNING = 01,      /*0000 0001*/
    __FUTURE_FINISHED = 02,     /*0000 0010*/
    __FUTURE_TIMEOUT = 04,      /*0000 0100*/
    __FUTURE_CANCELLED = 010,   /*0000 1010*/
    __FUTURE_DESTROYED = 020,   /*0001 0100*/
};

typedef struct __threadtask {
    void *(*func)(void *);
    void *arg;
    struct __tpool_future *future;
    struct __threadtask *next;
} threadtask_t;

typedef struct __jobqueue {
    threadtask_t *head, *tail;
    pthread_cond_t cond_nonempty;
    pthread_mutex_t rwlock;
} jobqueue_t;

struct __tpool_future {
    int flag;
    void *result;
    pthread_t pid;
    pthread_mutex_t mutex;
    pthread_cond_t cond_finished;
};

struct __threadpool {
    size_t count;
    pthread_t *workers;
    jobqueue_t *jobqueue;
};

static struct __tpool_future *tpool_future_create(void)
{
    struct __tpool_future *future = malloc(sizeof(struct __tpool_future));
    if (future) {
        future->flag = 0;
        future->result = NULL;
        pthread_mutex_init(&future->mutex, NULL);
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        pthread_cond_init(&future->cond_finished, &attr);
        pthread_condattr_destroy(&attr);
    }
    return future;
}

int tpool_future_destroy(struct __tpool_future *future)
{
    if (future) {
        pthread_mutex_lock(&future->mutex);
        if (future->flag & __FUTURE_FINISHED ||
            future->flag & __FUTURE_CANCELLED) {
            pthread_mutex_unlock(&future->mutex);
            pthread_mutex_destroy(&future->mutex);
            pthread_cond_destroy(&future->cond_finished);
            free(future);
        } else {
            future->flag |= __FUTURE_DESTROYED;
            pthread_mutex_unlock(&future->mutex);
        }
    }
    return 0;
}

/**
 * CONTROL TASK TIMELIMIT
 */
static void *jobqueue_fetch(void *queue);
void *tpool_future_get(struct __threadpool *pool, struct __tpool_future *future, unsigned int seconds)
{
    pthread_mutex_lock(&future->mutex);
    /* turn off the timeout bit set previously */
    future->flag &= ~__FUTURE_TIMEOUT;
    while ((future->flag & __FUTURE_FINISHED) == 0) {
        if (seconds) {
            struct timespec expire_time;
            // clock_gettime(CLOCK_MONOTONIC, &expire_time);            
            clock_gettime(CLOCK_REALTIME, &expire_time);
            expire_time.tv_sec += seconds;
            int status = pthread_cond_timedwait(&future->cond_finished,
                                                &future->mutex, &expire_time);
            // printf("status %d(110 == ETIMEDOUT)\n", status);
            if (status == ETIMEDOUT) {
                future->flag |= __FUTURE_TIMEOUT;
                pthread_t pid = future->pid;
                pthread_mutex_unlock(&future->mutex);
                int cancel_status = pthread_cancel(future->pid);
                if (cancel_status != ESRCH) {
                    printf("canceled\n");
                    future = NULL;
                    int i;
                    for (i = 0;i < pool->count;i++)
                        if (pool->workers[i] == pid) {
                            printf("find out %d with pid %ld\n", i, pid);
                            break;
                        }
                    pthread_join(pid, NULL);
                    pthread_create(&pool->workers[i], NULL, jobqueue_fetch,
                               (void *) pool->jobqueue);
                }
                else {
                    pthread_mutex_unlock(&future->mutex);
                    printf("cancel failed\n");
                }
                return NULL;
            }
        } else
            pthread_cond_wait(&future->cond_finished, &future->mutex);
            //FFF
    }

    pthread_mutex_unlock(&future->mutex);
    return future->result;
}


static jobqueue_t *jobqueue_create(void)
{
    jobqueue_t *jobqueue = malloc(sizeof(jobqueue_t));
    if (jobqueue) {
        jobqueue->head = jobqueue->tail = NULL;
        pthread_cond_init(&jobqueue->cond_nonempty, NULL);
        pthread_mutex_init(&jobqueue->rwlock, NULL);
    }
    return jobqueue;
}

static void jobqueue_destroy(jobqueue_t *jobqueue)
{
    threadtask_t *tmp = jobqueue->head;
    while (tmp) {
        jobqueue->head = jobqueue->head->next;
        pthread_mutex_lock(&tmp->future->mutex);
        if (tmp->future->flag & __FUTURE_DESTROYED) {
            pthread_mutex_unlock(&tmp->future->mutex);
            pthread_mutex_destroy(&tmp->future->mutex);
            pthread_cond_destroy(&tmp->future->cond_finished);
            free(tmp->future);
        } else {
            tmp->future->flag |= __FUTURE_CANCELLED;
            pthread_mutex_unlock(&tmp->future->mutex);
        }
        free(tmp);
        tmp = jobqueue->head;
    }

    pthread_mutex_destroy(&jobqueue->rwlock);
    pthread_cond_destroy(&jobqueue->cond_nonempty);
    free(jobqueue);
}

static void __jobqueue_fetch_cleanup(void *arg)
{
    pthread_mutex_t *mutex = (pthread_mutex_t *) arg;
    pthread_mutex_unlock(mutex);
}

static void __task_cleanup(void *arg) {
    threadtask_t *task = (threadtask_t *) arg;
    pthread_mutex_unlock(&task->future->mutex);
    pthread_mutex_destroy(&task->future->mutex);
    pthread_cond_destroy(&task->future->cond_finished);
    free(task->future->result);
    free(task->future);
    free(task);
}

static void *jobqueue_fetch(void *queue)
{
    jobqueue_t *jobqueue = (jobqueue_t *) queue;
    threadtask_t *task;
    // int old_state;
    int old_type;

    pthread_cleanup_push(__jobqueue_fetch_cleanup, (void *) &jobqueue->rwlock);

    while (1) {
        pthread_mutex_lock(&jobqueue->rwlock);
        // pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &old_state);
        pthread_testcancel();
        
        /**
         * Fetch job from jobqueue
         */
        // GGG
        while (!jobqueue->tail)
            pthread_cond_wait(&jobqueue->cond_nonempty, &jobqueue->rwlock);
        // pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_state);

        if (jobqueue->head == jobqueue->tail) {
            task = jobqueue->tail;
            jobqueue->head = jobqueue->tail = NULL;
        } else {
            /*origin
            threadtask_t *tmp;
            for (tmp = jobqueue->head; tmp->next != jobqueue->tail;
                 tmp = tmp->next)
                ;
            task = tmp->next;
            tmp->next = NULL;
            jobqueue->tail = tmp;
            //*/

           ///* ALTER
            task = jobqueue->head;
            jobqueue->head = task->next;
           //*/
        }
        pthread_mutex_unlock(&jobqueue->rwlock);

        /**
         * Start working (fetched the work)
         */
        if (task->func) {
            /**
             * working or not, check the state (cancelled or running)
             */
            /***********************************************/
            pthread_mutex_lock(&task->future->mutex);
            // task cancelled
            if (task->future->flag & __FUTURE_CANCELLED) {
                pthread_mutex_unlock(&task->future->mutex);
                free(task);
                continue;
            } else {
            // task running
                task->future->flag |= __FUTURE_RUNNING;
                task->future->pid = pthread_self();
                pthread_mutex_unlock(&task->future->mutex);
            }
            /***********************************************/

            void *ret_value = NULL;
            pthread_cleanup_push(__task_cleanup, task);
            pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &old_type);
            // FUNCTION START
            ret_value = task->func(task->arg);
            pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &old_type);
            pthread_cleanup_pop(0);

            // work continue
            pthread_mutex_lock(&task->future->mutex);
            /*if (task->future->flag & __FUTURE_DESTROYED) {
                printf("cancel\n");
                pthread_mutex_unlock(&task->future->mutex);
                pthread_mutex_destroy(&task->future->mutex);
                pthread_cond_destroy(&task->future->cond_finished);
                free(task->future);
            } else {*/
                printf("finished\n");
                task->future->flag |= __FUTURE_FINISHED; // KKK
                task->future->result = ret_value;
                // LLL
                pthread_cond_broadcast(&task->future->cond_finished);
                pthread_mutex_unlock(&task->future->mutex);
            // }
            free(task);
            /***********************************************/

        } else {
            // function work failed
            pthread_mutex_destroy(&task->future->mutex);
            pthread_cond_destroy(&task->future->cond_finished);
            free(task->future);
            free(task);
            break;
        }
    }

    pthread_cleanup_pop(0);
    pthread_exit(NULL);
}

struct __threadpool *tpool_create(size_t count)
{
    jobqueue_t *jobqueue = jobqueue_create();
    struct __threadpool *pool = malloc(sizeof(struct __threadpool));
    if (!jobqueue || !pool) {
        if (jobqueue)
            jobqueue_destroy(jobqueue);
        free(pool);
        return NULL;
    }

    pool->count = count, pool->jobqueue = jobqueue;
    if ((pool->workers = malloc(count * sizeof(pthread_t)))) {
        for (int i = 0; i < count; i++) {
            if (pthread_create(&pool->workers[i], NULL, jobqueue_fetch,
                               (void *) jobqueue)) {
                for (int j = 0; j < i; j++)
                    pthread_cancel(pool->workers[j]);
                for (int j = 0; j < i; j++)
                    pthread_join(pool->workers[j], NULL);
                free(pool->workers);
                jobqueue_destroy(jobqueue);
                free(pool);
                return NULL;
            }
        }
        return pool;
    }

    jobqueue_destroy(jobqueue);
    free(pool);
    return NULL;
}

struct __tpool_future *tpool_apply(struct __threadpool *pool,
                                   void *(*func)(void *),
                                   void *arg)
{
    jobqueue_t *jobqueue = pool->jobqueue;
    threadtask_t *new_head = malloc(sizeof(threadtask_t));
    struct __tpool_future *future = tpool_future_create();
    if (new_head && future) {
        new_head->func = func, new_head->arg = arg, new_head->future = future;
        pthread_mutex_lock(&jobqueue->rwlock);
        if (jobqueue->head) {
            /*origin
            new_head->next = jobqueue->head;
            jobqueue->head = new_head;
            //*/
            
            ///* ALTER
            jobqueue->tail->next = new_head;
            jobqueue->tail = new_head;
            //*/
        } else {
            jobqueue->head = jobqueue->tail = new_head;
            // HHH
            pthread_cond_broadcast(&jobqueue->cond_nonempty);
        }
        pthread_mutex_unlock(&jobqueue->rwlock);
    } else if (new_head) {
        free(new_head);
        return NULL;
    } else if (future) {
        tpool_future_destroy(future);
        return NULL;
    }
    return future;
}

int tpool_join(struct __threadpool *pool)
{
    size_t num_threads = pool->count;
    for (int i = 0; i < num_threads; i++)
        tpool_apply(pool, NULL, NULL);
    for (int i = 0; i < num_threads; i++)
        pthread_join(pool->workers[i], NULL);
    free(pool->workers);
    jobqueue_destroy(pool->jobqueue);
    free(pool);
    return 0;
}