#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"
#include "traps.h"
#include "debugsw.h"

#define SWAP_BUF_SIZE (PGSIZE / 4)    // Buffer size when swap.

struct swap_offset_desc
{
  // If is_high is true, the addr is in stack.
  // Otherwise it's in heap/data/text.
  int is_high;

  // Offset used in swapping.
  uint offset;
};

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm) < 0) {
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  if(p == 0)
    panic("switchuvm: no process");
  if(p->kstack == 0)
    panic("switchuvm: no kstack");
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                sizeof(mycpu()->ts)-1, 0);
  mycpu()->gdt[SEG_TSS].s = 0;
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort) 0xFFFF;
  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir));  // switch to process's address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, P2V(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

void fifo_record(char *va, struct proc *curproc)
{
  int i;
  for (i = 0; i < MAX_PHYS_PAGES; i++)
    if (curproc->mem_pages[i].va == SLOT_USABLE)
    {
      curproc->mem_pages[i].va = va;
      curproc->mem_pages[i].next = curproc->head;
      curproc->head = &curproc->mem_pages[i];
      return;
    }

  panic("[ERROR] No free slot in memory.");
}

// Add a new page to mem_pages.
void record_page(char *va)
{
  struct proc *curproc = myproc();
  fifo_record(va, curproc);
  curproc->num_mem_entries++;
}

struct mem_page *fifo_write()
{
  int i;
  struct mem_page *link, *last;
  struct proc *curproc = myproc();

  for (i = 0; i < MAX_PHYS_PAGES; i++)
    if (curproc->swap_pages[i].va == SLOT_USABLE)
    {
      // Find the last record in mem_pages,
      // then swap it out.
      link = curproc->head;
      if (link == 0 || link->next == 0)
        panic("Only 0 or 1 page in memory.");
      while (link->next->next != 0)
        link = link->next;
      last = link->next;
      link->next = 0;

      // Swap the page out, write it to swapfile.
      // The record in swapfile and swap_pages is in the same order.
      curproc->swap_pages[i].va = last->va;
      if (swapwrite(curproc, (char *)PTE_ADDR(last->va), i * PGSIZE, PGSIZE) == 0)
        return 0;

      // Free the page pointed by last - it has been swapped out and can be reused.
      pte_t *pte = walkpgdir(curproc->pgdir, (void *)last->va, 0);
      if (!(*pte))
        panic("[ERROR] [fifo_write] PTE empty.");
      kfree((char *)(P2V_WO(PTE_ADDR(*pte))));
      *pte = PTE_W | PTE_U | PTE_PG;

      curproc->num_swap_pages++;

      // Refresh page dir.
      lcr3(V2P(curproc->pgdir));

      // Return the freed slot.
      return last;
    }

  panic("[ERROR] No free slot in storage.");
}

// Swap out a page from mem_pages to swap_pages.
struct mem_page *write_page(char *va)
{
  return fifo_write();
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.

// We are using it to allocate memory from oldsz to newsz.
// Memory is not continuous now due to stack auto growth.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uint a;
  struct proc* curproc = myproc();
  int stack_reserved = USERTOP - curproc->stack_size - PGSIZE;

  uint newpage_allocated = 1;
  struct mem_page* l;

  // Check args.
  if (curproc->stack_grow == 1)
  {
    // An empty page is reserved between stack and heap.
    if (oldsz == stack_reserved && oldsz < curproc->stack_size + PGSIZE)
      return 0;

    if (stack_reserved - PGSIZE < curproc->sz)
      return 0;
  }
  else if (newsz > stack_reserved)
  {
    return 0;
  }

  if (newsz > KERNBASE)
    return 0;
  if (newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){

    // Check if we have enough space to put the page in memory.
    if (curproc->num_mem_entries >= MAX_PHYS_PAGES)
    {
      // Swap out page at oldsz.
      //! At least in fifo, the arg passed to write_page is unimportant.
      if ((l = write_page((char *)a)) == 0)
        panic("[ERROR] Cannot write to swapfile.");

      //! These code are for FIFO only.
      l->va = (char *)a;
      l->next = curproc->head;
      curproc->head = l;

      // No new page in memory will be used
      // (A page will be reused), mark that.
      newpage_allocated = 0;
    }

    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }

