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

#define MAX_LINES 20000			   //Max lines to be allowed in log file, multiply this by 5 to get real count
const int CLOCK_ADD_INC = 5000000; //How much to increment the clock by per tick
int NUM_SHARED = -1;			   //number of shared resources, set at runtime
int VERBOSE_LEVEL = 0;			   //verbose level, 1 = verbose, 0 is non verbose
long lineCount = 0;				   //keeps track of # of lines written to file

/* Statistics */
int deadlockCount = 0; //keeps track of how many deadlocks happened
int deadlockProcs = 0; //keepstrack of how many procs were kille b/c of deadlock

int pidreleases = 0;  //how many resouces released
int pidallocs = 0;	//how many resources alloced
int pidprocterms = 0; //how many procceses terminated

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

/* My new time comparison function which uses epoch math instead of comparing nano/secs which sometimes causes issues*/
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

	if (VERBOSE_LEVEL == 1 && lineCount++ < MAX_LINES) //Display final state of resources
		DisplayResources();

	if (VERBOSE_LEVEL == 1) //Display each child's status word before the end
	{
		printf("\n\n\n** STATUSES **\n");
		for (i = 0; i < childCount; i++)
		{
			printf("%i: %s\n", i, data->proc[i].status);
		}
	}

	/* Display program statistics to the screen */
	double ratio = ((double)deadlockProcs) / ((double)pidprocterms);

	printf("\n\n*** Statistics ***\n\n\
	Resource Allocs: %i\n\
	Resource Releases %i\n\n\
	Process Terminations: %d\n\
	Process Deadlock Proc Kills %d\n\
	Process Deadlock Count: %i\n\
	Process Deadlock to Normal Death Ratio: %f\n\n",
		   pidallocs, pidreleases, pidprocterms, deadlockProcs, deadlockCount, ratio);

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

/* The miracle of resource creation is done here */
void GenerateResources()
{
	int i;
	for (i = 0; i < 20; i++) //Populate resource vector & copy to allocation vector initially
	{
		data->resVec[i] = (rand() % 10) + 1;
		data->allocVec[i] = data->resVec[i];
	}

	NUM_SHARED = (rand() % 4) + 2; //Generate random size of shared resources -- sometimes sticks to 5 no matter what /shrug

	for (i = 0; i < NUM_SHARED; i++)
	{
		while (1)
		{
			int tempval = rand() % 20;

			if (CheckForExistence(data->sharedRes, NUM_SHARED, tempval) == -1) //If shared  resource is yet to be in the array, add it.
			{
				data->sharedRes[i] = tempval;
				break;
			}
		}
	}
	if (VERBOSE_LEVEL == 1 && lineCount++ < MAX_LINES)
		DisplayResources(); //Display initial state of machine
}

/* Checks for existence of value in values of size 'size', 1 on found, -1 on not found */
int CheckForExistence(int *values, int size, int value)
{
	int i;
	for (i = 0; i < size; i++)
		if (values[i] == value)
			return 1;
	return -1;
}

