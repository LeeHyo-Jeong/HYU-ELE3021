#include "types.h"
#include "stat.h"
#include "user.h"

#define NUM_LOOP 10
int main(int argc, char* argv[]){
	int pid, i;
	pid = fork();
	
	if(pid == 0){
		for(i = 0 ; i < NUM_LOOP ; i++){
			printf(1, "Child\n");
			yield();
		}
	}
	else{
		for(i = 0 ; i < NUM_LOOP ; i++){
			printf(1, "Parent\n");
			yield();
		}
	}

	return 0;
}
