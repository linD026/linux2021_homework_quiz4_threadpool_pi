#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdbool.h>

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
    _Atomic(threadtask_t *) head;
    _Atomic(threadtask_t *) tail;
    _Atomic(threadtask_t *) free_list_head;
    _Atomic(threadtask_t *) free_list_tail;
    atomic_flag free_mode;
    atomic_flag head_stop;
    atomic_bool tail_stop;
    atomic_int free_list_num;
    atomic_int free_list_adding;
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
        threadtask_t *dummy = malloc(sizeof(threadtask_t));
        dummy->future = NULL;
        dummy->next = NULL;
        jobqueue->head = ATOMIC_VAR_INIT(dummy);
        jobqueue->tail = ATOMIC_VAR_INIT(dummy);

        dummy = malloc(sizeof(threadtask_t));
        dummy->future = NULL;
        dummy->next = NULL;
        jobqueue->free_list_head = ATOMIC_VAR_INIT(dummy);
        jobqueue->free_list_tail = ATOMIC_VAR_INIT(dummy);

        atomic_init(&jobqueue->free_list_num, 0);
        atomic_init(&jobqueue->free_list_adding, 0);

        atomic_flag_clear(&jobqueue->free_mode);
        atomic_flag_clear(&jobqueue->head_stop);
        atomic_init(&jobqueue->tail_stop, false);
        // jobqueue->cond_nonempty = ATOMIC_FLAG_INIT;
    }
    return jobqueue;
}

#define water_mark 50
static void jobqueue_free_list_through_and_clean(jobqueue_t *jobqueue, threadtask_t *new_tail) {
        //while (atomic_load(&jobqueue->tail_stop));
        //atomic_fetch_add(&jobqueue->free_list_adding, 1);
        new_tail->next = NULL;
        // atomic_store(&new_tail->next, NULL);

        while (1) {
            threadtask_t *old_tail = atomic_load(&jobqueue->free_list_tail);
            if (old_tail != NULL) {
                threadtask_t *dummy = NULL;
                if (atomic_compare_exchange_strong(&old_tail->next, &dummy, new_tail)) {
                    atomic_compare_exchange_strong(&jobqueue->free_list_tail, &old_tail, new_tail);
                    return;
                }    
            }
       }
}

static void jobqueue_free_list_clean(jobqueue_t *jobqueue) {
    atomic_fetch_add(&jobqueue->free_list_num, 1);
    atomic_fetch_sub(&jobqueue->free_list_adding, 1);

    // printf("num %d need > water mark %d\n", atomic_load(&jobqueue->free_list_num), water_mark);
    if (atomic_load(&jobqueue->free_list_num) > water_mark) {
        
        if (atomic_load(&jobqueue->tail_stop) == true)
            return;

        // atomic_thread_fence(memory_order_seq_cst);
        while (atomic_flag_test_and_set(&jobqueue->free_mode) == 1);

        // printf("free_list_num %d \n", atomic_load(&jobqueue->free_list_num));        
        if (atomic_load(&jobqueue->free_list_num) < water_mark) {
            atomic_flag_clear(&jobqueue->free_mode);
            return;
        }

        // block add into free list
        // atomic_store(&jobqueue->free_list_num, water_mark);
        atomic_store(&jobqueue->tail_stop, true);

        // waiting all work done
        // printf("free_list_adding: %d\n", atomic_load(&jobqueue->free_list_adding));
        while (atomic_load(&jobqueue->free_list_adding) > 0);
        // printf("in\n");
        // printf("free_list_adding: %d\n", atomic_load(&jobqueue->free_list_adding));

        threadtask_t *ptr = atomic_load(&jobqueue->free_list_head);
        threadtask_t *next = NULL;
        for (;ptr != atomic_load(&jobqueue->free_list_tail);) {
            next = ptr->next;
            free(ptr);
            ptr = next;
        }
        // atomic_store(&ptr->next, NULL);

        jobqueue->free_list_head = ATOMIC_VAR_INIT(ptr);
        // jobqueue->free_list_tail = ATOMIC_VAR_INIT(ptr);

        atomic_store(&jobqueue->free_list_num, 0);
        atomic_store(&jobqueue->tail_stop, false);
        atomic_flag_clear(&jobqueue->free_mode);
        // printf("out\n");
    }
}

