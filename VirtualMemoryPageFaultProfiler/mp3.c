#define LINUX

#include "mp3_given.h"
// module
#include <linux/module.h>
#include <linux/kernel.h>
// mem allocation
#include <linux/slab.h>
#include <linux/uaccess.h>
// proc file 
#include <linux/fs.h>
#include <linux/proc_fs.h>
// linked list, pcb, lock
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
// vmalloc, PG_reserved
#include <linux/vmalloc.h>
#include <linux/page-flags.h>
#include <linux/mm.h>
// delayed work queue
#include <linux/jiffies.h>
#include <linux/workqueue.h>
// character device
#include <linux/device.h>
#include <linux/mm_types.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ziangw2");
MODULE_DESCRIPTION("CS-423 MP3");


// compile flag
//#define DEBUG 1


// slab allocation for registering process & rw buffer
struct kmem_cache *mp3_entry_slab = NULL;
struct kmem_cache *mp3_write_buf_slab = NULL;


// proc file system names & globals
#define PROC_DIR_NAME 		"mp3"
#define PROC_FILE_NAME 		"status"
#define PROC_READ_BUF_SIZE 	1024
#define PROC_WRITE_BUF_SIZE 32
// proc_read flag
#define PROC_READ_DONE 		1
#define PROC_UNREAD 		0
// proc fs entry - for create and delete
static struct proc_dir_entry *mp3_proc_dir = NULL;
static struct proc_dir_entry *mp3_proc_entry = NULL;
// command format
#define CMD_FORMAT_REGIST 	"R %d"
#define CMD_FORMAT_UNREGIST "U %d"


// linked list entry
typedef struct mp3_list_entry_t {
	struct list_head head;

	struct task_struct *pcb_ptr;
	int pid;

	unsigned long cpu_util;
	unsigned long major_fault_count;
	unsigned long minor_fault_count;
} mp3_list_entry;
// access marcos
#define list_head_ptr(entry) ( &(entry->head) )
// entry count
static unsigned mp3_list_length = 0;
// the mp3 linked list & list lock
static mp3_list_entry *regist_head = NULL;
static spinlock_t list_lock;


// 128 * 4KB memory buffer
#define VIRTUAL_BUF_PAGE_NUM	128
#define VM_PAGE_SIZE 			4096
#define VIRTUAL_BUF_SIZE 		(VIRTUAL_BUF_PAGE_NUM * VM_PAGE_SIZE)
static char *virtual_mem_buf = NULL;
// filled sample count
static unsigned MAX_SAMPLE_ALLOWED = ( VIRTUAL_BUF_SIZE / (4*sizeof(unsigned long)) );
static unsigned sampling_count = 0;
static unsigned long *sampling_position_ptr = NULL;


// work queue & memory
#define WQ_TIME_INTERVAL 50 // ms
static struct workqueue_struct *mp3_wq = NULL;
static struct delayed_work *mp3_work_ptr = NULL;
// update macro
#define STILL_RUNNING 	0
// list length cooperate macro
#define WQ_STOP 	0
#define WQ_QUEUE 	1
// work queue status, it is only updated when list_lock is locked by exactly one func
static int wq_status = WQ_STOP;


// character device macros & globals
#define DEVICE_NAME 	"mp3_device"
static int mp3_major_num = 0;


/*

	Delayed Work Queue

*/
void _queue_work(void);
void _report_terminate(void);
// collect the status information in buffer, linked list traversal
void update_virtual_mem_buf(struct work_struct *data){
   	struct list_head *pos;
   	struct list_head *temp;

   	mp3_list_entry *this_entry;
   	unsigned long this_utime;
   	unsigned long this_stime;
   	unsigned long this_maj_flt;
   	unsigned long this_min_flt;

   	unsigned long acc_cpu_util;
   	unsigned long acc_maj_flt;
   	unsigned long acc_min_flt;

   	#ifdef DEBUG
   	printk(KERN_ALERT "update_virtual_mem_buf called\n");
   	#endif

   	acc_cpu_util = 0;
   	acc_maj_flt = 0;
   	acc_min_flt = 0;

   	spin_lock(&list_lock);
	  
   	// safe traversal with memory freed
   	list_for_each_safe(pos, temp, list_head_ptr(regist_head) ){
		this_entry = (mp3_list_entry*) pos;

		// running or not
		if(get_cpu_use(this_entry->pid, &this_min_flt,
			&this_maj_flt, &this_utime, &this_stime) == STILL_RUNNING){
			acc_cpu_util += (this_utime + this_stime);
			acc_maj_flt += this_maj_flt;
			acc_min_flt += this_min_flt;
		}else{
			// remove from the linked list
			list_del(pos);

			#ifdef DEBUG
			printk(KERN_ALERT "free [%p]\n", this_entry);
			#endif

			kmem_cache_free(mp3_entry_slab, this_entry);
			mp3_list_length -= 1;
		}
   	}

   	// whether to report or not : sth left running
   	if(mp3_list_length > 0 && sampling_count < MAX_SAMPLE_ALLOWED){
   		// report
   		sampling_position_ptr[0] = jiffies;
   		sampling_position_ptr[1] = acc_min_flt;
   		sampling_position_ptr[2] = acc_maj_flt;
   		sampling_position_ptr[3] = acc_cpu_util;

   		#ifdef DEBUG
   		printk(KERN_ALERT "report [%lu] min_flt:[%lu] maj_flt:[%lu] cpu:[%lu]\n", jiffies, acc_min_flt, acc_maj_flt, acc_cpu_util);
   		#endif

   		sampling_count += 1;
   		sampling_position_ptr += 4;
   	}else{
   		// stop reporting
   		wq_status = WQ_STOP;
   	}

   	spin_unlock(&list_lock);
   	
   	// keep queuing if no status change
   	if(wq_status == WQ_QUEUE){
   		_queue_work();
	}else{
		_report_terminate();
	}
}

