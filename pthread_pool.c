#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include "threadpool.h"

#define DEFAULT_TIME 10                 
#define MIN_WAIT_TASK_NUM 10            
#define DEFAULT_THREAD_VARY 10          
#define true 1
#define false 0