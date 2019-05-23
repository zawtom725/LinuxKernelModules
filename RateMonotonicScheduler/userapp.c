#include "userapp.h"

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>
#include <string.h>

/*

	the library function to use my LKM

	time unit: ms
	cmd format: see below

*/

#define REGIST_CMD_FORMAT "R,%d,%u,%u"
#define YIELD_CMD_FORMAT "Y,%d"
#define DEREGIST_CMD_FORMAT "D,%d"

#define READ_SIZE 2048
#define WRITE_SIZE 256

#define ITERATION 6

// input * cost_base = actual cost in milliseconds
#define COST_BASE 31
#define JOB_BASE 10000000

#define MICROSEC 1000000

/*

	The library to use my LKM.

*/

// a global file struct, so that we do not open every time
int fd = -1;

// start & end communication
void start_communicat(void){
	fd = open("/proc/mp2/status", O_RDWR);
}

void terminate_communicat(void){
	if(fd != -1){
		close(fd);
	}
}

// register
void register_process(int pid, unsigned cost, unsigned process){
	char* buf;

	if(fd != -1){
		buf = malloc(WRITE_SIZE);
		sprintf(buf, REGIST_CMD_FORMAT, pid, cost, process);
		printf("register cmd [%s]\n", buf);
		write(fd, buf, strlen(buf));
		free(buf);
	}
}

// return a pointer to the position after newline
char* to_next_newline(char* input){
	while(*input != '\n'){
		input += 1;
	}
	return input + 1;
}

// check whether the id is accepted
bool check_accepted(int pid){
	char* buf;
	char* this_entry_pt;
	int* temp;
	bool ret;

	ret = false;

	if(fd != -1){
		buf = (char*) calloc(READ_SIZE, sizeof(char));
		temp = (int*) malloc(sizeof(int));
		read(fd, buf, READ_SIZE);
		this_entry_pt = buf;

		// process the read
		while(strlen(this_entry_pt) > 0){
			sscanf(this_entry_pt, "%d", temp);
			if(*temp == pid){
				ret = true;
				break;
			}
			this_entry_pt = to_next_newline(this_entry_pt);
		}

		free(temp);
		free(buf);
	}

	return ret;
}

// yield
void yield_process(int pid){
	char* buf;

	if(fd != -1){
		buf = malloc(WRITE_SIZE);
		sprintf(buf, YIELD_CMD_FORMAT, pid);
		printf("yield cmd [%s]\n", buf);
		write(fd, buf, strlen(buf));
		free(buf);
	}
}

// deregister
void deregister_process(int pid){
	char* buf;

	if(fd != -1){
		buf = malloc(WRITE_SIZE);
		sprintf(buf, DEREGIST_CMD_FORMAT, pid);
		printf("deregister cmd [%s]\n", buf);
		write(fd, buf, strlen(buf));
		free(buf);
	}
}


/*

	Time Count helper function
	I run them to determine the COST_BASE

*/


// suseconds_t == long
// return the current microseconds time
suseconds_t get_current_time(void){
	struct timeval* temp;
	suseconds_t ret;

	temp = malloc(sizeof(struct timeval));
	gettimeofday(temp, NULL);
	ret = temp->tv_usec;
	free(temp);

	return ret;
}

// calculate the duration of the job
long calc_elapse(long current, long last){
	if(current > last){
		return current - last;
	}else{
		return current + (MICROSEC - last);
	}
}

void fib(int amount);

// the test program to determine COST_BASE
void test(char* input){
	int i;
	int* job_amount;
	long last;
	long current;
	long elapse;

	printf("test for input [%s]\n", input);


	job_amount = malloc(sizeof(int));
	sscanf(input, "%d", job_amount);

	last = get_current_time();
	printf("job starts\n");

	for(i = 0; i < ITERATION; i++){
		fib(*job_amount);
		current = get_current_time();
		printf("job [%d] finished for [%ld]\n", i, calc_elapse(current, last));
		last = current;
	}

	free(job_amount);
}

/*

	Userapp

*/

// some job: fib
// duration: input * COST_BASE in milliseeconds
void fib(int amount){
	int j;
	long fact;
	// calculate factorial 50
	fact = 1;
	for(j = 1; j < amount * JOB_BASE; j++){
		fact = fact * j;
	}
}

int main(int argc, char* argv[]){
	int period;
	int cost;
	int job_amount;
	int pid;
	int i;
	// timing
	struct timeval base;
	struct timeval wakeup;
	struct timeval yield;
	long sec_count;
	long usec_count;

	if(argc < 3){
		printf("usage: ./userapp [cost in ms] [period in ms]\n");
		return 0;
	}

	sscanf(argv[1], "%d", &cost);
	job_amount = cost / COST_BASE;
	sscanf(argv[2], "%d", &period);
	pid = getpid();

	printf("run job [%d] with cost [%d ms] and period [%d ms]\n", pid, cost, period);

	start_communicat();

	register_process(pid, cost, period);
	if(!check_accepted(pid)){
		printf("job [%d] rejected\n", pid);
		return 0;
	}

	// initial yield
	gettimeofday(&base, NULL);
	yield_process(pid);
	printf("job [%d] accepted\n", pid);

	// job loop
	for(i = 0; i < ITERATION; i++){
		// wake up timing
		gettimeofday(&wakeup, NULL);
		sec_count = wakeup.tv_sec - base.tv_sec;
		usec_count = wakeup.tv_usec - base.tv_usec;
		if(usec_count < 0){
			sec_count -= 1;
			usec_count = MICROSEC + usec_count;
		}
		printf("job [%d] iteration [%d] wakeup [%zu s %zu us] after the beginning\n", pid, i, sec_count, usec_count);

		fib(job_amount);

		// working timing
		gettimeofday(&yield, NULL);
		sec_count = yield.tv_sec - wakeup.tv_sec;
		usec_count = yield.tv_usec - wakeup.tv_usec;
		if(usec_count < 0){
			sec_count -= 1;
			usec_count = MICROSEC + usec_count;
		}
		printf("job [%d] iteration [%d] finished for [%zu s %zu us] after wakeup\n", pid, i, sec_count, usec_count);

		yield_process(pid);
	}

	// done
	printf("job [%d] finished\n", pid);
	deregister_process(pid);

	terminate_communicat();
}
