### Implementation

My implementation consists of four parts:
1) Module initialization and exit
    Initialize and deallocate the linked list, timers, spin lock. Start and stop the dispatch thread.
2) Proc FS read & write
    Read() returns a string of all the currently registered processes. Write() processes the input commands: register, deregister and yield.
3) Dispatch thread
    A thread that, if wakes up, finds the next ready process with the shortest period to run, and it also takes care of context switch.
4) PCB augmentation and the linked list
    A linked list with each entry representing the augmented PCB of each registered process. Implemented functionalities are as follows:
    register() with admission control: allocate an entry for the process, add it to the linked list if permitted, 
        initialize the timer, and update the current load of running processes
    yield(): sleep a process, set a timer for it, and wake up the dispatch thread
    deregister(): delete a process, free the memory, stop the timer, and wake up the dispatch thread
    find(): traverse the linked list, and find the ready process with the shortest period, if any
    read(): traverse the linked list, and return a string representation of all the currently registered processes
    init(): initialize the spin lock and the list head
    free(): free the whole linked list, and stop all the timers

### Design Decisions

1) I keep a global variable, current_load, to implement admission control. Every register and deregister updates that value.
2) To avoid floating point arithmetic, I compute the work load of each process by:
    1 + 1000 * ProcessTimePerPeriod / Period (I use the term Cost instead of ProcessTimePerPeriod.)
    Thus, admission control means to keep the current_load global variable under 693.
3) I keep a global variable, running_process_pt, to keep track of the currently running process.
    It points to the augmented PCB of that process.
4) When deregistering a process, I compare it with running_process_pt to see whether it is currently running. If so, I wake up
    the dispatch thread after deregistration.
5) I do not perform context switch if there is a tie between the two period times.
6) When doing context switch, I explicitly set the old task to sleeping by doing
    set_task_state(pcb_ptr(old_task), TASK_UNINTERRUPTIBLE);
7) I use millisecond as the time unit for input. Internally, I store the time unit in jiffies.

### Testing

I write a userapp that immitate a repeating real time job for ITERATION (a macro in userapp.c, default 6) iterations. Sample usage:

`.\userapp 300 1000`

It immitates a job that runs 300 milliseconds every 1000 milliseconds for 6000 milliseconds. The actual processing time per period
is not guaranteed if the input value is too large. I recommend a value between 20 and 1500.

Some interesting test cases and their shell commands are as follows:
1) Single source of repeating tasks:
`./userapp 600 1000`

2) Two sources of repeating tasks without preemption:
`./userapp 1000 3000 & ./userapp 1000 3050 &`

3) Two sources of repeating tasks with preemption:
`./userapp 1000 3000 & ./userapp 500 1550 &` 