    if (newpage_allocated)
      record_page((char *)a);

    memset(mem, 0, PGSIZE);
    if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t *pte;
  uint a, pa;
  int i;
  struct proc* curproc = myproc();

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte)
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
    else if((*pte & PTE_P) != 0){
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");

      // If the page is in mem_pages, clear it.
      if (curproc->pgdir == pgdir)
      {
        for (i = 0; i < MAX_PHYS_PAGES; i++)
          if (curproc->mem_pages[i].va == (char *)a)
            goto SLOT_FOUND;
        panic("[ERROR] deallocuvm (entry not found (memory)).");
      SLOT_FOUND:
        curproc->mem_pages[i].va = SLOT_USABLE;

        if (curproc->head == &curproc->mem_pages[i])
          curproc->head = curproc->mem_pages[i].next;
        else
        {
          struct mem_page *l = curproc->head;
          while (l->next != &curproc->mem_pages[i])
            l = l->next;
          l->next = curproc->mem_pages[i].next;
        }
        curproc->mem_pages[i].next = 0;
        curproc->num_mem_entries--;
      }

      char *v = P2V(pa);
      kfree(v);
      *pte = 0;
    }
    // Maybe the page is not presented by is in swapfile.
    else if((*pte&PTE_PG)&&curproc->pgdir == pgdir)
    {
      for(i = 0;i<MAX_PHYS_PAGES;i++)
      if(curproc->swap_pages[i].va == (char*) a)
      {
        curproc->swap_pages[i].va = SLOT_USABLE;

        curproc->num_swap_pages--;
        return newsz;
      }
      panic("[ERROR] deallocuvm (entry not found (swap)).");
    }
  }
  return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.

