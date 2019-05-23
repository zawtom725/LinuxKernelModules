#define LINUX

#include "mp2_given.h"

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/spinlock.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ziangw2");
MODULE_DESCRIPTION("CS-423 MP2");

// proc file system names & globals
#define PROC_DIR_NAME "mp2"
#define PROC_FILE_NAME "status"

#define PROC_READ_BUF_SIZE 2048

static struct proc_dir_entry *mp2_proc_dir = NULL;
static struct proc_dir_entry *mp2_proc_entry = NULL;

// the flag used to handle proc_read
#define UNREAD 0
#define READ_DONE 1

// command format
#define REGIST_CMD_FORMAT "R,%d,%u,%u"
#define YIELD_CMD_FORMAT "Y,%d"
#define DEREGIST_CMD_FORMAT "D,%d"

// register, deregister: linked list entry
typedef struct mp2_list_entry_t {
	struct list_head head;

	struct task_struct* pcb_pt;

	int state; // run 0, ready 1, sleep 2
	int pid;

	// keep these in jiffies
	unsigned long period;
	unsigned long next_period;
	unsigned long cost;

	// compute once
	unsigned int load;
	
	// the timer used by yield
	struct timer_list wakeup_timer;
} mp2_list_entry;
// struct access macros
#define list_head_ptr(entry) ( &(entry->head) )
#define pcb_ptr(entry) ( entry->pcb_pt )
#define timer_ptr(entry) ( &(entry->wakeup_timer) )

// the state of the process
#define STATE_RUNNING_CODE 	0
#define STATE_RUNNING_STR	"running"
#define STATE_READY_CODE 	1
#define STATE_READY_STR		"ready"
#define STATE_SLEEPING_CODE	2
#define STATE_SLEEPING_STR 	"sleeping"

// the mp2 linked list & list lock
static mp2_list_entry* regist_head = NULL;
static spinlock_t list_lock;

// admission control global & helper macro
static unsigned int current_load = 0;
// to immitate \sigma{c/p} < 0.693 ==> 1000*c/p < 693, +1 to make the condition a little stricter
#define compute_load(cost, period) ((1000 * cost / period) + 1)

// scheduling globals
static mp2_list_entry* running_process_pt = NULL;
static struct task_struct* dispath_thread_pcb_pt = NULL;

// compile flag
#define DEBUG 		1	// define DEBUG to have rich printk messages
// #define ECHO_TEST 	1	// define ECHO_TEST to test the module with fake process ids


/*

	PCB Augmentation and Linked List

*/
// used in register_process, invoked when the timer wakes up (real time job comes)
void _timer_func(unsigned long entry_pt){
	mp2_list_entry* this_entry;

	this_entry = (mp2_list_entry*) entry_pt;

	#ifdef DEBUG
	printk(KERN_ALERT "timer_func called for [%d]\n", this_entry->pid);
	#endif

	// set to ready and invoke the dispatch thread
	this_entry->state = STATE_READY_CODE;
	wake_up_process(dispath_thread_pcb_pt);
	// NOTE: no need to call schedule() here, will also lead to a BUG
}

