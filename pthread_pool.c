#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include "pthread_pool.h"

#define DEFAULT_TIME 10                 
#define MIN_WAIT_TASK_NUM 10            
#define DEFAULT_THREAD_VARY 10          
#define true 1
#define false 0

typedef struct{
    void *(*function)(void *);//函数指针 回调函数
    void *arg;//上面函数的参数
}threadpool_task_t;
struct threadpool_t
{
    pthread_mutex_t lock;//用于锁本结构体
    pthread_mutex_t thread_counter;//记录忙状态线程个数的锁

    pthread_cond_t queue_not_full;//当任务队列满时 添加任务的线程阻塞等待此条件变量；
    pthread_cond_t queue_not_empty;//任务队列里不为空时，通知等待任务的线程

    pthread_t *threads;//存放线程池中每个线程的tid  数组
    pthread_t adjust_tid ;//存管理线程tid
    threadpool_task_t *task_queue; //任务队列（数组首地址）

    int min_thr_num;//线程池最小线程数
    int max_thr_num;//线程池最大线程数
    int live_thr_num;//当前存活线程个数
    int busy_thr_num;//忙状态线程个数
    int wait_exit_thr_num;//要销毁的线程个数

    int queue_front;//任务队列队头下标
    int queue_rear;//任务队列队尾下标
    int queue_size;//任务队列中实际的任务数
    int queue_max_size;//任务队列可容纳的任务数上限

    int shutdown;//线程池使用状态即有没有关闭 true or false
};
int threadpool_free(threadpool_t *pool)
{
    if (pool == NULL) {
        return -1;
    }

    if (pool->task_queue) {
        free(pool->task_queue);
    }
    if (pool->threads) {
        free(pool->threads);
        pthread_mutex_lock(&(pool->lock));
        pthread_mutex_destroy(&(pool->lock));
        pthread_mutex_lock(&(pool->thread_counter));
        pthread_mutex_destroy(&(pool->thread_counter));
        pthread_cond_destroy(&(pool->queue_not_empty));
        pthread_cond_destroy(&(pool->queue_not_full));
    }
    free(pool);
    pool = NULL;

    return 0;
}
void * threadpool_thread(void *threadpool)
{
    threadpool_t *pool=(threadpool_t*)threadpool;
    threadpool_task_t task;
    while(true)
    {
        pthread_mutex_lock(&(pool->lock));
        while((pool->queue_size==0)&&(!pool->shutdown))
        {
            pthread_cond_wait(&(pool->queue_not_empty),&(pool->lock));
            if(pool->wait_exit_thr_num>0)//线程池关闭过程中控制工作线程的安全退出
            {
                pool->wait_exit_thr_num--;
                if(pool->live_thr_num>pool->min_thr_num)
                {
                    pool->live_thr_num--;
                    pthread_mutex_unlock(&(pool->lock));
                    pthread_exit(NULL);
                }
            }
        }
    if(pool->shutdown)//如果线程池处于关闭状态（pool->shutdown），当前线程会解锁并退出。
    {
        pthread_mutex_unlock(&(pool->lock));
        pthread_detach(pthread_self());
        pthread_exit(NULL);
    }
    //任务队列中有任务，线程从 queue_front 位置取出任务，
    //更新队列前端指针，并减小队列中的任务数
    //取出任务后发出 queue_not_full 信号通知生产者线程然后解锁。
    task.function=pool->task_queue[pool->queue_front].function;
    task.arg=pool->task_queue[pool->queue_front].arg;
    pool->queue_front=(pool->queue_front+1)%pool->queue_max_size;//创造环形队列
    pool->queue_size--;
    pthread_cond_broadcast(&(pool->queue_not_full));
    pthread_mutex_unlock(&(pool->lock));

    pthread_mutex_lock(&(pool->thread_counter));
    pool->busy_thr_num++;
    pthread_mutex_unlock(&(pool->thread_counter));
    (*(task.function))(task.arg);
    pthread_mutex_lock(&(pool->thread_counter));
    pool->busy_thr_num--;
    pthread_mutex_unlock(&(pool->thread_counter));
    }
    pthread_exit(NULL);
}

