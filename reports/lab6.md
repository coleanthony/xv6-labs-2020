## Lab6: Copy-on-Write Fork for xv6

### Implement copy-on write

（1）在kernel/riscv.h中选取PTE中的保留位定义标记一个页面是否为COW Fork页面的标志位

```c
#define PTE_F (1L << 8)
```

（2）在kalloc.c中进行如下修改
- 定义引用计数的全局变量ref，其中包含了一个自旋锁和一个引用计数数组，由于ref是全局变量，会被自动初始化为全0。

```c
struct ref_stru{
  struct spinlock lock;
  int cnt[PHYSTOP/PGSIZE];
} ref;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&ref.lock,"ref");
  freerange(end, (void*)PHYSTOP);
}
```

（3）修改freerange、kalloc、kfree

```c
void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    ref.cnt[(uint64)p/PGSIZE]=1;
    kfree(p);
  }
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&ref.lock);
  ref.cnt[(uint64)pa/PGSIZE]--;
  if (ref.cnt[(uint64)pa/PGSIZE]==0){
    release(&ref.lock);

    memset(pa, 1, PGSIZE);

    r = (struct run*)pa;
    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
  }else{
    release(&ref.lock);
  }
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r){
    kmem.freelist = r->next;
    acquire(&ref.lock);
    ref.cnt[(uint64)r/PGSIZE]=1;

    release(&ref.lock);
  }
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
```

（4）新增几个函数

```c
//判断一个页面是否为COW页面
int judgecowpage(pagetable_t pagetable,uint64 va){
  if (va>=MAXVA)
  {
    return -1;
  }
  pte_t* pte=walk(pagetable,va,0);
  if (pte==0)
  {
    return -1;
  }
  if ((*pte&PTE_V)==0)
  {
    return -1;
  }
  return (*pte &PTE_F?0:-1);
}

//kaddrefcnt 增加内存的引用计数
int kaddrefcnt(void *pa){
  if (((uint64)pa%PGSIZE)!=0||(char*)pa<end||(uint64)pa>=PHYSTOP)
  {
    return -1;
  }
  acquire(&ref.lock);
  ref.cnt[(uint64)pa/PGSIZE]++;
  release(&ref.lock);
  return 0;
}

//krefcnt 获取内存的引用计数
int krefcnt(void* pa) {
  return ref.cnt[(uint64)pa / PGSIZE];
}

//cowalloc copy-on-write分配器
void* cowalloc(pagetable_t pagetable,uint64 va){
  if (va%PGSIZE!=0)
  {
    return 0;
  }
  uint64 pa=walkaddr(pagetable,va);
  if (pa==0)
  {
    return 0;
  }

  pte_t* pte=walk(pagetable,va,0);

  if (krefcnt((char*)pa)==1){
    *pte |= PTE_W;
    *pte &= ~PTE_F;
    return (void*)pa;
  }else{
    //存在多个进程对物理内存存在引用
    char* mem=kalloc();
    if (mem==0)
    {
      return 0;
    }
    memmove(mem,(char*)pa,PGSIZE);

    *pte&=~PTE_V;

    if(mappages(pagetable, va, PGSIZE, (uint64)mem, (PTE_FLAGS(*pte) | PTE_W) & ~PTE_F) != 0) {
      kfree(mem);
      *pte |= PTE_V;
      return 0;
    }

    kfree((char*)PGROUNDDOWN(pa));
    return mem;
  }
}
```

（5）修改uvmcopy

```c
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);

    if (flags&PTE_W){
      // 禁用写并设置COW Fork标记
      flags=(flags|PTE_F)&~PTE_W;
      *pte=PA2PTE(pa)|flags;
    }
    

    if(mappages(new, i, PGSIZE,pa, flags) != 0){
      uvmunmap(new, 0, i / PGSIZE, 1);
      return -1;
    }

    //增加内存引用计数
    kaddrefcnt((char*)pa);
    
  }
  return 0;
}
```

（6）修改usertrap

```c
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8){
    // system call

    if(p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    intr_on();

    syscall();
  } else if((which_dev = devintr()) != 0){
    // ok
  }else if(r_scause()==13||r_scause()==15){
    uint64 faultva=r_stval();
    if (faultva>=p->sz||judgecowpage(p->pagetable,faultva)!=0||cowalloc(p->pagetable,PGROUNDDOWN(faultva))==0)
    {
      p->killed=1;
    }
  }else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

  usertrapret();
}
```

（7）修改copyout

```c
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);

    if (judgecowpage(pagetable,va0)==0)
    {
      pa0=(uint64)cowalloc(pagetable,va0);
    }
    

    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}
```
