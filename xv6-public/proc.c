#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct Queue{
	struct proc* front;		// first process of the queue
	struct proc* rear;		// last process of the queue
	struct spinlock lock;		// lock of the queue
	int tq;				// time quantum of the queue
	int level;
	int size;			// # of unfinished process, used for MoQ		
};

struct Queue mlfq[4];
struct Queue moq;

void initQueue(struct Queue* q);
int mlfq_empty(int level);
void enqueue(struct proc* p, int level);
void dequeue(struct proc* p, struct Queue* q);
void move(struct proc* p, int level);
int mlfq_isrunnable(struct proc* p);
void priority_boosting(void);
void moq_move(struct proc* p);
void moq_dequeue(struct proc* p);

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;


/*
 if((p->pgdir = setupkvm()) == 0){
    kfree(p->kstack);
    p->kstack = 0;
    p->state = UNUSED;
    return 0;
  }*/
  
  p->queuelevel = -1;
  p->proctick = 0;
  p->priority = 0;

  enqueue(p, 0);
  
  isMLFQ = 1;
 /* acquire(&ptable.lock);
  p->state = RUNNABLE;
  release(&ptable.lock);
  */
  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}


// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

int getlev(void){
	return myproc()->queuelevel;
}

void initQueue(struct Queue* q){
	q->front = 0;
	q->rear = 0;
	q->tq = 0;
}

int mlfq_empty(int level){
	return (mlfq[level].front == 0);
}

void enqueue(struct proc* p, int level){
	if(p->queuelevel != -1){
		return;
	}
		
	if(level < 0 || level > 4){
		cprintf("out of level range\n");
		return;
	}

	// acquire lock
	//acquire(&mlfq[level].lock);

	// if the queue is empty
	if(mlfq_empty(level)){
		acquire(&mlfq[level].lock);
		mlfq[level].front = p;
		mlfq[level].rear = p;
		release(&mlfq[level].lock);
	}

	// if the queue is not empty
	else{	
		acquire(&mlfq[level].lock);
		// set the process p as the last process of the queue
		mlfq[level].rear->next = p;
		p->prev = mlfq[level].rear;
		mlfq[level].rear = p;
		release(&mlfq[level].lock);
	}
	
	// the next process of the last process is null
	p->next = 0;
	
	p->queuelevel = level;
	p->proctick = 0;


	//p->state = RUNNABLE;

	
	p->priority = 0;

	//release(&mlfq[level].lock);
}

// this function is for debugging
void proc_print(){
	struct proc* p;
	for(int level = 0 ; level < 4 ; level++){
		p = mlfq[level].front;
		cprintf("[ L%d ] global tick: %d\n", level, ticks);
		if(p == 0) cprintf(" empty queue\n");
		while(p != 0){
			cprintf(" %d | ", p->pid);
			if(p->next == p) return;
			p = p->next;
		}
	cprintf("\n");
	}
}

void moq_dequeue(struct proc* p){
	if(p->isMoQ != 1){
		cprintf("This process is not in MoQ\n");
		return;
	}
	
	// if the process to be dequeued is front of MoQ
	if(p == moq.front){
                //acquire(&moq.lock);
                moq.front = p->next;
                if(moq.front == 0) moq.rear = 0;
                p->next = 0;
                //release(&moq.lock);
	}
	else{
		//acquire(&moq.lock);
		struct proc* find = moq.front;
		while(find->next != p) find = find->next;
                find->next = p->next;
                if(moq.rear == p) moq.rear = find;
                p->next = 0;
		//release(&moq.lock);
	}

	p->isMoQ = 0;
}


void dequeue(struct proc* p, struct Queue* q){
	if(p->queuelevel != q->level){
		return;
	}

	int level = p->queuelevel;
	if(level < 0 || level > 4){
		cprintf("out of level range\n");
		return;
	}

	//acquire(&mlfq[level].lock);

	if(mlfq_empty(level)){
		cprintf("Empty queue\n");
		return;
	}
	
	// find the given process in the queue
	// and if it is found, dequeue it from the queue

	// if the process to be dequeued is front of the queue
	if(p == mlfq[level].front){
		acquire(&mlfq[level].lock);
		mlfq[level].front = p->next;
		if(mlfq[level].front == 0) mlfq[level].rear = 0;
		p->next = 0;
		release(&mlfq[level].lock);
	}
	
	// else, find the process in the queue
	else{
		struct proc* find = q->front;

		while(find->next != p) find = find->next;
		find->next = p->next;
		if(q->rear == p) q->rear = find;
		p->next = 0;
	}
	p->queuelevel = -1;
	//release(&mlfq[level].lock);
}	

