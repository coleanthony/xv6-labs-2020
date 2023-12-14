## Lab5 : Lazy

### Eliminate allocation from sbrk()

```c
uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  /*
  if(growproc(n) < 0)
    return -1;
  */
  myproc()->sz+=n;
  return addr;
}
```

---

### Lazy allocation

修改usertrap。在usertrap()中查看r_scause()的返回值是否为13或15来判断该错误是否为页面错误，并通过r_stval()读取stval寄存器中保存了造成页面错误的虚拟地址。参考vm.c中的uvmalloc()中的代码。

```c
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
  } else if(r_scause()==13||r_scause()==15){
    uint64 stval=r_stval();
    uint64 mem=(uint64)kalloc();
    if(mem == 0){
      p->killed=1;
      printf("usertrap:alloc page error\n");
    }else{
      memset((void*)mem, 0, PGSIZE);
      stval=PGROUNDDOWN(stval);
      if(mappages(p->pagetable, stval, PGSIZE, mem, PTE_W|PTE_U|PTE_R) != 0){
        kfree((void*)mem);
        printf("usertrap:mappages error");
        p->killed=1;
      }
    }
  }else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }
```

修改uvmunmap()(kernel/vm.c)，之所以修改这部分代码是因为lazy allocation中首先并未实际分配内存，所以当解除映射关系的时候对于这部分内存要略过，而不是使系统崩溃

```c
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0){
      //panic("uvmunmap: not mapped");
      continue;
    }
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}
```

---

### Lazytests and Usertests

修改usertrap

```c
else if(r_scause()==13||r_scause()==15){
    uint64 stval=r_stval();
    uint64 mem=(uint64)kalloc();

    if(mem != 0&&PGROUNDUP(p->trapframe->sp)-1<stval&&stval<p->sz){
      memset((void*)mem, 0, PGSIZE);
      stval=PGROUNDDOWN(stval);
      if(mappages(p->pagetable, stval, PGSIZE, mem, PTE_W|PTE_U|PTE_R) != 0){
        kfree((void*)mem);
        printf("usertrap:mappages error");
        p->killed=1;
      }
    }else{
      p->killed=1;
    }
  }
```

处理sbrk()参数为负的情况。如果某个进程在高于sbrk()分配的任何虚拟内存地址上出现页错误，则终止该进程。

```c
uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  
  struct proc* p=myproc();
  addr = p->sz;
  int sz=p->sz;

  if (n>0){
    p->sz+=n;
  }else if (sz+n>0){
    sz=uvmdealloc(p->pagetable,sz,sz+n);
    p->sz=sz;
  }else{
    return -1;
  }
  
  return addr;
}
```

在fork()中正确处理父到子内存拷贝。将uvmcopy修改

```c
    if((pte = walk(old, i, 0)) == 0)
      continue;
    if((*pte & PTE_V) == 0)
      continue;
```

最后修改argaddr

```c
int
argaddr(int n, uint64 *ip)
{
  *ip = argraw(n);
  struct proc* p=myproc();

  if (walkaddr(p->pagetable,*ip)==0){
    if (PGROUNDUP(p->trapframe->sp)-1<*ip&&*ip<p->sz){
      char *pa=kalloc();
      if (pa==0)  return -1;
      memset(pa, 0, PGSIZE);
      if(mappages(p->pagetable, PGROUNDDOWN(*ip), PGSIZE, (uint64)pa, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
        kfree(pa);
        return -1;
      }
    }else{
      return -1;
    }
  }
  
  return 0;
}
```