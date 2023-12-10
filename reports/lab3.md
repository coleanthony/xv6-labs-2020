## Lab3 : Pgtbl

### Print a page table

```c
void vmprint_(pagetable_t pagetable,int level){
  //PTE_V是用来判断页表项是否有效
  char *prev[]={"..",".. ..",".. .. .."};
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if(pte & PTE_V){
      printf("%s",prev[level-1]);
      uint64 child = PTE2PA(pte);
      printf("%d: pte %p pa %p\n",i,pte,child);
      if (level<3){
        vmprint_((pagetable_t)child,level+1);
      }
    }
  }
}

void vmprint(pagetable_t pagetable){
  printf("page table %p\n",pagetable);
  vmprint_(pagetable,1);
}
```

---

### A kernel page table per process

1. 在struct proc中为进程的内核页表增加一个字段

```c
struct proc {
  struct spinlock lock;

  // p->lock must be held when using these:
  enum procstate state;        // Process state
  struct proc *parent;         // Parent process
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  int xstate;                  // Exit status to be returned to parent's wait
  int pid;                     // Process ID

  // these are private to the process, so p->lock need not be held.
  uint64 kstack;               // Virtual address of kernel stack
  uint64 sz;                   // Size of process memory (bytes)
  pagetable_t pagetable;       // User page table
  pagetable_t pagetable_kn;    // Kernel page table
  struct trapframe *trapframe; // data page for trampoline.S
  struct context context;      // swtch() here to run process
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
};
```

2. 为一个新进程生成一个内核页表的合理方案是实现一个修改版的kvminit，这个版本中应当创造一个新的页表而不是修改kernel_pagetable。你将会考虑在allocproc中调用这个函数。

```c
pagetable_t kn_kvminit(){
  pagetable_t pagetable_kn=uvmcreate();
  if (pagetable_kn==0)
    return 0;

  // uart registers
  kn_kvmmap(pagetable_kn, UART0, UART0, PGSIZE, PTE_R | PTE_W);
  // virtio mmio disk interface
  kn_kvmmap(pagetable_kn,VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);
  // CLINT
  kn_kvmmap(pagetable_kn,CLINT, CLINT, 0x10000, PTE_R | PTE_W);
  // PLIC
  kn_kvmmap(pagetable_kn,PLIC, PLIC, 0x400000, PTE_R | PTE_W);
  // map kernel text executable and read-only.
  kn_kvmmap(pagetable_kn,KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);
  // map kernel data and the physical RAM we'll make use of.
  kn_kvmmap(pagetable_kn,(uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);
  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kn_kvmmap(pagetable_kn,TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  return pagetable_kn;
}
```

3. 确保每一个进程的内核页表都关于该进程的内核栈有一个映射。在未修改的XV6中，所有的内核栈都在procinit中设置。你将要把这个功能部分或全部的迁移到allocproc中。

```c
  p->pagetable_kn=kn_kvminit();
  if(p->pagetable_kn==0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  char *pa = kalloc();
  if(pa == 0)
    panic("kalloc");
  uint64 va = KSTACK((int) (p - proc));
  kn_kvmmap(p->pagetable_kn,va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  p->kstack = va;
```

4. 修改scheduler()来加载进程的内核页表到核心的satp寄存器(参阅kvminithart来获取启发)。不要忘记在调用完w_satp()后调用sfence_vma()。没有进程运行时scheduler()应当使用kernel_pagetable。

```c
        kn_kvminithart(p->pagetable_kn);

        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        kvminithart();
```

5. 在freeproc中释放一个进程的内核页表，需要一种方法来释放页表，而不必释放叶子物理内存页面。

```c
void
proc_freepagetable_kn(pagetable_t pagetable_kn)
{
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable_kn[i];
    if(pte & PTE_V){
      pagetable_kn[i]=0;
      if ((pte & (PTE_R|PTE_W|PTE_X)) == 0){
        // this PTE points to a lower-level page table.
        uint64 child = PTE2PA(pte);
        proc_freepagetable_kn((pagetable_t)child);
      }
    }
  }
  kfree((void*)pagetable_kn);
}
```

---

### Simplify

1. 先用对copyin_new的调用替换copyin()，copyinstr同样。就是把原本的内容删了，调用copyin_new和copyinstr_new。

2. 在内核更改进程的用户映射的每一处，都以相同的方式更改进程的内核页表。包括fork(), exec(), 和sbrk().
首先需要新增一个将进程的用户态页表复制一份到进程的内核态页表。

```c
int usermaptokvmcopy(pagetable_t user,pagetable_t kernel,uint64 old_sz,uint64 new_sz){
  pte_t *pte;
  uint64 pa,i;
  old_sz=PGROUNDUP(old_sz);
  for (i = old_sz; i < new_sz; i+=PGSIZE){
    if((pte = walk(user, i, 0)) == 0)
      panic("usermaptokvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("usermaptokvmcopy: page not present");
    pa = PTE2PA(*pte);
    uint flags=PTE_FLAGS(*pte)&(~PTE_U);
    if(mappages_kn(kernel, i, PGSIZE, pa, flags) != 0){
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(kernel, 0, i / PGSIZE, 1);
  return -1;
}
```

exec、fork都是添加

```c
usermaptokvmcopy(pagetable, p->pagetable_kn, 0,sz);
```

sbrk

```c
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if (PGROUNDUP(sz+n)>=PLIC){
      return -1;
    }
    
    if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      return -1;
    }
     usermaptokvmcopy(p->pagetable, p->pagetable_kn, sz-n,sz);
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}
```

3. 在userinit的内核页表中包含第一个进程的用户页表
