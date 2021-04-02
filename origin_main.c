#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#include "original.h"

static void *dummy(void *arg) {
    // srandom(time(NULL));
    // sleep( random() % 10);
    sleep(10);
    double *product = malloc(sizeof(double));
    printf("dummy %d\n", *(int*)arg);
    // printf("pid is %ld\n", pthread_self());
    *product = 1;
    return (void *) product;
}

#define task_n 8
#define wait_t 1
int main()
{
    // create the thread and each thread loop for fetch work. (empty then wait)
    tpool_t pool = tpool_create(4);
    tpool_future_t futures[task_n];
    int temp[task_n] = {0};


    // put task in the thread
    for (int i = 0; i < task_n; i++) {
        temp[i] = i;
        futures[i] = tpool_apply(pool, dummy, (void *) &temp[i]);
    }

    // get result
    int sum = 0;
    for (int i = 0; i < task_n; i++) {
        double *result = tpool_future_get(futures[i], wait_t);
        if (result != NULL) {
            sum += *result;
            tpool_future_destroy(futures[i]);
            free(result);
        }
    }

    tpool_join(pool);
    printf("sum %d\n", sum);
    return 0;
}