// register a new process, linked list insert
void register_process(int* pid_int_pt, unsigned int* period_ms_pt, unsigned int* comput_cost_ms_pt){
	mp2_list_entry* new_entry;
	unsigned int this_load;

	#ifdef DEBUG
	printk(KERN_ALERT "insert [%d] with period [%u] cost [%u]\n", *pid_int_pt, *period_ms_pt, *comput_cost_ms_pt);
	#endif

	spin_lock(&list_lock);

	// init the new entry
	new_entry = kmalloc(sizeof(mp2_list_entry), GFP_KERNEL);
	new_entry->pid = *pid_int_pt;
	new_entry->pcb_pt = find_task_by_pid(new_entry->pid);
	new_entry->state = STATE_SLEEPING_CODE;
	new_entry->period = msecs_to_jiffies(*period_ms_pt);
	new_entry->next_period = 0; // set after first yield
	new_entry->cost = msecs_to_jiffies(*comput_cost_ms_pt);

	// set up timer
	setup_timer(timer_ptr(new_entry), _timer_func, (unsigned long) new_entry);
	
	// compute load - do it before converted to jiffies
	this_load = compute_load(*comput_cost_ms_pt, *period_ms_pt);
	new_entry->load = this_load;

	#ifdef DEBUG
	printk(KERN_ALERT "alloc entry [%p] with pcb_ptr [%p]\n", new_entry, pcb_ptr(new_entry));
	#endif

	// admission control
	if(this_load + current_load > 693){
		// admission denied
		kfree(new_entry);

		#ifdef DEBUG
		printk(KERN_ALERT "insert denied with current load [%u] this load [%u]\n", current_load, this_load);
		#endif
	}else{
		// add this to the linked list
		list_add(list_head_ptr(new_entry), list_head_ptr(regist_head));
		current_load += this_load;

		#ifdef DEBUG
		printk(KERN_ALERT "inserted at [%p] after insert current load [%u]\n", new_entry, current_load);
		#endif
	}

	spin_unlock(&list_lock);
}

// yield a new process
void yield_process(int* pid_int_pt){
	mp2_list_entry* this_process;
	struct list_head* pos;

	#ifdef DEBUG
	printk(KERN_ALERT "yield process [%d]\n", *pid_int_pt);
	#endif

	spin_lock(&list_lock);
	// list traversal, it is a for loop
	list_for_each(pos, list_head_ptr(regist_head) ){
		this_process = (mp2_list_entry*) pos;

		// yielding this
		if(this_process->pid == *pid_int_pt){
			// terminate if it is running
			this_process->state = STATE_SLEEPING_CODE;
			if(running_process_pt == this_process){
				running_process_pt = NULL;
			}

			// calculate and set the next timer
			if(this_process->next_period == 0){
				// newly registered, immediately ready
				this_process->next_period = jiffies;
			}else{
				// finished job, if no missing jobs, this loop will only run once
				while(this_process->next_period <= jiffies){
					this_process->next_period += this_process->period;
				}	
			}

			// set the timer to wake up for the next period
			mod_timer(timer_ptr(this_process), this_process->next_period);

			// set this process to sleeping
			#ifndef ECHO_TEST
			set_task_state(pcb_ptr(this_process), TASK_UNINTERRUPTIBLE);
			#endif

			break;
		}	
	}

	spin_unlock(&list_lock);

	// wake up the dispatch thread to schedule a new job
	wake_up_process(dispath_thread_pcb_pt);
	schedule();
}

// deregister
void deregister_process(int* pid_int_pt){
	struct list_head* pos;
	struct list_head* temp;
	mp2_list_entry* this_process;
	bool schedule_another;

	#ifdef DEBUG
	printk(KERN_ALERT "deregister_process [%d]\n", *pid_int_pt);
	#endif

	schedule_another = false;

	spin_lock( &list_lock );

	// safe traversal with memory freed
	list_for_each_safe(pos, temp, list_head_ptr(regist_head) ){
		this_process = (mp2_list_entry*) pos;

		if(this_process->pid == *pid_int_pt){
			// load decreasing
			current_load -= this_process->load;
			// stop the timer
			del_timer(timer_ptr(this_process));
			// schedule another process if the current running one stopped
			if(running_process_pt == this_process){
				running_process_pt = NULL;
				schedule_another = true;
			}
			// remove from  the linked list
			list_del(pos);

			#ifdef DEBUG
			printk(KERN_ALERT "remove pid [%d] afterwards current load [%u]\n", this_process->pid, current_load);
			#endif

			kfree(this_process);
			break;
		}
	}

	spin_unlock( &list_lock );

	if(schedule_another){
		wake_up_process(dispath_thread_pcb_pt);
		schedule();
	}
}

