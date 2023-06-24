#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/* 任务 */
typedef struct threadpool_task_s {
    void *(*function)(void *arg);       /* 任务函数 */
    void *arg;                          /* 函数参数 */
} threadpool_task_t;

/* 线程池结构 */
typedef struct threadpool_s {
    pthread_mutex_t lock;               /* 整个线程池的锁 */
    pthread_mutex_t thread_counter;     /* 用于使用忙状态的线程数的锁 */
    pthread_cond_t queue_not_full;      /* 任务队列不为满 */
    pthread_cond_t queue_not_empty;     /* 任务队列不为空 */

    pthread_t *threads;                 /* 任务线程队列 */
    pthread_t admin_tid;               /* 管理线程队列 */
    threadpool_task_t *task_queue;      /* 任务队列 */
    /* 线程池信息 */
    size_t min_thrd_num;                /* 最少线程数 */
    size_t max_thrd_num;                /* 最多线程数 */
    size_t live_thrd_num;               /* 线程池中的存活线程数 */
    size_t busy_thrd_num;               /* 忙状态线程，正在运行的线程 */
    size_t wait_exit_thrd_num;          /* 需要销毁的线程 */
    /* 任务队列信息 */
    size_t queue_front;                 /* 队头 */
    size_t queue_rear;                  /* 队尾 */
    size_t queue_size;                  /* 存在的线程数 */
    size_t queue_capacity;              /* 线程队列的容量 */
    /* 线程池状态 */
    int shutdown;                       /* 是否关闭线程池 */
} threadpool_t ;

/**
 * @brief create thread pool
 * @param min_thrd_num
 * @param max_thrd_num
 * @param queuq_capacity;
 * @return thread pool ponter
 */
extern threadpool_t *threadpool_create(size_t min_thrd_num, size_t max_thrd_num, size_t queue_capacity);

/**
 * @brief add a task to work qeueue tail
 */
extern int threadpool_add(threadpool_t *pool, void *(*function)(void *arg), void *arg);

/**
 * @brief destory thread pool
 */
extern int threadpool_destroy(threadpool_t *pool);


#endif /* THREAD_POOL_H */
