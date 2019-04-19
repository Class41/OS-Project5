#ifndef SHARED_H

#define MAX_PROCS 19

typedef struct
{
	int pid; //process pid
	char status[50];
} Process;

/* Time structure */
typedef struct
{
	unsigned int seconds;
	unsigned int ns;
} Time;

typedef struct
{
	Time sysTime;
	Process proc[MAX_PROCS]; //process table
	int allocVec[20];
	int resVecProtectorTop[10];
	int resVec[20];
	int resVecProtectorBot[10];
	int req[20][19];
	int alloc[20][19];
	int sharedRes[5];
} Shared;

#define SHARED_H
#endif