/* Display the system resource tables to the file */
void DisplayResources()
{
	fprintf(o, "\n\n##### Beginning print of resource tables #####\n\n"); //print table headers
	fprintf(o, "** Allocated Resources **\nX -> resources, Y -> proccess\n");
	fprintf(o, "Proc ");
	int i;
	for (i = 0; i < 20; i++)
	{
		fprintf(o, "%3i ", i);
	}

	int j; //print resources for each child
	for (i = 0; i < childCount; i++)
	{
		fprintf(o, "\n %3i|", i);
		for (j = 0; j < 20; j++)
			fprintf(o, "%4i", data->alloc[j][i]);
	}

	fprintf(o, "\n\n\n** Requested Resources **\nX -> resources, Y -> proccess\n"); //print headers
	fprintf(o, "Proc ");
	for (i = 0; i < 20; i++)
	{
		fprintf(o, "%3i ", i);
	}

	//print resources for each child
	for (i = 0; i < childCount; i++)
	{
		fprintf(o, "\n %3i|", i);
		for (j = 0; j < 20; j++)
			fprintf(o, "%4i", data->req[j][i]);
	}

	//print resource vector & header
	fprintf(o, "\n\n\n** Resource Vector **\n");
	for (i = 0; i < 20; i++)
	{
		fprintf(o, "%3i ", i);
	}
	fprintf(o, "\n");
	for (i = 0; i < 20; i++)
	{
		fprintf(o, "%3i ", data->resVec[i]);
	}

	//print allocation vector and resources
	fprintf(o, "\n\n\n** Allocation Vector **\n");
	for (i = 0; i < 20; i++)
	{
		fprintf(o, "%3i ", i);
	}
	fprintf(o, "\n");
	for (i = 0; i < 20; i++)
	{
		fprintf(o, "%3i ", data->allocVec[i]);
	}

	//print shared resource array and headers
	fprintf(o, "\n\n\n** Shared Resource IDs **\n");
	for (i = 0; i < NUM_SHARED; i++)
	{
		fprintf(o, "%3i ", data->sharedRes[i]);
	}

	fprintf(o, "\n\n##### Ending print of resource tables #####\n\n");
}

/* Scans request table and finds the first non-zero value. This indicates this resouce is being requested */
int FindAllocationRequest(int procRow)
{
	int i;
	for (i = 0; i < 20; i++)
		if (data->req[i][procRow] > 0)
			return i;
}

/*DEPRECATED: Was used for bug testing. Calculates resource column to make sure it doesn't exceed total resource */
int CalcResourceTotal(int resID)
{
	int i;
	int total = 0;
	for (i = 0; i < childCount; i++)
		total += data->alloc[resID][i];

	if (total > 0)
		printf("\nTotal of resID: %i is %i", resID, total);
}

/* Handles reource allocation */
int AllocResource(int procRow, int resID)
{
	while (data->allocVec[resID] > 0 && data->req[resID][procRow] > 0) //While we have a request for the proccess that is not complete and have resources in our vector
	{
		if (CheckForExistence(&(data->sharedRes), NUM_SHARED, resID) == -1) //If not a shared resource
		{
			(data->allocVec[resID])--;
		}
		(data->alloc[resID][procRow])++;
		(data->req[resID][procRow])--;
	}

	if (data->req[resID][procRow] > 0) //if we still don't have all resources, request unfulfilled.
		return -1;

	return 1;
}

/* Delete proccess - Clear its rows in resource allocation and request tables and clear it from the queue */
void DeleteProc(int procrow, struct Queue *queue)
{
	int i;
	for (i = 0; i < 20; i++) //Clear from alloc and requet tables
	{
		if (CheckForExistence(&(data->sharedRes), NUM_SHARED, i) == -1) //if not shared, add resource to alloc vector
			data->allocVec[i] += data->alloc[i][procrow];
		if (data->alloc[i][procrow] > 0)
			pidreleases++;

		data->alloc[i][procrow] = 0;
		data->req[i][procrow] = 0;
	}

	//	printf("%i", getSize(queue));
	int temp;
	for (i = 0; i < getSize(queue); i++) //loop through array and clear out junk values and the current one deleted
	{
		temp = dequeue(queue);

		if (temp == data->proc[procrow].pid || temp == -1)
			continue;
		else
			enqueue(queue, temp);
	}
}