// util func: get the ready process with the highest priority
mp2_list_entry* get_highest_prio_ready_proc(void){
	unsigned long smallest_period;
	mp2_list_entry* this_process;
	mp2_list_entry* ret_pt;
	struct list_head* pos;

	#ifdef DEBUG
	printk(KERN_ALERT "get_highest_prio_ready_proc called\n");
	#endif

	spin_lock(&list_lock);
	smallest_period = 0;

	ret_pt = NULL;
	// list traversal, it is a for loop
	list_for_each(pos, list_head_ptr(regist_head) ){
		this_process = (mp2_list_entry*) pos;

		// ready one
		if(this_process->state == STATE_READY_CODE){
			if(smallest_period == 0){
				// first ready process
				smallest_period = this_process->period;
				ret_pt = this_process;
			}else{
				// comparison needed
				if(this_process->period < smallest_period){
					smallest_period = this_process->period;
					ret_pt = this_process;
				}
			}
		}	
	}

	spin_unlock(&list_lock);

	#ifdef DEBUG
	if(ret_pt == NULL){
		printk(KERN_ALERT "no ready process\n");
	}else{
		printk(KERN_ALERT "ready [%d] period [%lu]\n", ret_pt->pid, ret_pt->period);
	}
	#endif

	return ret_pt;
}

// read all the current registered process, into the buffer, return the num of bytes read
ssize_t read_all_registered(char* buf, size_t buf_len){
	char* temp;
	size_t remain_len;
	ssize_t total;
	struct list_head* pos;
	mp2_list_entry* this_process;
	int printed_len;
	char* state_str;

	#ifdef DEBUG
	printk(KERN_ALERT "read_all_registered called [%p][%zu]\n", buf, buf_len);
	#endif

	spin_lock( &list_lock );

	// for str formatting
	temp = buf;
	remain_len = buf_len;
	total = 0;

	// list traversal, it is a for loop
	list_for_each(pos, list_head_ptr(regist_head) ){
		this_process = (mp2_list_entry*) pos;

		// use snprintf, defined in linux/kernel.h, not worried about flip page
		if(this_process->state == STATE_SLEEPING_CODE){
			state_str = STATE_SLEEPING_STR;
		}else if(this_process->state == STATE_READY_CODE){
			state_str = STATE_READY_STR;
		}else{
			state_str = STATE_RUNNING_STR;
		}

		printed_len = snprintf(temp, remain_len, "%d,%s,%lu,%lu\n", this_process->pid, state_str,
			this_process->cost, this_process->period);
		
		// update positions
		total += printed_len;
		temp += printed_len;
		remain_len -= printed_len;

		// defense against buffer overflow
		if(remain_len <= 1){
		 	break;
		}
	}

	spin_unlock(&list_lock);

	#ifdef DEBUG
	printk(KERN_ALERT "read_all_registered finished [%s]\n", buf);
	#endif

	return total;
}

// init the linked list & the spin lock
void init_linked_list(void){
	#ifdef DEBUG
	printk(KERN_ALERT "init_linked_list called\n");
	#endif

	spin_lock_init(&list_lock);

	spin_lock(&list_lock);

	regist_head = (mp2_list_entry*) kmalloc(sizeof(mp2_list_entry), GFP_KERNEL);
   	INIT_LIST_HEAD( list_head_ptr(regist_head) );
   	regist_head->pid = -1;

   	#ifdef DEBUG
   	printk(KERN_ALERT "alloc [%p]\n", regist_head);
   	#endif

   	spin_unlock(&list_lock);
}

// free the linked list
void free_linked_list(void){
	struct list_head* pos;
	mp2_list_entry* this_process;

	#ifdef DEBUG
	printk(KERN_ALERT "free_linked_list called\n");
	#endif

	spin_lock(&list_lock);

	// my own version of traversal, free resource in place
	for(pos = list_head_ptr(regist_head)->next; pos != list_head_ptr(regist_head); /*increment is done in the loop body*/){
		this_process = (mp2_list_entry*) pos;
		// stop the timer
		del_timer(timer_ptr(this_process));
		pos = pos->next;

		#ifdef DEBUG
		printk(KERN_ALERT "free [%p]\n", this_process);
		#endif

		kfree(this_process);
	}

	#ifdef DEBUG
	printk(KERN_ALERT "free [%p]\n", regist_head);
	#endif

	// free the list head
	kfree(regist_head);

	spin_unlock(&list_lock);
	// static variable list_lock automatically freed after the program terminates
}


