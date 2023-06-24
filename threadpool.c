#include <errno.h>
// #include <locale.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "threadpool.h"

static const int DEFAULT_TIME = 10;         /* check internal time */
static const int MIN_WAIT_TASK_NUM = 10;    /*  */
static const int DEFAULT_THREAD_VARY = 3;   /*  */

enum BOOLEAN {
    FALSE = 0,
    TRUE = 1,
};

/**
 * @brief 工作队列线程
 */
static void *threadpool_thread(void *thrdpool);
/**
 * @brief 管理线程
 */
static void *admin_thread(void *thrdpool);
/**
 * @brief 释放线程池资源
 */
static int threadpool_free(threadpool_t **pool);
/**
 * @brief 检查线程是否存活
 */
static int is_thread_alive(pthread_t tid);

threadpool_t *threadpool_create(size_t min_thrd_num, size_t max_thrd_num, size_t queue_capacity)
{
    threadpool_t *pool = NULL;

    pool = calloc(1, sizeof(threadpool_t));
    if (!pool) {
        printf("Allocate object failed! errno: %d, %s\n", errno, strerror(errno));
        return NULL;
    }

    pool->min_thrd_num = min_thrd_num;
    pool->max_thrd_num = max_thrd_num;
    pool->busy_thrd_num = 0;
    pool->live_thrd_num = min_thrd_num;
    pool->wait_exit_thrd_num = 0;

    pool->queue_size = 0;
    pool->queue_capacity = queue_capacity;
    pool->queue_front = 0;
    pool->queue_rear = 0;
    pool->shutdown = FALSE;

    pool->threads = calloc(max_thrd_num, sizeof(pthread_t));
    if (!pool->threads) {
        printf("Allocate thread queue failed! errno: %d, %s\n", errno, strerror(errno));
        goto threadpool_create_err;
    }

    pool->task_queue = calloc(queue_capacity, sizeof(threadpool_task_t));
    if (!pool->task_queue) {
        printf("Allocate task queue failed! errno: %d, %s\n", errno, strerror(errno));
        goto threadpool_create_err;
    }

    if (!pthread_mutex_init(&pool->lock, NULL) || !pthread_mutex_init(&pool->thread_counter, NULL)
        || !pthread_cond_init(&pool->queue_not_empty, NULL) || !pthread_cond_init(&pool->queue_not_full, NULL)) {
            printf("thread syncinron initialize failed!\n");
            goto threadpool_create_err;
    }

    for (size_t i = 0; i < min_thrd_num; ++i) {
        pthread_create(&pool->threads[i], NULL, threadpool_thread, pool);
    }

    pthread_create(&pool->admin_tid, NULL, admin_thread, pool);
    pthread_detach(pool->admin_tid);

    sleep(1);

    return pool;

threadpool_create_err:
    threadpool_free(&pool);
    return pool;
}

int threadpool_add(threadpool_t *pool, void *(*function)(void *arg), void *arg)
{
	pthread_mutex_lock(&pool->lock);

	/* ==为真，队列已经满， 调wait阻塞 */
	while (pool->queue_size == pool->queue_capacity && !pool->shutdown) {
		pthread_cond_wait(&pool->queue_not_full, &pool->lock);
	}

	if (pool->shutdown) {
		pthread_mutex_unlock(&pool->lock);
	}

	/* 清空 工作线程 调用的回调函数 的参数arg */
	if (pool->task_queue[pool->queue_rear].arg) {
		free(pool->task_queue[pool->queue_rear].arg);//这里调用free不安全，因为有可能线程执行函数中, 忙符合时低概率
		pool->task_queue[pool->queue_rear].arg = NULL;
	}

	/* 添加任务到任务队列里 */
	pool->task_queue[pool->queue_rear].function = function; //在队列的尾部添加元素
	pool->task_queue[pool->queue_rear].arg = arg;
	pool->queue_rear = (pool->queue_rear + 1) % pool->queue_capacity; /* 队尾指针移动, 模拟环形 */
	++pool->queue_size;

	/* 添加完任务后，队列不为空，唤醒阻塞在为空的那个条件变量上中的线程 */
	pthread_cond_signal(&pool->queue_not_empty);
	pthread_mutex_unlock(&pool->lock);

	return 0;
}

int threadpool_destroy(threadpool_t *pool)
{
	if (pool == NULL) {
		return -1;
	}

	pool->shutdown = TRUE;

	/* 通知所有的空闲线程, 不需要循环广播 */
	pthread_cond_broadcast(&pool->queue_not_empty);
	for (int i = 0; i < pool->live_thrd_num; i++) {
		pthread_join(pool->threads[i], NULL);
	}
	threadpool_free(&pool);

	return 0;
}

