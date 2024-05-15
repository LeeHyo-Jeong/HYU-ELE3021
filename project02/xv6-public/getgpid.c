#include "types.h"
#include "defs.h"
//#include "date.h"
#include "param.h"
#include "mmu.h"
#include "proc.h"

// getgpid system call
int getgpid(char* str){
	cprintf("%s ", str);
	
	struct proc* curproc = myproc();
	int gpid = curproc->parent->parent->pid;

	return gpid;
}

// wrapper for getgpid
int sys_getgpid(void){
	char *str;
	if(argstr(0, &str) < 0)
		return -1;
	
	return getgpid(str);
}