void move(struct proc* p, int level){
	/*cprintf(" [ %d -> %d ]\n", p->queuelevel, level);
	cprintf("Before move\n");
	proc_print();*/
	dequeue(p, &mlfq[p->queuelevel]);
	enqueue(p, level);
	p->queuelevel = level;
	/*cprintf("After move\n");
	proc_print();*/
}

int mlfq_isrunnable(struct proc* p){
	return (p != 0 && p->state == RUNNABLE);
}

void priority_boosting(void){
	// if global tick equals to 100,
	// move processes from L1~3 to L0
	//cprintf("<---------priority boosting---------->\n");
	struct proc* p;

	
	acquire(&ptable.lock);
	
	//cprintf("before boosting\n");
	//proc_print();
	for(p = ptable.proc ; p < &ptable.proc[NPROC] ; p++){
		// move only MLFQ processes
		if(p->isMoQ == 1) continue;

		
		if(p->queuelevel == 1 || p->queuelevel == 2 || p->queuelevel == 3){
			dequeue(p, &mlfq[p->queuelevel]);
			enqueue(p, 0);
		
			//p->priority = 0;
			p->proctick = 0;
		}
	}
	//cprintf("after boosting\n");
	//proc_print();

	release(&ptable.lock);
	

	mlfq[1].front = 0;
	mlfq[2].front = 0;
	mlfq[3].front = 0;
	
	//ticks = 0;
}

int setpriority(int pid, int priority){
	if(0 > priority || 10 < priority) return -2;

	acquire(&ptable.lock);
	struct proc* find;
	for(find = ptable.proc; find < &ptable.proc[NPROC] ; find++){
		if(find->pid == pid){
                	find->priority = priority;
			release(&ptable.lock);
			return 0;
                }
        }
        // process not found
	release(&ptable.lock);
        return -1;
}

#ifdef MLFQ
void scheduler(void){
	struct proc* p = 0;
	struct cpu* c = mycpu();
	// there is no process which is using this cpu
	c->proc = 0;

	// set up time quantum for each queue
	for(int i = 0 ; i < 4 ; i++){
		//cprintf("timequantum\n");
		mlfq[i].tq = 2 * i + 2;
		mlfq[i].level = i;
	}

	for(;;){
		// check if monopolize called
		if(isMoQ == 1 && isMLFQ == 0) {
			//acquire(&monolock);
			//cprintf("MoQ scheduler executed\n");
			// implement locks
			//acquire(&ptable.lock);
			// run until no ZOMBIE exist in MoQ
        		//acquire(&moq.lock);
       			for(p = moq.front ; p != 0 ; p = p->next){
                		if(p->state != RUNNABLE) continue;
        			
				//cprintf("Processing in MoQ..\n");
		
                		// run the process in MoQ

				acquire(&ptable.lock);
                		c->proc = p;
                		switchuvm(p);
                		p->state = RUNNING;
                		swtch(&(c->scheduler), p->context);
                		switchkvm();
				release(&ptable.lock);

                		c->proc = 0;
		
				//cprintf("Process in MoQ executed\n");

				if(p->state == ZOMBIE){
					//cprintf("Delete zombie in MoQ\n");
					moq_dequeue(p);
				}
				
				if(moq.front == 0){
					unmonopolize();
				}
        		}
	
/*
			// if there is no process in MoQ, unmonopolize				
			if(moq.front == 0){
				//release(&moq.lock);
				unmonopolize();
			}
*/	
			//release(&monolock);
			//release(&moq.lock);
			//release(&ptable.lock);
		}
		// MLFQ scheduler
		else if(isMLFQ == 1 && isMoQ == 0){
			sti();
			
			acquire(&ptable.lock);
			for(int level = 0 ; level < 3 ; level++){
                        	for(p = mlfq[level].front ; p != 0 ; p = p->next){
                                	if(p->state != RUNNABLE) continue;
			
					// run the process
					c->proc = p;
					switchuvm(p);
					p->state = RUNNING;
					p->proctick++;
                               		swtch(&(c->scheduler), p->context);
					switchkvm();

                                	// process done running
                                	c->proc = 0;
							
                        	        // if the process end up its job during time quantum
                        	        if(p->state == ZOMBIE){
						//cprintf("ZOMBIE\n");
                        	                dequeue(p, &mlfq[level]);
                        	        }

					// to avoid moving SLEEPING process, check if the process if RUNNABLE
					else if(p->state == RUNNABLE){
                        	        	// L0 process used up its time quantum
                        	        	// and it has even pid
						if(p->queuelevel == 0 && p->proctick >= mlfq[0].tq){ 
							if(p->pid % 2 == 0){
								//cprintf("move even\n");
								move(p, 2);
							}
                                			// it has odd pid
                                			else if(p->pid % 2 != 0){
								//cprintf("move odd\n");
							 	move(p, 1);
							}
						}	
						else if(p->queuelevel == 1 && p->proctick >= mlfq[1].tq){
							//cprintf("move to L3\n");
							move(p, 3);
						}
						else if(p->queuelevel == 2 && p->proctick >= mlfq[2].tq){
							//cprintf("move to L3\n");
							move(p, 3);
						}
                        		}
				}
			}

			// L3
			if(mlfq[3].front != 0 && mlfq[1].front == 0 && mlfq[2].front == 0){
				//acquire(&mlfq[3].lock);
	
				struct proc* max = mlfq[3].front;
				struct proc* find = 0;
				
				int max_priority = max->priority;

				for(find = mlfq[3].front ; find != 0 ; find = find->next){
					if(find->state != RUNNABLE) continue;
					if(find->priority > max_priority){
						max_priority = find->priority;
						max = find;
					}
				}		
			
				if(max == 0){
					release(&ptable.lock);
					continue;			
				}

				// run the process
                        	c->proc = max;
                        	switchuvm(max);
                        	max->state = RUNNING;
                        	max->proctick++;
                        	swtch(&(c->scheduler), max->context);
                        	switchkvm();
                        	      
				// process done running
                        	c->proc = 0;
                	
		        	// if the process end up its job during time quantum
                	        if(max->state == ZOMBIE){
                        	    	dequeue(max, &mlfq[3]);
                        	}
			
				// if the process has used up its time quantum
				if(max->state == RUNNABLE && max->proctick >= mlfq[3].tq){
					max->proctick = 0;
					setpriority(max->pid, (max->priority)-1);
				}
			}
                	release(&ptable.lock);
		}
	}
}