void * adjust_pthread(void * threadpool)
{
    threadpool_t *pool=(threadpool_t*)threadpool;
    while(!pool->shutdown)
    {
        sleep(DEFAULT_TIME);
        pthread_mutex_lock(&(pool->lock));
        int queue_size=pool->queue_size;
        int live_thr_num=pool->live_thr_num;
        pthread_mutex_unlock(&(pool->lock));
        pthread_mutex_lock(&(pool->thread_counter));
        int busy_thr_num = pool->busy_thr_num; 
        thread_mutex_unlock(&(pool->thread_counter));
        if (queue_size >= MIN_WAIT_TASK_NUM && live_thr_num < pool->max_thr_num) 
        {
            int add=0;
            for (int i = 0; i < pool->max_thr_num && add < DEFAULT_THREAD_VARY&& pool->live_thr_num < pool->max_thr_num; i++) 
            {
                if (pool->threads[i] == 0 || !is_thread_alive(pool->threads[i])) 
                {
                    pthread_create(&(pool->threads[i]), NULL, threadpool_thread, (void *)pool);
                    add++;
                    pool->live_thr_num++;
                }
            }
        }
        
        if ((busy_thr_num * 2) < live_thr_num && live_thr_num > pool->min_thr_num) 
        {
            pool->wait_exit_thr_num = DEFAULT_THREAD_VARY;
            for (int i = 0; i < DEFAULT_THREAD_VARY; i++) 
            {
                pthread_cond_signal(&(pool->queue_not_empty));
                //如果忙碌线程数过少且存活线程数过多
                //管理线程会通过 pthread_cond_signal 唤醒空闲线程，并让这些线程自行退出以减少线程池中的线程数。
            }   
        }

    }

}
threadpool_create(int min_thr_num,int max_thr_num,int queue_max_size)
{
    threadpool_t *pool=NULL;
    do{
        if((pool=(threadpool_t *)malloc(sizeof(threadpool_t)))==NULL)
        {
            printf("malloc threadpool fail");
            break; 
        }
        pool->max_thr_num=max_thr_num;
        pool->min_thr_num=min_thr_num;
        pool->busy_thr_num=0;
        pool->live_thr_num=min_thr_num;
        pool->queue_front=0;
        pool->queue_rear=0;
        pool->queue_size=0;
        pool->shutdown=false;
        pool->wait_exit_thr_num=0;
        pool->queue_max_size=queue_max_size;
        pool->threads=(pthread_t*)malloc(sizeof((pthread_t*)max_thr_num));
          if (pool->threads == NULL) {
            printf("malloc threads fail");
            break;
        }
        pool->task_queue=(threadpool_task_t *)malloc(sizeof(threadpool_task_t )*queue_max_size);
        if (pool->task_queue == NULL) {
            printf("malloc task_queue fail");
            break;
        }
        pthread_mutex_init(&(pool->lock),NULL);
        pthread_mutex_init(&(pool->thread_counter),NULL);
        pthread_cond_init(&(pool->queue_not_full),NULL);
        pthread_cond_init(&(pool->queue_not_empty),NULL);
        //创建工作线程
        for(int i=0;i<min_thr_num;++i)
        {
            pthread_create(&(pool->threads[i]),NULL,threadpool_thread,(void*)pool);
            printf("start thread 0x%x...\n", (unsigned int)pool->threads[i]);
        }
        //创建管理线程
        pthread_create(&(pool->adjust_tid),NULL,adjust_pthread,(void *)pool);
        return pool;
    }while(0);//如果中间有错直接break退出  do while代替goto
    threadpool_free(pool);//若有任何步骤失败直接释放所有资源
    return NULL;
}
//将任务添加到任务队列中，并根据需要唤醒等待的工作线程
int threadpool_add(threadpool_t *pool,void*(*function)(void * arg),void*arg)
{
    pthread_mutex_lock(&(pool->lock));
    while(pool->queue_size==pool->queue_max_size&&(!pool->shutdown))
    {
        pthread_cond_wait(&(pool->queue_not_full),&(pool->lock));
    }
    if(pool->shutdown)
    {
        pthread_cond_broadcast(&(pool->queue_not_empty));
        pthread_mutex_unlock(&(pool->lock));
        return 0;
    }
    //检查任务队列中 rear 指向的位置的参数是否为 NULL。如果不是，将其设置为 NULL，以防止内存泄漏。
    if(pool->task_queue[pool->queue_rear].arg!=NULL)
    {
        pool->task_queue[pool->queue_rear].arg=NULL;
    }
    pool->task_queue[pool->queue_rear].function=function;
    pool->task_queue[pool->queue_rear].arg=arg;
    pool->queue_rear=(pool->queue_rear+1)%pool->queue_max_size;
    pool->queue_size++;

    pthread_cond_signal(&(pool->queue_not_empty));
    pthread_mutex_unlock(&(pool->lock));
}

void*process(void*arg)
{
    printf("thread 0x%x working on task %d\n ",(unsigned int)pthread_self(),(int)arg);
    sleep(1);                           
    printf("task %d is end\n",(int)arg);

    return NULL;
}

int threadpool_destory(threadpool_t * pool)
{
    if(pool==NULL)
    {
        return -1;
    }
    pool->shutdown=true;
    //关闭管理线程
    pthread_join(pool->adjust_tid,NULL);
    //循环调用确保所有线程都能收到信号
    for(int i=0;i<pool->live_thr_num;++i)
    {
        pthread_cond_broadcast(&(pool->queue_not_empty));
    }
    for(int i=0;i<pool->live_thr_num;++i)
    {
        pthread_join(pool->threads[i],NULL);
    }
    threadpool_free(pool);
    return 0;
}
int main()
{
    threadpool_t *thp=threadpool_create(3,100,100);
    int num[20], i;
    for (i = 0; i < 20; i++) {
        num[i] = i;
        printf("add task %d\n",i);
        threadpool_add(thp, process, (void*)&num[i]);   
    }
    sleep(10);                                        
    threadpool_destroy(thp);
}