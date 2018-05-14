// Per-CPU state
struct cpu {
  uchar apicid;                // Local APIC ID
  struct context *scheduler;   // swtch() here to enter scheduler
  struct taskstate ts;         // Used by x86 to find stack for interrupt
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  volatile uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  struct proc *proc;           // The process running on this cpu or null
};

extern struct cpu cpus[NCPU];
extern int ncpu;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
  uint edi;
  uint esi;
  uint ebx;
  uint ebp;
  uint eip;
};

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Every memory swap table page has 170 entries to fit into a page.
#define NUM_MEMSTAB_PAGE_ENTRIES 170
// Every swapped swap table page has 510 entries to fit into a page.
// Every page in swapstab can contain ~1M infomation about swapped out memory.
#define NUM_SWAPSTAB_PAGE_ENTRIES 510

// A swap table has 25 table pages, so the number of in-memory pages is limited to 4250,
// which is 16.6MB. All swap tables take 6.25MB.
// Number of swapped stab pages is unlimited, it will grow dynamically and is limited by USERTOP.
#define NUM_MEMSTAB_PAGES 25
struct memstab_page_entry
{
  char *vaddr;
  int age;
  struct memstab_page_entry *next;
};

// This is the entry of a page of the swapped swap table. It only contains a virtual address,
// So the pages in memory and swapfile should be in the same order.
struct swapstab_page_entry
{
  char *vaddr;
};

// This is part of a table to record pages in memory (stab means 'swap table').
// 2 pointer takes 16 bytes, and the array takes 170x24 bytes (4080 in total),
// so it can be filled into a single page perfectly.

// By linking many of these pages, we can have a large swap table in kernel memory.
// Cannot use a static table as the size of the kernel is limited to 4MB.
struct memstab_page
{
  struct memstab_page *prev;
  struct memstab_page *next;
  struct memstab_page_entry entries[NUM_MEMSTAB_PAGE_ENTRIES];
};

struct swapstab_page
{
  struct swapstab_page *prev;
  struct swapstab_page *next;
  struct swapstab_page_entry entries[NUM_SWAPSTAB_PAGE_ENTRIES];
};

// Per-process state
struct proc {
  uint sz;                     // Size of process memory (bytes)
  pde_t* pgdir;                // Page table
  char *kstack;                // Bottom of kernel stack for this process
  enum procstate state;        // Process state
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  struct trapframe *tf;        // Trap frame for current syscall
  struct context *context;     // swtch() here to run process
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)

  // Now the stack is growing from top to bottom,
  // and the heap is growing from bottom to top (Both expandable).

  uint stack_size;             // Process stack size.
  int stack_grow;              // Is the stack growing.

  int num_mem_pages;

  struct file *swapfile_high; // Swap file for high memory - stack.
  struct file *swapfile_low;  // Swap file for low memory - heap, data and text.

  struct memstab_page *memstab_head;
  struct memstab_page *memstab_tail;

  struct swapstab_page *swapstab_high_head;
  struct swapstab_page *swapstab_high_tail;
  struct swapstab_page *swapstab_low_head;
  struct swapstab_page *swapstab_low_tail;
};