static void jobqueue_destroy(jobqueue_t *jobqueue)
{
    while (atomic_flag_test_and_set(&jobqueue->head_stop) == 1);
    threadtask_t *tmp = atomic_load(&jobqueue->head);
    threadtask_t *ptr = atomic_load(&jobqueue->free_list_head);
    threadtask_t *next = NULL;
    for (;ptr;) {
        next = ptr->next;  
        // if (tmp == ptr) tmp = tmp->next;
        free(ptr);
        ptr = next;
    }

    while (tmp) {
        atomic_store(&jobqueue->head, tmp->next);
        if (tmp->future != NULL) {
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
        }
        free(tmp);
        tmp = atomic_load(&jobqueue->head);   
    }
    atomic_flag_clear(&jobqueue->head_stop);
    free(jobqueue);
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
    int old_type;


    while (1) {  
        pthread_testcancel();
        threadtask_t *base = NULL;
        /**
         * maybe the object base is freed, but task is goin to get the base->next;
         */
        
        /*
        while (atomic_flag_test_and_set(&jobqueue->head_stop) == 1);
        
        while (1) {
            base = atomic_load(&jobqueue->head);
            if (atomic_compare_exchange_strong(&jobqueue->head, &base, base)) {
                task = base->next;
                if (task != NULL) {
                    if (atomic_compare_exchange_strong(&jobqueue->head, &base, task)) {
                        jobqueue_free_list_through_and_clean(jobqueue, base);
                        break;                  
                    }                      
                }
            }
        }
        atomic_flag_clear(&jobqueue->head_stop);*/

        //while (atomic_flag_test_and_set(&jobqueue->head_stop) == 1);
        while (1) {
            while (atomic_flag_test_and_set(&jobqueue->head_stop) == 1);
            base = atomic_load(&jobqueue->head);
            task = atomic_load(&base->next);
            // atomic_flag_clear(&jobqueue->head_stop);
            if (task != NULL) {
                if (atomic_compare_exchange_strong(&jobqueue->head, &base, task)) {
                    while (atomic_load(&jobqueue->tail_stop));
                    atomic_fetch_add(&jobqueue->free_list_adding, 1);
                    atomic_flag_clear(&jobqueue->head_stop);
                    jobqueue_free_list_through_and_clean(jobqueue, base);
                    break;                  
                }                      
            }
            atomic_flag_clear(&jobqueue->head_stop);
        }
        //atomic_flag_clear(&jobqueue->head_stop);


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
                // free(task);
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
            // pthread_cleanup_pop(0);



            pthread_mutex_lock(&task->future->mutex);
            task->future->flag |= __FUTURE_FINISHED; // KKK
            task->future->result = ret_value;
            pthread_cond_broadcast(&task->future->cond_finished);
            pthread_mutex_unlock(&task->future->mutex);
            // free(task);
            // task = NULL;
            pthread_cleanup_pop(0);
            // printf("num %d need > water mark %d\n", atomic_load(&jobqueue->free_list_num), water_mark);
 
            atomic_thread_fence(memory_order_seq_cst);

            jobqueue_free_list_clean(jobqueue);

            /***********************************************/

        } else {
            // function work failed
            /*
            pthread_mutex_destroy(&task->future->mutex);
            pthread_cond_destroy(&task->future->cond_finished);
            free(task->future);
            free(task);
            */
            // printf("get stop\n");
            // tpool_future_destroy(task->future);
            
            // atomic_thread_fence(memory_order_seq_cst);

            jobqueue_free_list_clean(jobqueue);
            break;
        }
    }
    // printf("thread exit\n");
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
    threadtask_t *new_tail = malloc(sizeof(threadtask_t));
    struct __tpool_future *future = NULL;
    /**
     * dummy input to stop but don't need future
     */
    int det = 1;
    if (func) {
        future = tpool_future_create();
        if (!future) det = 0;
    }

    if (new_tail && det) {
        new_tail->func = func, new_tail->arg = arg, new_tail->future = future;
        new_tail->next = NULL;

        atomic_thread_fence(memory_order_seq_cst);

       while (1) {
            threadtask_t *old_tail = atomic_load(&jobqueue->tail);
            // old_tail->next = NULL;
            if (old_tail != NULL) {
                threadtask_t *dummy = NULL;
                if (atomic_compare_exchange_strong(&old_tail->next, &dummy, new_tail)) {
                    atomic_compare_exchange_strong(&jobqueue->tail, &old_tail, new_tail);
                    break;
                }    
            }
            // printf("CAS fail\n");
       }

    } else if (new_tail) {
        free(new_tail);
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
    // printf("not exit\n");
    for (int i = 0; i < num_threads; i++)
        pthread_join(pool->workers[i], NULL);
    free(pool->workers);
    // printf("free destroy\n");
    jobqueue_destroy(pool->jobqueue);
    free(pool);
    return 0;
}


void test_func(struct __threadpool *pool) {
    threadtask_t *ptr = pool->jobqueue->head;
    for (;ptr;ptr = ptr->next)
        printf("task %d\n", *(int *)ptr->arg);
}