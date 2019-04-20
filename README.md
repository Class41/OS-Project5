# README

***

Author: Vasyl Onufriyev
Date Finished: 4-20-19

***

## Purpose

oss will spawn off processes at random intervals and listen for requests from those children, and attempt to manage the system resources and allocate to each of them as requests come in to release/terminate/accuire resources.

Children will loop until terminated asking for resources, releasing resources, or doing nothing if resource release is rolled but nothing is releasable.

In the case that a child cannot get a resouce, it enters a resource queue which is checked every OSS iteration and resources are given to proceses as they come in.

In the case that a deadlock occours, the deadlocking algorithm takes center stage and checks for obvious deadlocks at first, then attempts to free up the deadlock by killing the proccess which is located closer to the 0th position of the process table, then checking if that resolved the issue. It continues doing this until no further deadlocks exist.

WARNINGS:

Sometimes the random number generator...isn't quite random. Keep an eye out for that

!!! Important !!!

When you launch oss, it may take longer than 2 seconds. This is is because of I/O. Give it about 15 seconds you will get a termination prompt.
## How to Run
```
$ make
$ ./oss [options]
```

### Options:

```
-h -> show help menu
-n -> how many children should exist at any given time. Max 19/Default 19
-v -> set verbose, see more data on what is happening internally in the output file. default off
```

Output file:

output.log