// This function has been modified.
// (Copy on write and stack auto growth.)
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;

  if((d = setupkvm()) == 0)
    return 0;

  // Copy code section, data section and heap section.
  for (i = PGSIZE; i < sz; i += PGSIZE)
  {
    if ((pte = walkpgdir(pgdir, (void *)i, 0)) == 0)
      panic("copyuvm: pte should exist");

    // Don't know why need to ignore the test.
    // Run "cat README | grep run" to see difference.
    if(!(*pte & PTE_P) && !(*pte & PTE_PG))
      continue;
    if (*pte & PTE_PG)
    {
      pte = walkpgdir(d, (void *)i, 1);
      *pte = PTE_U | PTE_W | PTE_PG;
      continue;
    }

    *pte &= ~PTE_W;
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if(mappages(d,(void*)i, PGSIZE, pa, flags) < 0)
      goto bad;
    incr_page_ref(pa);
  }

  // Copy stack section.
  // For simplicity we keep the stack shared.
  for(i = USERTOP - myproc()->stack_size;i<USERTOP;i+=PGSIZE)
  {
    if ((pte = walkpgdir(pgdir, (void *)i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if (!(*pte & PTE_P) && !(*pte & PTE_PG))
      continue;
    if (*pte & PTE_PG)
    {
      pte = walkpgdir(d, (void *)i, 1);
      *pte = PTE_U | PTE_W | PTE_PG;
      continue;
    }

    *pte &= ~PTE_W;
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if (mappages(d, (void *)i, PGSIZE, pa, flags) < 0)
      goto bad;
    incr_page_ref(pa);
  }

  lcr3(V2P(pgdir));
  return d;

bad:
  freevm(d);
  lcr3(V2P(pgdir));
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.


//todo Refactor this messy code.
void pagefault(uint err_code)
{
  uint va = rcr2();
  struct proc* curproc = myproc();

  if(SHOW_PAGEFAULT_INFO)
    cprintf("pagefault at virt addr 0x%x, error code is %d, process name %s.\n", va, err_code, curproc->name);

  // If the page fault is caused by a non-present page,
  // should be due to lazy allocation or null pointer protection,
  // or stack needing growth, or due to be swapped out.
  // Otherwise, it should be due to protection violation (copy on write).
  // If the page fault is caused by kernel, it should be handled too.
  if (!(err_code & PGFLT_P))
  {
    // Used by swapping.
    pte_t* pte = &curproc->pgdir[PDX(va)];
    if(((*pte) & PTE_P) != 0)
    {
      // If the page is swapped out, swap it in.
      if(((uint*)PTE_ADDR(P2V(*pte)))[PTX(va)] & PTE_PG) 
      {
        swappage(PTE_ADDR(va));
        return;
      }
    }

    // If va is less than PGSIZE, this is a null pointer.
    if (va < PGSIZE)
    {
      cprintf("[ERROR] Dereferencing a null pointer (0x%x), \"%s\" will be killed.\n", va, curproc->name);
      curproc->killed = 1;
      return;
    }

    // If va is higher than sz and lower than stack top, should be stack growth.
    //? Is this always corrent?
    if (va >= curproc->sz + PGSIZE && va < USERTOP - curproc->stack_size)
    {
      if (SHOW_STACK_GROWTH_INFO)
        cprintf("[INFO ] Stack of \"%s\" is now growing.\n", curproc->name);
      curproc->stack_grow = 1;

      // An empty page is reserved between stack and heap.
      if (allocuvm(curproc->pgdir, USERTOP - curproc->stack_size - PGSIZE, USERTOP - curproc->stack_size) == 0)
      {
        cprintf("[ERROR] Stack growth failed, \"%s\" will be killed.\n", curproc->name);
        curproc->killed = 1;
      }
      curproc->stack_grow = 0;
      curproc->stack_size += PGSIZE;
      return;
    }

    // Otherwise, should be heap allocation.
    if (SHOW_LAZY_ALLOCATION_INFO)
      cprintf("Lazy allocation at virt addr 0x%x.\n", va);

    char *mem = kalloc();
    if (mem == 0)
    {
      cprintf("Lazy allocation failed: Memory out. Killing process.\n");
      curproc->killed = 1;
      return;
    }

    // va needs to be rounded down, or two pages will be mapped in mappages().
    va = PGROUNDDOWN(va);
    memset(mem, 0, PGSIZE);

    // The first process use this page can have write permissions,
    // but once forked, copyuvm will set it permission to readonly.
    if (mappages(curproc->pgdir, (char *)va, PGSIZE, V2P(mem), PTE_W | PTE_U) < 0)
    {
      cprintf("Lazy allocation failed: Memory out (2). Killing process.\n");
      curproc->killed = 1;
      return;
    };
  
    return;
  }

  pte_t *pte;

  if (curproc == 0)
  {
    panic("Pagefault. No process.");
  }

  if ((va >= KERNBASE) || (pte = walkpgdir(curproc->pgdir, (void *)va, 0)) == 0 || !(*pte & PTE_P) || !(*pte & PTE_U))
  {
    if (SHOW_PAGEFAULT_IA_ERR)
      cprintf("Pagefault. Illegal address.\n");
    curproc->killed = 1;
    return;
  }

  if (*pte & PTE_W)
  {
    panic("Pagefault. Already writeable.");
  }

  uint pa = PTE_ADDR(*pte);
  ushort ref = get_page_ref(pa);
  char *mem;

  if (ref == 1)
    *pte |= PTE_W;
  else if (ref > 1)
  {
    if ((mem = kalloc()) == 0)
    {
      cprintf("Pagefault. Out of memory.");
      curproc->killed = 1;
      return;
    }

    memmove(mem, P2V(pa), PGSIZE);
    *pte = V2P(mem) | PTE_P | PTE_U | PTE_W;
    decr_page_ref(pa);
  }
  else
    panic("Pagefault. Reference count error.");
}

void fifo_swap(uint addr)
{
  int i, j;
  char buf[SWAP_BUF_SIZE];
  pte_t *pte_in, *pte_out;
  struct proc *curproc = myproc();

  // Find the last record in mem_pages.
  struct mem_page *link = curproc->head;
  struct mem_page *last;
  if (link == 0 || link->next == 0)
    panic("[ERROR] Only 0 or 1 pages in memory.");
  while (link->next->next != 0)
    link = link->next;
  last = link->next;
  link->next = 0;

  // Locate the PTE of the page to be swapped out.
  pte_in = walkpgdir(curproc->pgdir, (void *)last->va, 0);
  if (!*pte_in)
    panic("[ERROR] A record is in mem_pages but not in pgdir.");

  // Find the record of the page to be swapped in in swap_pages.
  for (i = 0; i < MAX_PHYS_PAGES; i++)
    if (curproc->swap_pages[i].va == (char *)PTE_ADDR(addr))
      goto SLOT_FOUND;
  panic("[ERROR] Should found a record in swapfile!");

  // Perform swap.
SLOT_FOUND:
  curproc->swap_pages[i].va = last->va;
  pte_out = walkpgdir(curproc->pgdir, (void *)addr, 0);
  if (!*pte_out)
    panic("[ERROR] A record should be in pgdir!");
  *pte_out = PTE_ADDR(*pte_in) | PTE_U | PTE_W | PTE_P;

  // Real swap - read from swapfile and write to swap file.
  for (j = 0; j < 4; j++)
  {
    int loc = (i * PGSIZE) + (SWAP_BUF_SIZE * j);
    int offset = SWAP_BUF_SIZE * j;
    memset(buf, 0, SWAP_BUF_SIZE);
    swapread(curproc, buf, loc, SWAP_BUF_SIZE);
    swapwrite(curproc, (char *)(P2V_WO(PTE_ADDR(*pte_in)) + offset), loc, SWAP_BUF_SIZE);
    memmove((void *)(PTE_ADDR(addr) + offset), (void *)buf, SWAP_BUF_SIZE);
  }

  *pte_in = PTE_U | PTE_W | PTE_PG;
  last->next = curproc->head;
  curproc->head = last;
  last->va = (char *)PTE_ADDR(addr);
}

// Return whether the address is in high part of the memory space.
// True if the address is in stack.
// False if it's in heap/text/data.
int is_high_memory(struct proc *p, uint vaddr)
{
  vaddr = PGROUNDDOWN(vaddr);
  if ((vaddr >= p->sz && vaddr < USERTOP - p->stack_size) || vaddr >= USERTOP)
    panic("[ERROR] High memory check: invalid virt addr.");
  if (vaddr < p->sz)
    return 0;
  if (vaddr >= USERTOP - p->stack_size)
    return 1;
  panic("[ERROR] High memory check: invalid virt addr. (2)");
}

// Get the offset descriptor to be used in memory swapping.
// `vaddr` will be aligned inside the function.
static struct swap_offset_desc
get_swap_offset(struct proc *p, uint vaddr)
{
  vaddr = PGROUNDDOWN(vaddr);
  int is_high = is_high_memory(p, vaddr);
  struct swap_offset_desc ret;
  if (is_high)
  {
    ret.is_high = 1;
    ret.offset = USERTOP - PGSIZE - vaddr;
  }
  else
  {
    ret.is_high = 0;
    ret.offset = vaddr;
  }

  return ret;
}

void swappage(uint addr)
{
  struct proc*curproc = myproc();

  //? Why should we do this?
  if (kstrcmp(curproc->name, "init") == 0 || kstrcmp(curproc->name, "sh") == 0)
  {
    curproc->num_mem_entries++;
    return;
  }

  fifo_swap(addr);

  // Refresh page dir.
  lcr3(V2P(curproc->pgdir));

  //! remove
  get_swap_offset(0, 0);
}