/*

	Dispatching Thread and Timer

*/
// the main thread body
int dispatch_thread_func(void *unused){
	#ifndef ECHO_TEST
	struct sched_param sparam;
	#endif

	mp2_list_entry* highest_ready;

	#ifdef DEBUG
	printk(KERN_ALERT "dispatch thread launched\n");
	#endif

    while (!kthread_should_stop()){
    	#ifdef DEBUG
    	printk(KERN_ALERT "dispatching\n");
    	#endif

    	highest_ready = get_highest_prio_ready_proc();

    	if(highest_ready != NULL){
    		if(running_process_pt == NULL){
    			// none current running, set this one to run
    			highest_ready->state = STATE_RUNNING_CODE;
    			running_process_pt = highest_ready;
    			
    			#ifdef DEBUG
    			printk(KERN_ALERT "running [%d]\n", highest_ready->pid);
    			#endif

    			#ifndef ECHO_TEST
    			wake_up_process(pcb_ptr(running_process_pt));
    			sparam.sched_priority = 99;
				sched_setscheduler(pcb_ptr(running_process_pt), SCHED_FIFO, &sparam);
    			#endif
    		}else if(running_process_pt->period > highest_ready->period){
    			// preempyt current one
    			/*
					NOTE: the documentation's impl will lead to two processes running concurrently
					Therefore, I think it would make more sense to explicitly sleep the current one
    			*/
    			running_process_pt->state = STATE_READY_CODE;
    			highest_ready->state = STATE_RUNNING_CODE;
    			
    			#ifdef DEBUG
    			printk(KERN_ALERT "switching from [%d] to [%d]\n", running_process_pt->pid, highest_ready->pid);
    			#endif

    			// set current running to sleep, set highest_ready to run
    			#ifndef ECHO_TEST
    			set_task_state(pcb_ptr(running_process_pt), TASK_UNINTERRUPTIBLE);
    			sparam.sched_priority = 0;
				sched_setscheduler(pcb_ptr(running_process_pt), SCHED_NORMAL, &sparam);
    			
    			wake_up_process(pcb_ptr(highest_ready));
    			sparam.sched_priority = 99;
				sched_setscheduler(pcb_ptr(highest_ready), SCHED_FIFO, &sparam);
    			#endif

    			running_process_pt = highest_ready;
    		}else{
    			#ifdef DEBUG
    			printk(KERN_ALERT "current [%d] keep running\n", running_process_pt->pid);
    			#endif
    		}
    		
    	}
    	// else: nothing ready, preserver currently running stuff
    	
    	// interruptible sleep for the dispatch thread is enough
    	set_current_state(TASK_INTERRUPTIBLE);
		schedule();
    }

    #ifdef DEBUG
    printk(KERN_ALERT "dispatch thread stopped\n");
    #endif

    return 0;
}

// init the dispatch thread
void _launch_dispatch_thread(void){
	dispath_thread_pcb_pt = kthread_run(dispatch_thread_func, NULL, "dispatch");
	// wake this up to get a message printed
	wake_up_process(dispath_thread_pcb_pt);
	schedule();
}

// clean the dispatch thread
void _stop_dispatch_thread(void){
	kthread_stop(dispath_thread_pcb_pt);
	// wake dispatch thread to let it terminate
	wake_up_process(dispath_thread_pcb_pt);
	schedule();
}

/*

	Proc File System

*/
static ssize_t mp2_proc_read(struct file* file, char __user* buffer, size_t count, loff_t* offset){
	char *buf;
	ssize_t total;

   	#ifdef DEBUG
   	printk(KERN_ALERT "mp2_proc_read called: [%zu]\n", count);
   	#endif

	// if read, return 0 to terminate the reading process
	if(*offset == READ_DONE){
		*offset = UNREAD;
		return 0;
	}

	// not done - read
	buf = (char*) kmalloc(PROC_READ_BUF_SIZE, GFP_KERNEL);
	total = read_all_registered(buf, PROC_READ_BUF_SIZE);

	copy_to_user(buffer, buf ,total);

	kfree(buf);
	// flag to not to infinite loop
	*offset = READ_DONE;
	return total;
}

