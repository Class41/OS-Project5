#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/msg.h>
#include "queue.h"
#include "shared.h"
#include "string.h"

/*
*	Author: Vasyl Onufriyev
*	Project 5: Resource Managment
*	Date: 4/14/19
*	Purpose: Launch user processes, allocate resourced or deny them depending on a shared memory table
*/

int ipcid;			 //inter proccess shared memory
Shared *data;		 //shared memory data
int toChildQueue;	//queue for communicating to child from master
int toMasterQueue;   //queue for communicating from child to master
char *filen;		 //name of this executable
int childCount = 19; //Max children concurrent

FILE *o; //output log file pointer

const int CLOCK_ADD_INC = 50000;
int VERBOSE_LEVEL = 0;
int LINE_COUNT = 0;

/* Create prototypes for used functions*/
void Handler(int signal);
void DoFork(int value);
void ShmAttatch();
void TimerHandler(int sig);
int SetupInterrupt();
int SetupTimer();
void DoSharedWork();
int FindEmptyProcBlock();
void SweepProcBlocks();
void AddTimeLong(Time *time, long amount);
void AddTime(Time *time, int amount);
int FindPID(int pid);
void QueueAttatch();
void GenerateResources();
void DisplayResources();
int AllocResource(int procRow, int resID);
int FindAllocationRequest(int procRow);

/* Message queue standard message buffer */
struct
{
	long mtype;
	char mtext[100];
} msgbuf;

/* Add time to given time structure, max 2.147billion ns */
void AddTime(Time *time, int amount)
{
	int newnano = time->ns + amount;
	while (newnano >= 1000000000) //nano = 10^9, so keep dividing until we get to something less and increment seconds
	{
		newnano -= 1000000000;
		(time->seconds)++;
	}
	time->ns = newnano; //since newnano is now < 1 billion, it is less than second. Assign it to ns
}

/* Add more than 2.147 billion nanoseconds to the time */
void AddTimeLong(Time *time, long amount)
{
	long newnano = time->ns + amount;
	while (newnano >= 1000000000) //nano = 10^9, so keep dividing until we get to something less and increment seconds
	{
		newnano -= 1000000000;
		(time->seconds)++;
	}
	time->ns = (int)newnano; //since newnano is now < 1 billion, it is less than second. Assign it to ns
}

int CompareTime(Time *time1, Time *time2)
{
	long time1Epoch = ((long)(time1->seconds) * (long)1000000000) + (long)(time1->ns);
	long time2Epoch = ((long)(time2->seconds) * (long)1000000000) + (long)(time2->ns);

	if (time1Epoch > time2Epoch)
		return 1;
	else
		return 0;
}

/* handle ctrl-c and timer hit */
void Handler(int signal)
{
	fflush(stdout); //make sure that messages are output correctly before we start terminating things

	int i;

	DisplayResources();

	printf("\n\n\n** STATUSES **\n");
	for (i = 0; i < 19; i++)
	{
		printf("%i: %s\n", i, data->proc[i].status);
	}

	for (i = 0; i < childCount; i++) //loop thorough the proccess table and issue a termination signal to all unkilled proccess/children
		if (data->proc[i].pid != -1)
			kill(data->proc[i].pid, SIGTERM);

	fflush(o);							  //flush out the output file
	fclose(o);							  //close output file
	shmctl(ipcid, IPC_RMID, NULL);		  //free shared mem
	msgctl(toChildQueue, IPC_RMID, NULL); //free queues
	msgctl(toMasterQueue, IPC_RMID, NULL);

	printf("%s: Termination signal caught. Killed processes and killing self now...goodbye...\n\n", filen);

	kill(getpid(), SIGTERM); //kill self
}

/* Perform a forking call to launch a user proccess */
void DoFork(int value) //do fun fork stuff here. I know, very useful comment.
{
	char *forkarg[] = {//null terminated args set
					   "./user",
					   NULL}; //null terminated parameter array of chars

	execv(forkarg[0], forkarg); //exec
	Handler(1);
}