/* Dellocs a specific resource for a specific process */
void DellocResource(int procRow, int resID)
{
	if (CheckForExistence(&(data->sharedRes), NUM_SHARED, resID) == -1) //if not shared
	{
		(data->allocVec[resID]) += (data->alloc[resID][procRow]);
	}
	pidreleases++;
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

/*DEPRECATED: Check if array1 is greater equalto array2 in all positions */
int CompareArrayAgainstReq(int *array1, int procpos)
{
	int i;
	for (i = 0; i < 20; i++)
	{
		if ((array1[i] - data->req[i][procpos]) < 0)
		{
			return -1;
		}
	}

	return 1;
}

/* Deadlock detection algorithm be here */
void DeadLockDetector(int *procFlags)
{
	int *tempVec = calloc(20, sizeof(int)); //create a temporary vector with a copy of current resource alloc vector
	int i, j;
	int isEnding = 0; //keeps track of proccess flag if it will end eventually or deadlocked

	for (i = 0; i < 20; i++)
		tempVec[i] = data->allocVec[i];

	int updated;
	do
	{
		updated = 0;
		for (i = 0; i < childCount; i++) //for each process in the table
		{
			if ((procFlags[i] == 1) || (data->proc[i].pid < 0))
				continue; //proccess has already been marked as ending, no reason to check it again.

			isEnding = 1;			 //assume the process will end until otherwise detected
			for (j = 0; j < 20; j++) //for each resource j in process i
			{
				if ((data->req[j][i]) > 0 && (tempVec[j] - data->req[j][i]) < 0) //if we are requesting more resources than will be available even after some processes finish
				{
					isEnding = 0; //we are not ending then
				}
			}

			procFlags[i] = isEnding; //set flag of current proccess to detected state

			if (isEnding == 1)
			{
				updated = 1; //we have updated a process to ending

				for (j = 0; j < 20; j++) //add all of proccess i's resource values to the temporary resource vector
					tempVec[j] += data->alloc[j][i];
			}
		}
	} while (updated == 1); //While we can keep ending processes an updating them to dying...

	free(tempVec); //free our temporary vector of proccess data
}

/* The biggest and fattest function west of the missisipi */
void DoSharedWork()
{
	/* General sched data */
	int activeProcs = 0;
	int exitCount = 0;
	int status;
	int iterator;
	int requestCounter = 0; //not used

	/* Proc toChildQueue and message toChildQueue data */
	int msgsize;

	/* Set shared memory clock value */
	data->sysTime.seconds = 0;
	data->sysTime.ns = 0;

	/* Setup time for random child spawning and deadlock running */
	Time nextExec = {0, 0};
	Time deadlockExec = {0, 0};
	/* Create queues */
	struct Queue *resQueue = createQueue(childCount); //Queue of real PIDS

	while (1)
	{
		AddTime(&(data->sysTime), CLOCK_ADD_INC); //increment clock between tasks to advance the clock a little
		//printf("Wh");
		pid_t pid; //pid temp

		/* Only executes when there is a proccess ready to be launched, given the time is right for exec, there is room in the proc table */
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
				if (VERBOSE_LEVEL == 1 && lineCount++ < MAX_LINES)
					fprintf(o, "%s: [%i:%i] [PROC CREATE] pid: %i\n\n", filen, data->sysTime.seconds, data->sysTime.ns, pid);
				activeProcs++; //increment active execs
			}
			else
			{
				kill(pid, SIGTERM); //if child failed to find a proccess block, just kill it off
			}
		}
		//printf("did proc create");
		if ((msgsize = msgrcv(toMasterQueue, &msgbuf, sizeof(msgbuf), 0, IPC_NOWAIT)) > -1) //non-blocking wait while waiting for child to respond
		{
			if (strcmp(msgbuf.mtext, "REQ") == 0) //If message recieved was a request for resource
			{
				int reqpid = msgbuf.mtype;			 //save its mtype which is the pid of process
				int procpos = FindPID(msgbuf.mtype); //find its position in proc table
				int resID = 0;
				int count = 0;

				msgrcv(toMasterQueue, &msgbuf, sizeof(msgbuf), reqpid, 0); //wait for child to send resource identifier
				resID = atoi(msgbuf.mtext);
				msgrcv(toMasterQueue, &msgbuf, sizeof(msgbuf), reqpid, 0); //wait for child to send resource count
				count = atoi(msgbuf.mtext);
				data->req[resID][procpos] = count;

				//printf("Request for resource ID: %i from proc pos %i with count %i\n", resID, procpos, count);

				if (VERBOSE_LEVEL == 1 && lineCount++ < MAX_LINES)
					fprintf(o, "%s: [%i:%i] [REQUEST] pid: %i proc: %i resID: %i\n", filen, data->sysTime.seconds, data->sysTime.ns, msgbuf.mtype, procpos, resID);

				if (AllocResource(procpos, resID) == -1) //Allocate resource. if -1, failed or not complete. 1 = success.
				{
					//printf("Alloc failed");
					enqueue(resQueue, reqpid); //enqueue into wait queue since failed

					if (VERBOSE_LEVEL == 1 && lineCount++ < MAX_LINES)
						fprintf(o, "\t-> [%i:%i] [REQUEST] pid: %i request unfulfilled...\n\n", data->sysTime.seconds, data->sysTime.ns, msgbuf.mtype);
				}
				else
				{
					pidallocs++;					   //increment alloc counter
					strcpy(msgbuf.mtext, "REQ_GRANT"); //send message that resource has been granted to child
					msgsnd(toChildQueue, &msgbuf, sizeof(msgbuf), IPC_NOWAIT);
					if (VERBOSE_LEVEL == 1 && lineCount++ < MAX_LINES)
						fprintf(o, "\t-> [%i:%i] [REQUEST] pid: %i request fulfilled...\n\n", data->sysTime.seconds, data->sysTime.ns, msgbuf.mtype);
				}
			}
			else if (strcmp(msgbuf.mtext, "REL") == 0) //if release request
			{
				int reqpid = msgbuf.mtype;			 //save pid of child
				int procpos = FindPID(msgbuf.mtype); //lookup child in proc table
				//printf("Waiting on release resource ID");
				msgrcv(toMasterQueue, &msgbuf, sizeof(msgbuf), reqpid, 0); //wait for child to send releasing resource identifier
				DellocResource(procpos, atoi(msgbuf.mtext));			   //delloc the resource
				if (VERBOSE_LEVEL == 1 && lineCount++ < MAX_LINES)
					fprintf(o, "%s: [%i:%i] [RELEASE] pid: %i proc: %i  resID: %i\n\n", filen, data->sysTime.seconds, data->sysTime.ns, msgbuf.mtype, FindPID(msgbuf.mtype), atoi(msgbuf.mtext));
			}
			else if (strcmp(msgbuf.mtext, "TER") == 0) //if termination request
			{
				int procpos = FindPID(msgbuf.mtype); //find cild in proc table

				if (procpos > -1) //if child still exists
				{
					DeleteProc(procpos, resQueue); //delete the child's contents
					if (VERBOSE_LEVEL == 1 && lineCount++ < MAX_LINES)
						fprintf(o, "%s: [%i:%i] [TERMINATE] pid: %i proc: %i\n\n", filen, data->sysTime.seconds, data->sysTime.ns, msgbuf.mtype, FindPID(msgbuf.mtype));
					pidprocterms++;
				}
			}

			if ((requestCounter++) == 19)
			{
				if (VERBOSE_LEVEL == 1 && lineCount++ < MAX_LINES)
					DisplayResources(); //print the every-20 table
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

					if (position > -1) //if we could find the child in the proccess table, set it to unset
						data->proc[position].pid = -1;
				}
			}
		}

		if (CompareTime(&(data->sysTime), &deadlockExec)) //if it is time to check for deadlocks
		{
			deadlockExec.seconds = data->sysTime.seconds; //capture current time
			deadlockExec.ns = data->sysTime.ns;

			AddTimeLong(&deadlockExec, abs((long)(rand() % 1000) * (long)1000000)); //set new exec time to 0 - 1000  ms after now

			int *procFlags; //create empty flags pointer
			int i;

			int deadlockDisplayed = 0; //did we display a deadlock for this instance yet? 1 time switch basically
			int terminated;
			do
			{
				terminated = 0;								 //as long as we terminate a proccess...
				procFlags = calloc(childCount, sizeof(int)); //create a new proc flag vector

				DeadLockDetector(procFlags); //run detection algorithm which returns a array of 0's and 1's based on process positions in the table

				for (i = 0; i < childCount; i++) //for each ith resource
				{

					if (procFlags[i] == 0 && data->proc[i].pid > 0) //if the pid is > 0 meaning the process exists, and the flag was set to 0 meaning it is not going to free up on its own...
					{

						if (deadlockDisplayed == 0) //we have detected that at least a single deadlock exists.
						{
							deadlockCount++; //inc deadlock count, display deadlock state, begin playing sudoku
							deadlockDisplayed = 1;
							if (lineCount++ < MAX_LINES)
							{
								fprintf(o, "********** DEADLOCK DETECTED **********");
								DisplayResources();
								int j;
								fprintf(o, "Deadlocked Procs are as follows:\n [ ");
								for (j = 0; j < childCount; j++)
									if (procFlags[j] == 0)
										fprintf(o, "%i ", j);
								fprintf(o, "]\n");
							}
						}

						terminated = 1;					  //we are terminating a process this run
						msgbuf.mtype = data->proc[i].pid; //send link to gannon's lair with light
						strcpy(msgbuf.mtext, "DIE");
						msgsnd(toChildQueue, &msgbuf, sizeof(msgbuf), IPC_NOWAIT); //send signal
						DeleteProc(i, resQueue);								   //remove the process from the table
						pidprocterms++;
						deadlockProcs++;
						if (lineCount++ < MAX_LINES)
							fprintf(o, "%s: [%i:%i] [KILL SENT] [DEADLOCK BUSTER PRO V1337.420.360noscope edition] pid: %i proc: %i\n\n", filen, data->sysTime.seconds, data->sysTime.ns, data->proc[i].pid, i);
						break;
					}
				}
				free(procFlags);	   //remove the current flag array and create a new one next time
			} while (terminated == 1); //while we are still removing items
									   //The flow is: remove item, check if deadlock still exists, remove the next item, starting the lowest index.
		}

		/* Check the queues if anything can be reenstated now with requested resources... */
		for (iterator = 0; iterator < getSize(resQueue); iterator++)
		{
			int cpid = dequeue(resQueue);				//get realpid from the queue
			int procpos = FindPID(cpid);				//try to find the process in the table
			int resID = FindAllocationRequest(procpos); //get the requested resource

			if (procpos < 0) //if our proccess is no longer in the table, then just skip it and remove it from the queue
			{
				continue;
			}
			else if (AllocResource(procpos, resID) == 1) //the process was in the queue and alive and resources were granted
			{
				if (VERBOSE_LEVEL == 1 && lineCount++ < MAX_LINES)
					fprintf(o, "%s: [%i:%i] [REQUEST] [QUEUE] pid: %i request fulfilled...\n\n", filen, data->sysTime.seconds, data->sysTime.ns, msgbuf.mtype);
				pidallocs++;
				strcpy(msgbuf.mtext, "REQ_GRANT"); //send child signal that it got the resources
				msgbuf.mtype = cpid;
				msgsnd(toChildQueue, &msgbuf, sizeof(msgbuf), IPC_NOWAIT); //send parent termination signal
			}
			else
			{
				enqueue(resQueue, cpid); //the proc exists, but the resources werent granted. Back the looping queue hell
			}
		}

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
	filen = argv[0];					  //shorthand for filename
	srand(time(NULL) ^ (getpid() << 16)); //set random seed, doesn't really seem all that random tho...

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
		\t-v : enable verbose mode. Default: off \n\
		\t-n [count] : max proccesses at the same time. Default: 19\n\n",
				   filen);
			return;
		case 'v': //verbosity settings
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