static ssize_t mp2_proc_write(struct file* file, const char __user* buffer, size_t count, loff_t* data){
	char *buf;
	int* pid_int_pt;
	unsigned int* period_ms_lu_pt;
	unsigned int* comput_cost_ms_lu_pt;

	#ifdef DEBUG
	printk(KERN_ALERT "mp2_proc_write called\n");
	#endif

	// get to kernel space
	buf = (char*) kmalloc(count + 1, GFP_KERNEL);
	copy_from_user(buf, buffer, count);
	buf[count] = '\0';

	#ifdef DEBUG
	printk(KERN_ALERT "buf: [%s]\n", buf);
	#endif

	pid_int_pt = kmalloc(sizeof(int), GFP_KERNEL);
	period_ms_lu_pt = kmalloc(sizeof(unsigned long), GFP_KERNEL);
	comput_cost_ms_lu_pt = kmalloc(sizeof(unsigned long), GFP_KERNEL);

	if(sscanf(buf, REGIST_CMD_FORMAT, pid_int_pt, comput_cost_ms_lu_pt, period_ms_lu_pt) == 3){
		#ifdef DEBUG
		printk(KERN_ALERT "register [%d] with period [%u] cost [%u]\n", *pid_int_pt, *period_ms_lu_pt, *comput_cost_ms_lu_pt);
		#endif

		register_process(pid_int_pt, period_ms_lu_pt, comput_cost_ms_lu_pt);
	}else if(sscanf(buf, YIELD_CMD_FORMAT, pid_int_pt) == 1){
		#ifdef DEBUG
		printk(KERN_ALERT "yield [%d]\n", *pid_int_pt);
		#endif

		yield_process(pid_int_pt);
	}else if(sscanf(buf, DEREGIST_CMD_FORMAT, pid_int_pt) == 1){
		#ifdef DEBUG
		printk(KERN_ALERT "deregister [%d]\n", *pid_int_pt);
		#endif

		deregister_process(pid_int_pt);
	}
	// do nothing if error formatted input

   	// free the temp buf and return success signal
   	kfree(buf);
   	kfree(pid_int_pt);
   	kfree(period_ms_lu_pt);
   	kfree(comput_cost_ms_lu_pt);
   	return count;
}

static const struct file_operations mp2_proc_file_callbacks = {
   .owner = THIS_MODULE,
   .read = mp2_proc_read,
   .write = mp2_proc_write
};

// make proc file
void _create_proc_mp2_status(void){
	mp2_proc_dir = proc_mkdir(PROC_DIR_NAME, NULL);
	mp2_proc_entry = proc_create(PROC_FILE_NAME, 0666, mp2_proc_dir, &mp2_proc_file_callbacks);
}

// remove the proc file
void _delete_proc_mp2_status(void){
   	remove_proc_entry(PROC_FILE_NAME, mp2_proc_dir);
   	remove_proc_entry(PROC_DIR_NAME, NULL);
}


/*

	Init and Exit

*/
// mp2_init - Called when module is loaded
int __init mp2_init(void){
	#ifdef DEBUG
	printk(KERN_ALERT "MP2 MODULE LOADING, time: [%lu]\n", jiffies);
	#endif

	init_linked_list();

	_create_proc_mp2_status();

	_launch_dispatch_thread();

	// done loading
	printk(KERN_ALERT "MP2 MODULE LOADED\n");
	return 0;   
}

// mp2_exit - Called when module is unloaded
void __exit mp2_exit(void){
	#ifdef DEBUG
	printk(KERN_ALERT "MP2 MODULE UNLOADING, time [%lu]\n", jiffies);
	#endif

	_stop_dispatch_thread();

	_delete_proc_mp2_status();

	free_linked_list();

	// done unloading
	printk(KERN_ALERT "MP2 MODULE UNLOADED\n");
}


// Register init and exit funtions
module_init(mp2_init);
module_exit(mp2_exit);