/* Attaches to shared memory */
void ShmAttatch() //attach to shared memory
{
	key_t shmkey = ftok("shmshare", 312); //shared mem key

	if (shmkey == -1) //check if the input file exists
	{
		fflush(stdout);
		perror("Error: Ftok failed");
		return;
	}

	ipcid = shmget(shmkey, sizeof(Shared), 0600 | IPC_CREAT); //get shared mem

	if (ipcid == -1) //check if the input file exists
	{
		fflush(stdout);
		perror("Error: failed to get shared memory");
		return;
	}

	data = (Shared *)shmat(ipcid, (void *)0, 0); //attach to shared mem

	if (data == (void *)-1) //check if the input file exists
	{
		fflush(stdout);
		perror("Error: Failed to attach to shared memory");
		return;
	}
}

/* Handle the timer hitting x seconds*/
void TimerHandler(int sig)
{
	Handler(sig);
}

/* Setup interrupt handling */
int SetupInterrupt()
{
	struct sigaction act;
	act.sa_handler = TimerHandler;
	act.sa_flags = 0;
	return (sigemptyset(&act.sa_mask) || sigaction(SIGPROF, &act, NULL));
}

/* setup interrupt handling from the timer */
int SetupTimer()
{
	struct itimerval value;
	value.it_interval.tv_sec = 2;
	value.it_interval.tv_usec = 0;
	value.it_value = value.it_interval;
	return (setitimer(ITIMER_PROF, &value, NULL));
}

/* Find the next empty proccess block. Returns proccess block position if one is available or -1 if one is not */
int FindEmptyProcBlock()
{
	int i;
	for (i = 0; i < childCount; i++)
	{
		if (data->proc[i].pid == -1)
			return i; //return proccess table position of empty
	}

	return -1; //error: no proccess slot available
}

/* Sets all proccess blocks to the initial value of -1 for algorithm reasons */
void SweepProcBlocks()
{
	int i;
	for (i = 0; i < MAX_PROCS; i++)
		data->proc[i].pid = -1;
}

void GenerateResources()
{
	srand(time(NULL));
	int i;
	for (i = 0; i < 20; i++)
	{
		data->resVec[i] = (rand() % 5) + 1;
		data->allocVec[i] = data->resVec[i];
	}

	srand(time(NULL));
	for (i = 0; i < 5; i++)
	{
		while (1)
		{
			int tempval = rand() % 20;

			if (CheckForExistence(data->sharedRes, 5, tempval) == -1)
			{
				data->sharedRes[i] = tempval;
				break;
			}
		}
	}

	DisplayResources();
}

int CheckForExistence(int *values, int size, int value)
{
	int i;
	for (i = 0; i < size; i++)
		if (values[i] == value)
			return 1;
	return -1;
}

void DisplayResources()
{
	printf("\n\n##### Beginning print of resource tables #####\n\n");
	printf("** Allocated Resources **\nX -> resources, Y -> proccess\n");
	printf("Proc ");
	int i;
	for (i = 0; i < 20; i++)
	{
		printf("%3i ", i);
	}

	int j;
	for (i = 0; i < 19; i++)
	{
		printf("\n %3i|", i);
		for (j = 0; j < 20; j++)
			printf("%4i", data->alloc[i][j]);
	}

	printf("\n\n\n** Requested Resources **\nX -> resources, Y -> proccess\n");
	printf("Proc ");
	for (i = 0; i < 20; i++)
	{
		printf("%3i ", i);
	}

	for (i = 0; i < 19; i++)
	{
		printf("\n %3i|", i);
		for (j = 0; j < 20; j++)
			printf("%4i", data->req[i][j]);
	}

	printf("\n\n\n** Resource Vector **\n");
	for (i = 0; i < 20; i++)
	{
		printf("%3i ", i);
	}
	printf("\n");
	for (i = 0; i < 20; i++)
	{
		printf("%3i ", data->resVec[i]);
	}

	printf("\n\n\n** Allocation Vector **\n");
	for (i = 0; i < 20; i++)
	{
		printf("%3i ", i);
	}
	printf("\n");
	for (i = 0; i < 20; i++)
	{
		printf("%3i ", data->allocVec[i]);
	}

	printf("\n\n\n** Shared Resource IDs **\n");
	for (i = 0; i < 5; i++)
	{
		printf("%3i ", data->sharedRes[i]);
	}

	printf("\n\n##### Ending print of resource tables #####\n\n");
}

