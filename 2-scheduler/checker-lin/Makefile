all: build

libscheduler.so: build

build: so_scheduler.o 
	gcc -shared so_scheduler.o -o libscheduler.so -Wall -g -ggdb

so_scheduler.o: so_scheduler.c
	gcc -Wall -fPIC -g so_scheduler.c -c -o so_scheduler.o -ggdb

clean:
	rm *.o libscheduler.so