// init the delayed work queue, reuse the delayed_work struct
void _init_work_queue(void){
	mp3_wq = create_workqueue("mp3_wq");
	mp3_work_ptr = kmalloc(sizeof(struct delayed_work), GFP_KERNEL);
	wq_status = WQ_STOP;

	// init the work struct
	INIT_DELAYED_WORK(mp3_work_ptr, update_virtual_mem_buf);
}

// queue work
void _queue_work(void){
	#ifdef DEBUG
	printk(KERN_ALERT "_queue_work called\n");
	#endif

	queue_delayed_work(mp3_wq, mp3_work_ptr, msecs_to_jiffies(WQ_TIME_INTERVAL));
}

// stop queuing work by canceling the latest work
void _stop_work(void){
	#ifdef DEBUG
	printk(KERN_ALERT "_stop_work called\n");
	#endif

	cancel_delayed_work_sync(mp3_work_ptr);
	// after cancellation synchronized, report termination
	_report_terminate();
}

void _report_terminate(void){
	// report a terminating mark
	sampling_position_ptr[0] = -1;
	sampling_position_ptr[1] = -1;
	sampling_position_ptr[2] = -1;
	sampling_position_ptr[3] = -1;

	sampling_count += 1;
	sampling_position_ptr += 4;
}

// flush then destroy the work queue
void _destroy_work_queue(void){
	wq_status = WQ_STOP;
	flush_workqueue(mp3_wq);
   	destroy_workqueue(mp3_wq);
   	kfree(mp3_work_ptr);
}

/*

	Linked list

*/
// register a new process, linked list insert
void register_process(int pid_int){
	mp3_list_entry *new_entry;
	bool queue_work;

	#ifdef DEBUG
	printk(KERN_ALERT "insert [%d]\n", pid_int);
	#endif

	spin_lock(&list_lock);

	// init the new entry
	new_entry = kmem_cache_alloc(mp3_entry_slab, SLAB_PANIC);
	new_entry->pid = pid_int;
	new_entry->pcb_ptr = find_task_by_pid(new_entry->pid);
	new_entry->cpu_util = 0;
	new_entry->major_fault_count = 0;
	new_entry->minor_fault_count = 0;

	// add this to the linked list
	list_add(list_head_ptr(new_entry), list_head_ptr(regist_head));

	#ifdef DEBUG
	printk(KERN_ALERT "inserted at [%p]\n", new_entry);
	#endif

	if(mp3_list_length == 0 && wq_status == WQ_STOP){
		queue_work = true;
		wq_status = WQ_QUEUE;
	}else{
		queue_work = false;
	}
	mp3_list_length += 1;

	spin_unlock(&list_lock);

	// start queuing
	if(queue_work == true){
		_queue_work();
	}
}

// unregister
void unregister_process(int pid_int){
	struct list_head *pos;
	struct list_head *temp;
	mp3_list_entry *this_entry;

	bool stop_work;

	#ifdef DEBUG
	printk(KERN_ALERT "unregister_process [%d]\n", pid_int);
	#endif

	spin_lock(&list_lock);

	// safe traversal with memory freed
	list_for_each_safe(pos, temp, list_head_ptr(regist_head)){
		this_entry = (mp3_list_entry*) pos;

		if(this_entry->pid == pid_int){;
			// remove from  the linked list
			list_del(pos);

			#ifdef DEBUG
			printk(KERN_ALERT "free [%p]\n", this_entry);
			#endif

			kmem_cache_free(mp3_entry_slab, this_entry);
			mp3_list_length -= 1;
			break;
		}
	}

	if(mp3_list_length == 0 && wq_status == WQ_QUEUE){
		wq_status = WQ_STOP;
		stop_work = true;
	}else{
		stop_work = false;
	}

	spin_unlock(&list_lock);

	// mandatory stop
	if(stop_work == true){
		_stop_work();
	}
}