int FindAllocationRequest(int procRow)
{
	int i;
	for (i = 0; i < 20; i++)
	{
		if (data->req[i][procRow] > 0)
			return i;
	}
}

int AllocResource(int procRow, int resID)
{
	if (data->allocVec[resID] - data->req[resID][procRow] >= 0)
	{
		(data->alloc[resID][procRow]) += (data->req[resID][procRow]);
		if (CheckForExistence(&(data->sharedRes), 5, resID) == -1)
			(data->allocVec[resID]) -= (data->req[resID][procRow]);

		(data->req[resID][procRow]) = 0;
		return 1;
	}
	else
	{
		return -1;
	}
}

int DellocResource(int procRow, int resID)
{
	if (CheckForExistence(&(data->sharedRes), 5, resID) == -1)
		(data->allocVec[resID]) += (data->alloc[resID][procRow]);
	data->alloc[resID][procRow] = 0;
}

/* Find the proccess block with the given pid and return the position in the array */
int FindPID(int pid)
{
	int i;
	for (i = 0; i < childCount; i++)
		if (data->proc[i].pid == pid)
			return i;
	return -1;
}

/* The biggest and fattest function west of the missisipi */
void DoSharedWork()
{
	/* General sched data */
	int activeProcs = 0;
	int remainingExecs = 100;
	int exitCount = 0;
	int status;
	int iterator;
	int requestCounter = 0;

	/* Proc toChildQueue and message toChildQueue data */
	int activeProcIndex = -1;
	int procRunning = 0;
	int msgsize;

	/* Set shared memory clock value */
	data->sysTime.seconds = 0;
	data->sysTime.ns = 0;

	/* Setup time for random child spawning */
	Time nextExec = {0, 0};

	/* Create queues */
	struct Queue *resQueue = createQueue(childCount); //Queue of local PIDS (fake/emulated pids)

	srand(time(0)); //set random seed

	while (1)
	{
		AddTime(&(data->sysTime), CLOCK_ADD_INC); //increment clock between tasks to advance the clock a little

		pid_t pid; //pid temp

		/* Only executes when there is a proccess ready to be launched, given the time is right for exec, there is room in the proc table, annd there are execs remaining */
		if (activeProcs < childCount && CompareTime(&(data->sysTime), &nextExec))
		{
			pid = fork(); //the mircle of proccess creation

			if (pid < 0) //...or maybe not proccess creation if this executes
			{
				perror("Failed to fork, exiting");
				Handler(1);
			}

			if (pid == 0)
			{
				DoFork(pid); //do the fork thing with exec followup
			}

			/* Setup the next exec for proccess*/
			nextExec.seconds = data->sysTime.seconds; //capture current time
			nextExec.ns = data->sysTime.ns;

			AddTimeLong(&nextExec, abs((long)(rand() % 501) * (long)1000000)); //set new exec time to 0 - 500ms after now

			/* Setup the child proccess and its proccess block if there is a available slot in the control block */
			int pos = FindEmptyProcBlock();
			if (pos > -1)
			{
				/* Initialize the proccess table */
				data->proc[pos].pid = pid; //we stored the pid from fork call and now assign it to PID

				fprintf(o, "%s: [PROC CREATE] pid: %i\n\n", filen, pid);
				activeProcs++; //increment active execs
			}
			else
			{
				kill(pid, SIGTERM); //if child failed to find a proccess block, just kill it off
			}
		}
		for (iterator = 0; iterator < getSize(resQueue); iterator++)
		{
			int cpid = dequeue(resQueue);
			int procpos = FindPID(cpid);
			int resID = FindAllocationRequest(procpos);

			if (AllocResource(procpos, resID) == 1)
			{
				enqueue(resQueue, cpid);
			}
			else
			{
				fprintf(o, "%s: [REQUEST] [QUEUE] pid: %i request fulfilled...\n\n", filen, msgbuf.mtype);
				strcpy(msgbuf.mtext, "REQ_GRANT");
				msgbuf.mtype = cpid;
				msgsnd(toChildQueue, &msgbuf, sizeof(msgbuf), IPC_NOWAIT); //send parent termination signal
			}
		}

		if ((msgsize = msgrcv(toMasterQueue, &msgbuf, sizeof(msgbuf), 0, IPC_NOWAIT)) > -1) //blocking wait while waiting for child to respond
		{
			requestCounter++;
			if (strcmp(msgbuf.mtext, "REQ") == 0)
			{
				int reqpid = msgbuf.mtype;
				int procpos = FindPID(msgbuf.mtype);
				int resID = 0;
				int count = 0;

				msgrcv(toMasterQueue, &msgbuf, sizeof(msgbuf), reqpid, 0);
				resID = atoi(msgbuf.mtext);

				msgrcv(toMasterQueue, &msgbuf, sizeof(msgbuf), reqpid, 0);
				count = atoi(msgbuf.mtext);

				data->req[resID][procpos] = count;

				int resID = FindAllocationRequest(procpos);

				fprintf(o, "%s: [REQUEST] pid: %i proc: resID: %i\n", filen, msgbuf.mtype, FindPID(msgbuf.mtype), resID);

				if (AllocResource(procpos, resID) == -1)
				{
					enqueue(resQueue, msgbuf.mtype);
					fprintf(o, "\t-> [REQUEST] pid: %i request unfulfilled...\n\n", msgbuf.mtype);
				}
				else
				{
					strcpy(msgbuf.mtext, "REQ_GRANT");
					msgsnd(toChildQueue, &msgbuf, sizeof(msgbuf), IPC_NOWAIT); //send parent termination signal
					fprintf(o, "\t-> [REQUEST] pid: %i request fulfilled...\n\n", msgbuf.mtype);
				}

				printf("\nIn queue: %i", getSize(resQueue));
			}
			else if (strcmp(msgbuf.mtext, "REL") == 0)
			{
				int reqpid = msgbuf.mtype;
				int procpos = FindPID(msgbuf.mtype);

				msgrcv(toMasterQueue, &msgbuf, sizeof(msgbuf), reqpid, 0);
				DellocResource(procpos, atoi(msgbuf.mtext));
				fprintf(o, "%s: [RELEASE] pid: %i proc: %i  resID: %i\n\n", filen, msgbuf.mtype, FindPID(msgbuf.mtype), atoi(msgbuf.mtext));
			}
			else if (strcmp(msgbuf.mtext, "TER") == 0)
			{
				int procpos = FindPID(msgbuf.mtype);

				for (iterator = 0; iterator < 20; iterator++)
				{
					DellocResource(procpos, iterator);
				}

				fprintf(o, "%s: [TERMINATE] pid: %i proc: %i\n\n", filen, msgbuf.mtype, FindPID(msgbuf.mtype));
			}
			if (requestCounter == 19)
			{
				DisplayResources();
				requestCounter = 0;
			}
		}

		if ((pid = waitpid((pid_t)-1, &status, WNOHANG)) > 0) //if a PID is returned meaning the child died
		{
			if (WIFEXITED(status))
			{
				if (WEXITSTATUS(status) == 21) //21 is my custom return val
				{
					exitCount++;
					activeProcs--;

					int position = FindPID(pid);

					//printf("Exit count: %i Active procs: %i", exitCount, activeProcs);

					if (position > -1)
						data->proc[position].pid = -1;
				}
			}
		}

		/*if (remainingExecs <= 0 && exitCount >= 100) //only get out of loop if we run out of execs or we have maxed out child count
		{
			break;
		}*/

		fflush(stdout);
	}

	/* Wrap up the output file and detatch from shared memory items */
	shmctl(ipcid, IPC_RMID, NULL);
	msgctl(toChildQueue, IPC_RMID, NULL);
	msgctl(toMasterQueue, IPC_RMID, NULL);
	fflush(o);
	fclose(o);
}

