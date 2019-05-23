### Implementation
1) Module initialization and exit
    Initialize all the resources needed.
    Free them in reverse order.
2) Memory buffer allocation, mapping & the character device
    Initialize 512KB physical memory buffer through vmalloc, set the PG_reserved bit for all 128 virtual pages.
    Register a character deivce, map the buffer to user's virtual memory space through the device's mmap function.
    Unset the PG_reserved bit and deallocate the physical memory buffer when the module exits.
    Deregister the character device when the module exits.
3) Proc FS read & write
    Read(): return a string of all the currently registered processes.
    Write(): process the input commands: register and unregister.
4) The basic linked list functionality
    Init() and Exit(): initialize the spin lock on stack and the linked list. slab-free the whole linked list when module exits.
    Register(): slab-allocate and initialize an augmented PCB for the registered process and add it to the linked list.
    Deregister(): remove the given entry from the linked list. slab-free the augmented PCB.
    Read_all(): traverse the linked list, and return a string representation of all the currently registered processes.
5) The delayed work queue & buffer writing
    Update_virtual_mem_buf(): invoked every 50ms during the current measurement period. Write a row of record into the buffer as required.
    Report_terimnate(): write a row of -1 into the buffer when a measurement period ends.
    When the first process is added to the linked list, start a measurement period.
    When the final process is removed from the linked list, end the current measurement period.

### Design Decisions
1) Augmented PCB
    I use augmented PCB as linked list entry, as specified in the documentation. However, since I have the get_cpu_use() in mp3_give.h, I only need to know the pids for all the registered processes. Therefore, effectively, I am maintaining a list of pids, without using any other fields in the augmented PCB.
2) Delayed work queue management
    I maintain two global variables: the linked list length and the status of the work queue (RUNNING, STOPPED). When length=0 and a process is added, set the status of the work queue to RUNNING. After a process is removed and length=0, set the status of the work queue to STOPPED. I keep queuing new job to the delayed work queue if the status remains RUNNING. The two variables are modified only when the spin lock is locked. Therefore, correctness and synchronization is guaranteed.
3) Memory buffer management
    I keep a global pointer to the current position in the buffer, and update it whenever I write a record into the buffer. During the writing process, I lock the spin lock and also check the validity of each pid in the linked list. When detect an invalid pid, I remove it from the linked list.
    I do not memset the whole buffer to zero. Instead, when a measurement period ends, I write a row of -1s into the buffer to terminate the monitor process in a proper way.
4) Output unit and format
    For CPU utilization, I output the sum of utime and stime during the interval, without dividing it by the time interval length to output a rate. I do the division later when drawing the graph and analyzing the data.
5) The name of the character device is:
    mp3_device
6) The format of command is:
    CMD_FORMAT_REGIST   "R %d"
    CMD_FORMAT_UNREGIST "U %d"


### Testing
Following exactly what is told in the documentation:
1) Install the module
`sudo insmod ziangw2_MP3.ko`
2) Find the major number of the newly registered character device named "mp3_device"
`cat /proc/devices`
3) Create a file to access the character device
`mknod node c [major # of the device] 0`
4) Run working processes. For example
`nice ./work 1024 R 50000 & nice ./work 1024 R 10000 &`
5) Gather the data
`sudo ./monitor > output.txt`
6) When done, simply uninstall the module
`sudo rmmod ziangw2_MP3.ko`