// read all the current registered process, into the buffer, return the num of bytes read
ssize_t read_all_registered(char *buf, size_t buf_len){
	char *temp;
	size_t remain_len;
	ssize_t total;
	int printed_len;

	struct list_head *pos;
	mp3_list_entry *this_entry;

	#ifdef DEBUG
	printk(KERN_ALERT "read_all_registered called\n");
	#endif

	spin_lock(&list_lock);

	// for str formatting
	temp = buf;
	remain_len = buf_len;
	total = 0;

	// list traversal, it is a for loop
	list_for_each(pos, list_head_ptr(regist_head)){
		this_entry = (mp3_list_entry*) pos;

		printed_len = snprintf(temp, remain_len, "%d\n", this_entry->pid);
		
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

	regist_head = (mp3_list_entry*) kmem_cache_alloc(mp3_entry_slab, SLAB_PANIC);
   	INIT_LIST_HEAD(list_head_ptr(regist_head));
   	regist_head->pid = -1;

   	mp3_list_length = 0;

   	#ifdef DEBUG
   	printk(KERN_ALERT "alloc [%p]\n", regist_head);
   	#endif

   	spin_unlock(&list_lock);
}

// free the linked list
void free_linked_list(void){
	struct list_head *pos;
	mp3_list_entry *this_entry;

	#ifdef DEBUG
	printk(KERN_ALERT "free_linked_list called\n");
	#endif

	spin_lock(&list_lock);

	// my own version of traversal, free resource in place
	for(pos = list_head_ptr(regist_head)->next; pos != list_head_ptr(regist_head);
		/*increment is done in the loop body*/){
		this_entry = (mp3_list_entry*) pos;
		pos = pos->next;

		#ifdef DEBUG
		printk(KERN_ALERT "free [%p]\n", this_entry);
		#endif

		kmem_cache_free(mp3_entry_slab, this_entry);
	}

	#ifdef DEBUG
	printk(KERN_ALERT "free [%p]\n", regist_head);
	#endif
	// free the list head
	kmem_cache_free(mp3_entry_slab, regist_head);

	spin_unlock(&list_lock);
	// static variable list_lock automatically freed after the program terminates
}


/*

	Proc File System & Slabs

*/
static ssize_t mp3_proc_read(struct file* file, char __user* buffer, size_t count, loff_t* offset){
	char *buf;
	ssize_t total;

   	#ifdef DEBUG
   	printk(KERN_ALERT "mp3_proc_read called: [%zu]\n", count);
   	#endif

	// if read, return 0 to terminate the reading process
	if(*offset == PROC_READ_DONE){
		*offset = PROC_UNREAD;
		return 0;
	}

	// not done - read
	buf = (char*) kmalloc(PROC_READ_BUF_SIZE, GFP_KERNEL);
	total = read_all_registered(buf, PROC_READ_BUF_SIZE);
	copy_to_user(buffer, buf ,total);
	kfree(buf);

	// flag to not to infinite loop
	*offset = PROC_READ_DONE;
	return total;
}

static ssize_t mp3_proc_write(struct file* file, const char __user* buffer, size_t count, loff_t* data){
	char *buf;
	int pid_int;

	#ifdef DEBUG
	printk(KERN_ALERT "mp3_proc_write called\n");
	#endif

	if(count >= PROC_WRITE_BUF_SIZE - 1){
		// malformed for sure
		return count;
	}

	// get to kernel space
	buf = (char*) kmem_cache_alloc(mp3_write_buf_slab, SLAB_PANIC);
	copy_from_user(buf, buffer, count);
	buf[count] = '\0';

	#ifdef DEBUG
	printk(KERN_ALERT "buf: [%s]\n", buf);
	#endif

	if(sscanf(buf, CMD_FORMAT_REGIST, &pid_int) == 1){
		#ifdef DEBUG
		printk(KERN_ALERT "register [%d]\n", pid_int);
		#endif

		register_process(pid_int);
	}else if(sscanf(buf, CMD_FORMAT_UNREGIST, &pid_int) == 1){
		#ifdef DEBUG
		printk(KERN_ALERT "unregister [%d]\n", pid_int);
		#endif

		unregister_process(pid_int);
	}
	// do nothing if error formatted input

   	// free the temp buf and return success signal
   	kmem_cache_free(mp3_write_buf_slab, buf);
   	return count;
}

static const struct file_operations mp3_proc_file_callbacks = {
   .owner = THIS_MODULE,
   .read = mp3_proc_read,
   .write = mp3_proc_write
};

// make proc file
void _create_proc_mp3_status(void){
	mp3_proc_dir = proc_mkdir(PROC_DIR_NAME, NULL);
	mp3_proc_entry = proc_create(PROC_FILE_NAME, 0666, mp3_proc_dir, &mp3_proc_file_callbacks);
}

// remove the proc file
void _delete_proc_mp3_status(void){
   	remove_proc_entry(PROC_FILE_NAME, mp3_proc_dir);
   	remove_proc_entry(PROC_DIR_NAME, NULL);
}

// init all the used slabs
void _init_mp3_memory(void){
	struct page *this_page;
	int page_id;

	mp3_entry_slab = KMEM_CACHE(mp3_list_entry_t, SLAB_PANIC);
	mp3_write_buf_slab = kmem_cache_create("write_buf", PROC_WRITE_BUF_SIZE,
		PROC_WRITE_BUF_SIZE, SLAB_PANIC, NULL);

	// NOTE: vmalloc is page-aligned
	virtual_mem_buf = vmalloc(VIRTUAL_BUF_SIZE);

	// set PG_reserved, so that these pages won't be swapped out
	for(page_id = 0; page_id < VIRTUAL_BUF_PAGE_NUM; page_id++){
		this_page = vmalloc_to_page(virtual_mem_buf + page_id * VM_PAGE_SIZE);
		SetPageReserved(this_page);
	}

	// init the sampling variables and the vmalloced memory
	sampling_count = 0;
	sampling_position_ptr = (unsigned long*) virtual_mem_buf;
	_report_terminate();
}

// free all the used slabs
void _destroy_mp3_memory(void){
	struct page *this_page;
	int page_id;

	kmem_cache_destroy(mp3_entry_slab);
	kmem_cache_destroy(mp3_write_buf_slab);

	// clear PG_reserved to swap out the page
	for(page_id = 0; page_id < VIRTUAL_BUF_PAGE_NUM; page_id++){
		this_page = vmalloc_to_page(virtual_mem_buf + page_id * VM_PAGE_SIZE);
		ClearPageReserved(this_page);
	}

	vfree(virtual_mem_buf);
}


/*

	Character Devices

*/
// nothing for open and release
static int device_open(struct inode *inode, struct file *filp){
	return 0;
}
static int device_release(struct inode *inode, struct file *filp){
  	return 0;
}

// tricks for mmap
static int device_mmap(struct file *inode, struct vm_area_struct *vma){
	int page_id;
	unsigned long this_pfn;

	#ifdef DEBUG
	printk(KERN_ALERT "device_mmap called\n");
	#endif

	for(page_id = 0; page_id < VIRTUAL_BUF_PAGE_NUM; page_id++){
		// if not enought virtual memory left
		if(vma->vm_start + page_id * VM_PAGE_SIZE + VM_PAGE_SIZE >= vma->vm_end){
			break;
		}

		this_pfn = vmalloc_to_pfn(virtual_mem_buf + page_id * VM_PAGE_SIZE);
		remap_pfn_range(vma, vma->vm_start + page_id * VM_PAGE_SIZE,
			this_pfn, VM_PAGE_SIZE, vma->vm_page_prot);
	}

	return 0;
}

// fs struct
static const struct file_operations char_dev_ops = {
	.owner = THIS_MODULE,
	.mmap = device_mmap,
	.open = device_open,
	.release = device_release
};


// init the character device
void _init_char_dev(void){
	#ifdef DEBUG
	printk(KERN_ALERT "_init_char_dev called\n");
	#endif

	// create the device
	mp3_major_num = register_chrdev(0, DEVICE_NAME, &char_dev_ops);
	printk(KERN_ALERT "device created with num [%d]\n", mp3_major_num);
}

// destroy device and major number
void _destroy_char_dev(void){
	#ifdef DEBUG
	printk(KERN_ALERT "_destroy_char_dev called\n");
	#endif

	// destroy the device
	unregister_chrdev(mp3_major_num, DEVICE_NAME);
}


/*

	Init and Exit

*/
// mp2_init - Called when module is loaded
int __init mp3_init(void){
	#ifdef DEBUG
	printk(KERN_ALERT "MP3 MODULE LOADING\n");
	#endif

	_init_mp3_memory();

	init_linked_list();

	_create_proc_mp3_status();

	_init_char_dev();

	_init_work_queue();

	// done loading
	printk(KERN_ALERT "MP3 MODULE LOADED\n");
	return 0;   
}

// mp2_exit - Called when module is unloaded
void __exit mp3_exit(void){
	#ifdef DEBUG
	printk(KERN_ALERT "MP3 MODULE UNLOADING\n");
	#endif

	_destroy_work_queue();

	_destroy_char_dev();

	_delete_proc_mp3_status();

	free_linked_list();

	_destroy_mp3_memory();

	// done unloading
	printk(KERN_ALERT "MP3 MODULE UNLOADED\n");
}

// Register init and exit funtions
module_init(mp3_init);
module_exit(mp3_exit);