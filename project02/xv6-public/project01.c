#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char *argv[]){
	printf(1, "My student id is 2022077510\n");
	int pid = getpid();
	printf(1, "My pid is %d\n", pid);

	char* buf = "My gpid is";
	int gpid = getgpid(buf);
	printf(1, "%d\n", gpid);
	
	exit();
}
