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
*	Project 5: Resource managment
*	Date: 4/16/19
*	Purpose: User process that is managed by oss, will either terminate, use all time, or begin an IO operation which blocks and returns a certain amount of time
*/

/* Constants for termination and using all time--the reason termination is not const is because it changes depending if it is a realtime proccess or not */
int CHANCE_TO_DIE_PERCENT = 1;
const int CHANCE_TO_REQUEST = 65;

/* Housekeeping holders for shared memory and file name alias */
Shared *data;
int toChildQueue;
int toMasterQueue;
int ipcid;
char *filen;
int pid;

/* Function prototypes */
void ShmAttatch();
void QueueAttatch();
void AddTime(Time *time, int amount);
int FindPID(int pid);
int CompareTime(Time *time1, Time *time2);
void AddTimeLong(Time *time, long amount);

/* Message queue standard message buffer */
struct
{
	long mtype;
	char mtext[100];
} msgbuf;

/* Find the proccess block with the given pid and return the position in the array */
int FindPID(int pid)
{
	int i;
	for (i = 0; i < MAX_PROCS; i++)
		if (data->proc[i].pid == pid)
			return i;
	return -1;
}

/* Add time to given time structure, max 2.147billion ns */
void AddTime(Time *time, int amount)
{
	int newnano = time->ns + amount;
	while (newnano >= 1000000000) //nano = 10^9, so keep dividing until we get to something less and increment seconds
	{
		newnano -= 1000000000;
		(time->seconds)++;
	}
	time->ns = newnano; //since ns is < 10^9, it is our new nanoseconds
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

/* Attach to queues incoming/outgoing */
void QueueAttatch()
{
	key_t shmkey = ftok("shmsharemsg", 766);

	if (shmkey == -1) //check if the input file exists
	{
		printf("\n%s: ", filen);
		fflush(stdout);
		perror("Error: Ftok failed");
		return;
	}

	toChildQueue = msgget(shmkey, 0600 | IPC_CREAT); //attach to child queue

	if (toChildQueue == -1)
	{
		printf("\n%s: ", filen);
		fflush(stdout);
		perror("Error: toChildQueue creation failed");
		return;
	}

	shmkey = ftok("shmsharemsg2", 767);

	if (shmkey == -1) //check if the input file exists
	{
		printf("\n%s: ", filen);
		fflush(stdout);
		perror("Error: Ftok failed");
		return;
	}

	toMasterQueue = msgget(shmkey, 0600 | IPC_CREAT); //attach to master queue

	if (toMasterQueue == -1)
	{
		printf("\n%s: ", filen);
		fflush(stdout);
		perror("Error: toMasterQueue creation failed");
		return;
	}
}

/* Attaches to shared memory */
void ShmAttatch() //same exact memory attach function from master minus the init for the semaphores
{
	key_t shmkey = ftok("shmshare", 312); //shared mem key

	if (shmkey == -1) //check if the input file exists
	{
		printf("\n%s: ", filen);
		fflush(stdout);
		perror("Error: Ftok failed");
		return;
	}

	ipcid = shmget(shmkey, sizeof(Shared), 0600 | IPC_CREAT); //get shared mem

	if (ipcid == -1) //check if the input file exists
	{
		printf("\n%s: ", filen);
		fflush(stdout);
		perror("Error: failed to get shared memory");
		return;
	}

	data = (Shared *)shmat(ipcid, (void *)0, 0); //attach to shared mem

	if (data == (void *)-1) //check if the input file exists
	{
		printf("\n%s: ", filen);
		fflush(stdout);
		perror("Error: Failed to attach to shared memory");
		return;
	}
}

void CalcNextActionTime(Time *t)
{
	t->seconds = data->sysTime.seconds;
	t->ns = data->sysTime.ns;
	long mstoadd = (rand() % 251) * 1000000;
	AddTimeLong(t, mstoadd);
}

int getResourceToRelease(int pid)
{
	int myPos = FindPID(pid);
	int i;

	for (i = 0; i < 20; i++)
	{
		if (data->alloc[i][myPos] > 0)
			return i;
	}

	return -1;
}

int main(int argc, int argv)
{
	ShmAttatch();   //attach to shared mem
	QueueAttatch(); //attach to queues

	pid = getpid(); //shorthand for getpid every time from now
	signal(SIGINT, Handler);
	/* Variables to keep tabs on time to be added instead of creating new ints every time */
	Time nextActionTime = {0, 0};

	srand(pid); //ensure randomness by bitshifting and ORing the time based on the pid
	int resToReleasePos;

	while (1)
	{
		strcpy(data->proc[FindPID(pid)].status, "ST NEW LOOP");
		if (CompareTime(&(data->sysTime), &(nextActionTime)) == 1)
		{
			strcpy(data->proc[FindPID(pid)].status, "EN TIME START");
			if ((rand() % 100) <= CHANCE_TO_DIE_PERCENT) //roll for termination
			{
				msgbuf.mtype = pid;
				strcpy(msgbuf.mtext, "TER");
				strcpy(data->proc[FindPID(pid)].status, "SND MSTR TERM");
				msgsnd(toMasterQueue, &msgbuf, sizeof(msgbuf), 0); //send parent termination signal
				strcpy(data->proc[FindPID(pid)].status, "EXT MSTR GOT");
				exit(21);
			}

			resToReleasePos = getResourceToRelease(pid);
			if ((rand() % 100) < CHANCE_TO_REQUEST)
			{
				strcpy(data->proc[FindPID(pid)].status, "EN REQ BLOK");
				int resToRequest = (rand() % 20);

				//data->req[resToRequest][FindPID(pid)]

				msgbuf.mtype = pid;
				strcpy(msgbuf.mtext, "REQ");
				strcpy(data->proc[FindPID(pid)].status, "SND MASTER REQ");
				msgsnd(toMasterQueue, &msgbuf, sizeof(msgbuf), 0);

				char *convert[5];
				sprintf(convert, "%i", resToRequest);

				msgbuf.mtype = pid;
				strcpy(msgbuf.mtext, convert);
				strcpy(data->proc[FindPID(pid)].status, "SND MASTER RES POS");
				msgsnd(toMasterQueue, &msgbuf, sizeof(msgbuf), 0);

				int resCount = abs((rand() % ((data->resVec[resToRequest] - (data->alloc[resToRequest][FindPID(pid)]) + 1))));

				sprintf(convert, "%i", resCount);

				msgbuf.mtype = pid;
				strcpy(msgbuf.mtext, convert);
				strcpy(data->proc[FindPID(pid)].status, "SND MASTER RES CNT");
				msgsnd(toMasterQueue, &msgbuf, sizeof(msgbuf), 0);

				strcpy(data->proc[FindPID(pid)].status, "WAIT MASTER GRANT");

				do
				{
					msgrcv(toChildQueue, &msgbuf, sizeof(msgbuf), pid, 0);

					if (strcmp(msgbuf.mtext, "REQ_GRANT") == 0 || strcmp(msgbuf.mtext, "DIE") == 0)
						break;

				} while (1);

				if (strcmp(msgbuf.mtext, "DIE") == 0)
				{
					CHANCE_TO_DIE_PERCENT = 1000;
					CalcNextActionTime(&nextActionTime);
					printf("\nBegin force die....");
					continue;
				}

				strcpy(data->proc[FindPID(pid)].status, "GOT REQ GRANT");

				CalcNextActionTime(&nextActionTime);

			}
			else if (resToReleasePos >= 0)
			{
				strcpy(data->proc[FindPID(pid)].status, "START RELEASE");
				msgbuf.mtype = pid;
				strcpy(msgbuf.mtext, "REL");
				strcpy(data->proc[FindPID(pid)].status, "SND MASTER REL REQ");
				msgsnd(toMasterQueue, &msgbuf, sizeof(msgbuf), IPC_NOWAIT);

				char *convert[5];
				sprintf(convert, "%i", resToReleasePos);

				strcpy(msgbuf.mtext, convert);
				strcpy(data->proc[FindPID(pid)].status, "SND MASTER RELEASE ID");
				msgsnd(toMasterQueue, &msgbuf, sizeof(msgbuf), 0);
				strcpy(data->proc[FindPID(pid)].status, "MASTER ACCEPT RELEASE ID");
				CalcNextActionTime(&nextActionTime);
			}
			else
			{
				CalcNextActionTime(&nextActionTime);
			}
		}
	}
}