// moq schudeluer need to be implemented here

// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
#else
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;

  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;


      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }

    release(&ptable.lock);

    }
}

#endif

int setmonopoly(int pid, int password){
	if(password != 2022077510){
		cprintf("Invalid password\n");
		kill(pid);
		return -2;
	}

	struct proc* p;
	
	//acquire(&ptable.lock);
	for(p = ptable.proc ; p < &ptable.proc[NPROC] ; p++){
		if(p->pid == pid){
			// move the process from MLFQ to MoQ
			moq_move(p);
			p->isMoQ = 1;
			return moq.size;
		}
	}

	//cprintf("setmonopoly error\n");	
	//release(&ptable.lock);	
	
	return -1;
}

void monopolize(){
	acquire(&monolock);
	isMoQ = 1;
	release(&monolock);

	acquire(&mlfqlock);
	isMLFQ = 0;
	release(&mlfqlock);

	//cprintf("monopolize ended\n");
}

void unmonopolize(){
	// dequeue every ZOMBIE process in MoQ
	acquire(&monolock);
	isMoQ = 0;
	release(&monolock);
	
	acquire(&moq.lock);
	struct proc* p;
	for(p = moq.front ; p != 0 ; p = p->next){
		if(p->state == ZOMBIE && p->isMoQ == 1)
			moq_dequeue(p);
	}
	release(&moq.lock);

	ticks = 0;
	
	acquire(&mlfqlock);
	isMLFQ = 1;
	release(&mlfqlock);

	//cprintf("unmopolize ended\n");
}

void moq_move(struct proc* p){
        if(p == 0) return;

        int level = p->queuelevel;

        // dequeue the process from previous MLFQ
        dequeue(p, &mlfq[level]);
	
	acquire(&ptable.lock);

	if(p->queuelevel != -1) return;
	// enqueue the process in MoQ
        // if MoQ is empty
        if(moq.front == 0){
                moq.front = p;
                moq.rear = p;
        }
        
        // if MoQ is not empty
        else{
                moq.rear->next = p;
                moq.rear = p;
		p->next = 0;
        }
	
	if(p->state == RUNNABLE) moq.size++;

	release(&ptable.lock);

	p->isMoQ = 1;
	p->queuelevel = 99;
	//cprintf("moq_move ended\n");
	//cprintf("processes in MoQ\n");
	/*
	for(struct proc* q = moq.front ; q != 0 ; q = q->next)
		cprintf("%d | ", q->pid);
	cprintf("\n");
	*/
}

