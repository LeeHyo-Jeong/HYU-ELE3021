#include <stdio.h>
#include <pthread.h>

int shared_resource = 0;

#define NUM_ITERS 100
#define NUM_THREADS 1000

void lock();
void unlock();

typedef struct{
	// Initially, the value of lock is 0,
	// which means that a thread can acquire lock
	volatile int lock;
} SpinLock;

SpinLock s;

// Test method for lock
int test(int n){
	int cnt = 0;
	
	for(int i = 0 ; i < 1000 ; i++)
		cnt += i;
	
	return n + 1;
}

void lock(SpinLock *s){
	int val = 1;
	__asm__ __volatile__(
		"1: xchg %0, %1\n"
		"test %0, %0\n"
		"jnz 1b\n"
		: "+r" (val), "+m" (*s)
		:
		: "memory"
	);
}

void unlock(SpinLock* s){
	__asm__ __volatile__(
		"movl $0, %0\n"
		: "+m" (*s)
		:
		: "memory"
	);
}

void* thread_func(void* arg) {
    int tid = *(int*)arg;
    
/*    lock();
    
        for(int i = 0; i < NUM_ITERS; i++)    shared_resource++;
    
      unlock();
*/
	lock(&s);
	for(int i = 0 ; i < NUM_ITERS ; i++){
		shared_resource = test(shared_resource);
	}
	unlock(&s);
    pthread_exit(NULL);
}

int main() {
    pthread_t threads[NUM_THREADS];
    int tids[NUM_THREADS];
    
    for (int i = 0; i < NUM_THREADS; i++) {
        tids[i] = i;
        pthread_create(&threads[i], NULL, thread_func, &tids[i]);
    }
    
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("shared: %d\n", shared_resource);
    
    return 0;
}