/* Attach to queues incoming/outgoing */
void QueueAttatch()
{
	key_t shmkey = ftok("shmsharemsg", 766);

	if (shmkey == -1) //check if the input file exists
	{
		fflush(stdout);
		perror("./oss: Error: Ftok failed");
		return;
	}

	toChildQueue = msgget(shmkey, 0600 | IPC_CREAT); //attach to child queue

	if (toChildQueue == -1)
	{
		fflush(stdout);
		perror("./oss: Error: toChildQueue creation failed");
		return;
	}

	shmkey = ftok("shmsharemsg2", 767);

	if (shmkey == -1) //check if the input file exists
	{
		fflush(stdout);
		perror("./oss: Error: Ftok failed");
		return;
	}

	toMasterQueue = msgget(shmkey, 0600 | IPC_CREAT); //attach to master queue

	if (toMasterQueue == -1)
	{
		fflush(stdout);
		perror("./oss: Error: toMasterQueue creation failed");
		return;
	}
}

/* Program entry point */
int main(int argc, int **argv)
{
	//alias for file name
	filen = argv[0]; //shorthand for filename

	if (SetupInterrupt() == -1) //Handler for SIGPROF failed
	{
		perror("./oss: Failed to setup Handler for SIGPROF");
		return 1;
	}
	if (SetupTimer() == -1) //timer failed
	{
		perror("./oss: Failed to setup ITIMER_PROF interval timer");
		return 1;
	}

	int optionItem;
	while ((optionItem = getopt(argc, argv, "hvn:")) != -1) //read option list
	{
		switch (optionItem)
		{
		case 'h': //show help menu
			printf("\t%s Help Menu\n\
		\t-h : show help dialog \n\
        \t-v : enable verbose mode \n\
		\t-n [count] : max proccesses at the same time. Default: 19\n\n",
				   filen);
			return;
		case 'v': //max # of children
			VERBOSE_LEVEL = 1;
			printf("%s: Verbose mode enabled...\n", argv[0]);
			break;
		case 'n': //max # of children
			childCount = atoi(optarg);
			if (childCount > 19 || childCount < 0) //if 0  > n > 20
			{
				printf("%s: Max -n is 19. Must be > 0 Aborting.\n", argv[0]);
				return -1;
			}

			printf("\n%s: Info: set max concurrent children to: %s", argv[0], optarg);
			break;
		case '?': //an error has occoured reading arguments
			printf("\n%s: Error: Invalid Argument or Arguments missing. Use -h to see usage.", argv[0]);
			return;
		}
	}

	o = fopen("output.log", "w"); //open output file

	if (o == NULL) //check if file was opened
	{
		perror("oss: Failed to open output file: ");
		return 1;
	}

	ShmAttatch();	  //attach to shared mem
	QueueAttatch();	//attach to queues
	SweepProcBlocks(); //reset all proc blocks
	GenerateResources();
	signal(SIGINT, Handler); //setup handler for CTRL-C
	DoSharedWork();			 //fattest function west of the mississippi

	return 0;
}
