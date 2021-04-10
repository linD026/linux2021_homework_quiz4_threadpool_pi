#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#include "thread_pi.h"

#define PRECISION 100 /* upper bound in BPP sum */
void test_dummy(void);
void test1 (void);

/* Use Bailey–Borwein–Plouffe formula to approximate PI */
static void *bpp(void *arg)
{
    int k = *(int *) arg;
    double sum = (4.0 / (8 * k + 1)) - (2.0 / (8 * k + 4)) -
                 (1.0 / (8 * k + 5)) - (1.0 / (8 * k + 6));
    double *product = malloc(sizeof(double));
    if (product)
        *product = 1 / pow(16, k) * sum;
    return (void *) product;
}

static void *dummy(void *arg) {
    // srandom(time(NULL));
    // sleep( random() % 10);
    // sleep(10);
    double *product = malloc(sizeof(double));
    printf("dummy %d\n", *(int*)arg);
    // printf("pid is %ld\n", pthread_self());
    *product = 1;
    return (void *) product;
}


#define task_n 100
#define wait_t 0
int main()
{
    test1();
    return 0;
}

void test_dummy(void) {
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
        double *result = tpool_future_get(pool, futures[i], wait_t);
        if (result != NULL) {
            sum += *result;
            tpool_future_destroy(futures[i]);
            free(result);
        }
    }

    tpool_join(pool);
    printf("sum %d\n", sum);
}

void test1 (void) {
    int bpp_args[PRECISION + 1];
    double bpp_sum = 0;
    // create the thread and each thread loop for fetch work. (empty then wait)
    tpool_t pool = tpool_create(4);
    tpool_future_t futures[PRECISION + 1];


    // put task in the thread
    for (int i = 0; i <= PRECISION; i++) {
        bpp_args[i] = i;
        futures[i] = tpool_apply(pool, bpp, (void *) &bpp_args[i]);
    }

    // get result
    for (int i = 0; i <= PRECISION; i++) {
        double *result = tpool_future_get(pool, futures[i], 0 /* blocking wait */);
        bpp_sum += *result;
        tpool_future_destroy(futures[i]);
        free(result);
    }

    // printf("thread done\n");
    tpool_join(pool);
    printf("PI calculated with %d terms: %.15f\n", PRECISION + 1, bpp_sum);  
}



void t1(void *arg) {
	printf("t1 is running on thread \n");
}
void t2(void *arg) {
	printf("t2 is running on thread \n");
}
void t3(void *arg) {
	printf("t3 is running on thread \n");
}
void t4(void *arg) {
	printf("t4 is running on thread \n");
}

void test_atomic_cmp(void) {
    tpool_t pool = tpool_create(8);
    tpool_future_t future[4];
    for (int i = 0;i < task_n;i++) {
        future[0] = tpool_apply(pool, (void* )t1, NULL);
        future[1] = tpool_apply(pool, (void* )t2, NULL);
        future[2] = tpool_apply(pool, (void* )t3, NULL);
        future[3] = tpool_apply(pool, (void* )t4, NULL);
        for (int i = 0;i < 4;i++) {
            tpool_future_get(pool, future[i], 0);
            tpool_future_destroy(future[i]);
        }
    }

    sleep(1);

    tpool_join(pool);
}