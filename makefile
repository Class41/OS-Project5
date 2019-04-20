CC = gcc
CFLAGS = -g -w
TARGET1 = oss
TARGET2 = user
OBJS1 = oss.o
OBJS2 = user.o

all: oss user
$(TARGET1): $(OBJS1)
	$(CC) -o $(TARGET1) $(OBJS1) -lpthread
$(TARGET2): $(OBJS2)
	$(CC) -o $(TARGET2) $(OBJS2) -lpthread
oss.o: oss.c
	$(CC) $(CFLAGS) -c oss.c shared.h
user.o: user.c
	$(CC) $(CFLAGS) -c user.c shared.h
clean:
	/bin/rm -f *.o $(TARGET1) $(TARGET2)
