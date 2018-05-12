#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "defs.h"
#include "x86.h"
#include "elf.h"

int
exec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint argc, sz, sp, ustack[3+MAXARG+1];
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pde_t *pgdir, *oldpgdir;
  struct proc *curproc = myproc();

  begin_op();

  if((ip = namei(path)) == 0){
    end_op();
    cprintf("exec: fail\n");
    return -1;
  }
  ilock(ip);
  pgdir = 0;

  // Check ELF header
  if(readi(ip, (char*)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pgdir = setupkvm()) == 0)
    goto bad;

  // Save data for swapping, restore them later.
  // Normalily we can just clear them, but if exec failed,
  // we need to be able to restore.
  int num_mem_pages = curproc->num_mem_pages;
  int num_swap_pages = curproc->num_swap_pages;
  struct mem_page mem_pages[MAX_PHYS_PAGES];
  struct swap_page swap_pages[MAX_PHYS_PAGES];

  for (i = 0; i < MAX_PHYS_PAGES; i++)
  {
    mem_pages[i].va = curproc->mem_pages[i].va;
    mem_pages[i].age = curproc->mem_pages[i].age;
    mem_pages[i].next = curproc->mem_pages[i].next;

    swap_pages[i].va = curproc->swap_pages[i].va;
    swap_pages[i].age = curproc->swap_pages[i].age;
    swap_pages[i].swaploc = curproc->swap_pages[i].swaploc;

    curproc->mem_pages[i].va = SLOT_USABLE;
    curproc->mem_pages[i].age = 0;
    curproc->mem_pages[i].next = 0;
    curproc->swap_pages[i].va = SLOT_USABLE;
    curproc->swap_pages[i].age = 0;
    curproc->swap_pages[i].swaploc = 0;
  }

  struct mem_page* head = curproc->head;
  curproc->num_mem_pages = 0;
  curproc->num_swap_pages = 0;
  curproc->head = 0;

  // Load program into memory.
  sz = PGSIZE;
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, (char*)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if((sz = allocuvm(pgdir, sz, ph.vaddr + ph.memsz)) == 0)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    if(loaduvm(pgdir, (char*)ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  // Set sz to heap bottom.
  sz = PGROUNDUP(sz);

  // Setup the top page for stack.
  curproc->stack_grow = 1;
  if (allocuvm(pgdir, USERTOP - PGSIZE, USERTOP) == 0)
    goto bad;
  curproc->stack_grow = 0;

  sp = USERTOP;

  // Push argument strings, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp = (sp - (strlen(argv[argc]) + 1)) & ~3;
    if(copyout(pgdir, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[3+argc] = sp;
  }
  ustack[3+argc] = 0;

  ustack[0] = 0xffffffff;  // fake return PC
  ustack[1] = argc;
  ustack[2] = sp - (argc+1)*4;  // argv pointer

  sp -= (3+argc+1) * 4;
  if(copyout(pgdir, sp, ustack, (3+argc+1)*4) < 0)
    goto bad;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(curproc->name, last, sizeof(curproc->name));

  // Commit to the user image.
  oldpgdir = curproc->pgdir;
  curproc->pgdir = pgdir;
  curproc->stack_size = PGSIZE;
  curproc->sz = sz;
  curproc->tf->eip = elf.entry;  // main
  curproc->tf->esp = sp;

  // Refresh swapfile.
  swapdealloc(curproc);
  swapalloc(curproc);

  switchuvm(curproc);
  freevm(oldpgdir);
  return 0;

 bad:
  if(pgdir)
    freevm(pgdir);
  if(ip){
    iunlockput(ip);
    end_op();
  }

  // Restore data for swapping
  curproc->num_mem_pages = num_mem_pages;
  curproc->num_swap_pages = num_swap_pages;
  curproc->head = head;

  for (i = 0; i < MAX_PHYS_PAGES; i++)
  {
    curproc->mem_pages[i].va = mem_pages[i].va;
    curproc->mem_pages[i].age = mem_pages[i].age;
    curproc->mem_pages[i].next = mem_pages[i].next;

    curproc->swap_pages[i].va = swap_pages[i].va;
    curproc->swap_pages[i].age = swap_pages[i].age;
    curproc->swap_pages[i].swaploc = swap_pages[i].swaploc;
  }

  return -1;
}