void *threadpool_thread(void *thrdpool)
{
    threadpool_t *pool = thrdpool;
    threadpool_task_t task = {
        .function = NULL,
        .arg = NULL,
    };

    if (!thrdpool) {
        printf("Invalide argument!\n");
        return NULL;
    }

    while(TRUE) {
		/* 刚创建出线程，等待任务队列里 有任务，否则阻塞等待任务队列里有任务后再唤醒接收任务 */
		pthread_mutex_lock(&pool->lock);

		/* queue_size == 0 说明没有任务，调 wait 阻塞在条件变量上, 若有任务，跳过该while */
        // 线程池没有任务且不关闭线程池。
		while(pool->queue_size == 0 && !pool->shutdown) {
			printf("thread %llu is waiting\n", pthread_self());
			pthread_cond_wait(&(pool->queue_not_empty), &(pool->lock));//线程阻塞在这个条件变量上

			/* 清除指定数目的空闲线程，如果要结束的线程个数大于0，结束线程 */
            /* 要销毁的线程个数大于0 */
			if(pool->wait_exit_thrd_num > 0) {
				--pool->wait_exit_thrd_num;

				/* 如果线程池里线程个数大于最小值时可以结束当前线程 */
				if (pool->live_thrd_num > pool->min_thrd_num) {
					printf("thread %llu is exiting\n", pthread_self());
					--pool->live_thrd_num;
					pthread_mutex_unlock(&pool->lock);
					pthread_exit(NULL);
				}
			}
		}

		/* 如果指定了true，要关闭线程池里的每个线程，自行退出处理 */
		if(pool->shutdown) {
			pthread_mutex_unlock (&pool->lock);
			printf("thread %llu is exiting\n", pthread_self());
			pthread_exit(NULL);     /* 线程自行结束 */
		}

		// 从任务队列里获取任务，是一个出队操作
		task.function=pool->task_queue[pool->queue_front].function;
		task.arg = pool->task_queue[pool->queue_front].arg;

		pool->queue_front = ((pool->queue_front +1) % pool->queue_capacity); // 队头指针向后移动一位。
		--pool->queue_size;

		/* 任务队列中出了一个元素，还有位置 ，唤醒阻塞在这个条件变量上的线程，现在通知可以有新的任务添加进来 */
		pthread_cond_broadcast(&(pool->queue_not_full)); //queue_not_full另一个条件变量。

		/*任务取出后，立即将 线程池琐 释放*/
		pthread_mutex_unlock(&(pool->lock));

		/*执行任务*/
		printf("thread %llu start working\n", pthread_self());
		pthread_mutex_lock(&(pool->thread_counter));         /*忙状态线程数变量琐*/
		++pool->busy_thrd_num;                                /*忙状态线程数+1*/
		pthread_mutex_unlock(&(pool->thread_counter));
		task.function(task.arg);  /*执行回调函数任务，相当于process(arg)  */

		/*任务结束处理*/
		printf("thread %llu end working\n\n", pthread_self());
		usleep(10000);

		pthread_mutex_lock(&pool->thread_counter);
		--pool->busy_thrd_num;                 /*处理掉一个任务，忙状态数线程数-1*/
		pthread_mutex_unlock(&pool->thread_counter);

	}
	pthread_exit(NULL);

    return NULL;
}

void *admin_thread(void *thrdpool)
{
    threadpool_t *pool = thrdpool;

    // 线程池没有关闭
    while(!pool->shutdown) {
		sleep(DEFAULT_TIME);                                    /*定时 对线程池管理*/
		printf("10s is finish,start test thread pool\n");

		pthread_mutex_lock(&pool->lock);
		int queue_size = pool->queue_size;                      /* 关注 任务数 */
		int live_thr_num = pool->live_thrd_num;                 /* 存活 线程数 */
		pthread_mutex_unlock(&(pool->lock));

		pthread_mutex_lock(&(pool->thread_counter));
		int busy_thr_num = pool->busy_thrd_num;
		pthread_mutex_unlock(&(pool->thread_counter));                 /* 忙着的线程数 */
		printf("busy_thr_num is %d\n", busy_thr_num);

		/* 创建新线程 算法： 任务数大于最小线程池个数, 且存活的线程数少于最大线程个数时 如：30>=10 && 40<100 */
		if (queue_size >= MIN_WAIT_TASK_NUM && live_thr_num < pool->max_thrd_num) {
			printf("create new thread\n");

			pthread_mutex_lock(&pool->lock);
			int add = 0;

			/*一次增加 DEFAULT_THREAD 个线程*/
			for (int i = 0; i < pool->max_thrd_num && add < DEFAULT_THREAD_VARY
					&& pool->live_thrd_num < pool->max_thrd_num; ++i) {
				if (pool->threads[i] == 0 || !is_thread_alive(pool->threads[i])) {
					pthread_create(&pool->threads[i], NULL, threadpool_thread, pool);
					++add;
					++pool->live_thrd_num;
				}
			}
			pthread_mutex_unlock(&(pool->lock));

		}

		/* 销毁多余的空闲线程 算法：忙线程X2 小于 存活的线程数 且 存活的线程数 大于 最小线程数时*/
		if ((busy_thr_num * 2) < live_thr_num  &&  live_thr_num > pool->min_thrd_num) {
			printf("delete pthread\n");

			/* 一次销毁DEFAULT_THREAD个线程, 隨機10個即可 */
			pthread_mutex_lock(&(pool->lock));
			pool->wait_exit_thrd_num = DEFAULT_THREAD_VARY;
			pthread_mutex_unlock(&(pool->lock));

			for (int i = 0; i < DEFAULT_THREAD_VARY; ++i) {
				/* 通知处在空闲状态的线程, 他们会自行终止 */
				pthread_cond_signal(&(pool->queue_not_empty));
			}
		}
	}

    return NULL;
}

int threadpool_free(threadpool_t **pool)
{
    threadpool_t *ptr = NULL;

    if (!pool || !*pool) {
        return -1;
    }
    ptr = *pool;
    if (ptr->task_queue) {
        free(ptr->task_queue);
    }

    if (ptr->threads) {
        free(ptr->threads);

        pthread_mutex_unlock(&ptr->lock);
        pthread_mutex_destroy(&ptr->lock);

        pthread_mutex_unlock(&ptr->thread_counter);
        pthread_mutex_destroy(&ptr->thread_counter);

        pthread_cond_destroy(&ptr->queue_not_empty);
        pthread_cond_destroy(&ptr->queue_not_full);
    }

    free(*pool);
    *pool = NULL;

    return 0;
}

int is_thread_alive(pthread_t tid)
{
	int kill_rc = pthread_kill(tid, 0);     //发0号信号，测试线程是否存活
	if (kill_rc == ESRCH) {
		return FALSE;
	}

	return TRUE;
}
