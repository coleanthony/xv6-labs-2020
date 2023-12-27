## lab10:mmap

### mmap

1. 首先，向UPROGS添加_mmaptest，以及mmap和munmap系统调用，以便让user/mmaptest.c进行编译。现在，只需从mmap和munmap返回错误。在kernel/fcntl.h中为您定义了PROT_READ等。运行mmaptest，它将在第一次mmap调用时失败。添加系统调用的过程与之前相同。

```c
#ifdef LAB_MMAP
#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#endif
```

2. 跟踪mmap为每个进程映射的内容。定义与课程中描述的VMA（虚拟内存区域）对应的结构体，记录mmap创建的虚拟内存范围的地址、长度、权限、文件等。由于xv6内核中没有内存分配器，因此可以声明一个固定大小的VMA数组，并根据需要从该数组进行分配。大小为16应该就足够了。

```c
struct vma{
  int used;
  uint addr;
  int len;
  int prot;
  int flags;
  int vfd;
  struct file* fd;
  int offset;
};
// Per-process state
struct proc {
  //
  struct vma vmas[NVMA];
};
```

3. 实现mmap：在进程的地址空间中找到一个未使用的区域来映射文件，并将VMA添加到进程的映射区域表中。VMA应该包含指向映射文件对应struct file的指针；mmap应该增加文件的引用计数，以便在文件关闭时结构体不会消失。同时，mmap不会分配物理内存或读取文件。像在lazy page allocation实验中一样，在usertrap中（或由usertrap调用）的页面错误处理代码中分配物理内存或读取文件。这可以确保大文件的mmap是快速的，并且比物理内存大的文件的mmap是可能的。

  参考lazy实验中的分配方法（将当前p->sz作为分配的虚拟起始地址，但不实际分配物理页面），此函数写在sysfile.c中就可以使用静态函数argfd同时解析文件描述符和struct file

 ```c
uint64
sys_mmap(void){
  uint64 addr;
  int len,prot,flags,vfd,offset;
  struct file* vfile;
  uint64 err=0xffffffffffffffff;
  struct proc *p=myproc();
  if (argaddr(0,&addr)<0||argint(1,&len)<0||argint(2,&prot)<0||argint(3,&flags)<0||argfd(4,&vfd,&vfile)||argint(5,&offset)<0){
    return err;
  }
  if (addr!=0||offset!=0||len<0){
    return err;
  }
  if (p->sz+len>MAXVA){
    return err;
  }
  if (!vfile->writable&&(prot&PROT_WRITE)!=0&&flags==MAP_SHARED){
    return err;
  }
  for (int i = 0; i < NVMA; i++){
    if (p->vmas[i].used==0){
      p->vmas[i].used=1;
      p->vmas[i].addr=p->sz;
      p->vmas[i].len=len;
      p->vmas[i].prot=prot;
      p->vmas[i].flags=flags;
      p->vmas[i].vfd=vfd;
      p->vmas[i].vfile=vfile;
      p->vmas[i].offset=offset;
      filedup(vfile);

      p->sz+=len;

      return p->vmas[i].addr;
    }
  }
  return err;
}
```

4. 修改usertrap，主要完成三项工作：分配物理页面，读取文件内容，添加映射关系。用户试图去访问mmap所返回的地址时, 由于我们没有分配物理页, 将会触发缺页中断。我们就需要在usertrap里把对应offset的文件内容读到一个新分配的物理页中, 并把这个物理页加入这个进程的虚拟内存映射表里。使用readi读取文件，它接受一个偏移量参数，在该偏移处读取文件（但必须lock/unlock传递给readi的索引结点）。同时不要忘记在页面上正确设置权限。

```c
//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
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
  } else if(r_scause()==13||r_scause()==15){
    uint64 va=r_stval();
    struct proc* p=myproc();
    if (va>MAXVA||va>p->sz){
      p->killed=1;
      goto bad;
    }
    int i;
    for (i = 0; i < NVMA; i++){
      if (p->vmas[i].used&&p->vmas[i].addr<=va&&va<=p->vmas[i].addr+ p->vmas[i].len - 1){
        break;
      }
    }
    if (i==NVMA){
      p->killed=1;
      goto bad;
    }
    int pte_flags=PTE_U;
    if (p->vmas[i].prot&PROT_READ)  pte_flags|=PTE_R;
    if (p->vmas[i].prot&PROT_WRITE) pte_flags|=PTE_W;
    if (p->vmas[i].prot&PROT_EXEC)  pte_flags|=PTE_X;
    
    struct file* vf=p->vmas[i].vfile;
    // cause == 13：读取访问导致的页面故障（Load Page Fault）
    // cause == 15：写入访问导致的页面故障（Store Page Fault）
    if(r_scause() == 13 && vf->readable == 0){
      p->killed=1;
      goto bad;
    }
    if(r_scause() == 15 && vf->writable == 0){
      p->killed=1;
      goto bad;
    }

    void *pa=kalloc();
    if (pa==0){
        p->killed=1;
        goto bad;
    }
    
    memset(pa,0,PGSIZE);

    ilock(vf->ip);
    // 计算当前页面读取文件的偏移量，实验中p->vma[i].offset总是0
    // 要按顺序读读取，例如内存页面A,B和文件块a,b
    int offset=p->vmas[i].offset+PGROUNDDOWN(va-p->vmas[i].addr);
    int readbytes=readi(vf->ip,0,(uint64)pa,offset,PGSIZE);
    if (readbytes==0)
    {
      iunlock(vf->ip);
      kfree(pa);
      p->killed=1;
      goto bad;
    }
    iunlock(vf->ip);
    
    if(mappages(p->pagetable, PGROUNDDOWN(va), PGSIZE, (uint64)pa, pte_flags) != 0){
      kfree(pa);
      printf("usertrap:mappages error");
      p->killed=1;
    }

  } else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

 bad:
  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

  usertrapret();
}
```

5. munmap(addr, length)应删除指定地址范围内的mmap映射。如果进程修改了内存并将其映射为MAP_SHARED，则应首先将修改写入文件。

实现munmap：找到地址范围的VMA并取消映射指定页面（提示：使用uvmunmap）。如果munmap删除了先前mmap的所有页面，它应该减少相应struct file的引用计数。如果未映射的页面已被修改，并且文件已映射到MAP_SHARED，使用filewrite将页面写回该文件。mmaptest不检查非脏页是否没有回写，可以不用看脏位就写回页面。

```c
uint64
sys_munmap(void){
  uint64 addr;
  int len;
  if (argaddr(0,&addr)<0||argint(1,&len)<0)
    return -1;

  int i;
  struct proc* p=myproc();
  
  for (i = 0; i < NVMA; i++){
    if (p->vmas[i].used&&p->vmas[i].len>=len){
      if (p->vmas[i].addr==addr){
        p->vmas[i].addr+=len;
        p->vmas[i].len-=len;
        break;
      }
      if (addr+len==p->vmas[i].addr+p->vmas[i].len){
        p->vmas[i].len-=len;
        break;
      }  
    }
  }
  if (i==NVMA){
    return -1;
  }
  
  if (p->vmas[i].flags==MAP_SHARED&&(p->vmas[i].prot&PROT_WRITE)!=0){
    filewrite(p->vmas[i].vfile,addr,len);
  }

  uvmunmap(p->pagetable, addr, len/PGSIZE, 1);
  //如果munmap删除了先前mmap的所有页面，它应该减少相应struct file的引用计数
  if (p->vmas[i].len==0){
    fileclose(p->vmas[i].vfile);
    p->vmas[i].used=0;
  }
  
  return 0;
}
```

6. 修改exit将进程的已映射区域取消映射，就像调用了munmap一样。同时由于对惰性分配的页面调用了uvmunmap，或者子进程在fork中调用uvmcopy复制了父进程惰性分配的页面都会导致panic，因此需要修改uvmunmap和uvmcopy检查PTE_V后不再panic，而是直接continue。

```c
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  for (int i = 0; i < NVMA; i++){
    if (p->vmas[i].used){
      if (p->vmas[i].flags==MAP_SHARED&&(p->vmas[i].prot&PROT_WRITE)!=0){
        filewrite(p->vmas[i].vfile,p->vmas[i].addr,p->vmas[i].len);
      }
      fileclose(p->vmas[i].vfile);
      uvmunmap(p->pagetable, p->vmas[i].addr, p->vmas[i].len/PGSIZE, 1);
      p->vmas[i].used=0;
    }
  }
  

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  // we might re-parent a child to init. we can't be precise about
  // waking up init, since we can't acquire its lock once we've
  // acquired any other proc lock. so wake up init whether that's
  // necessary or not. init may miss this wakeup, but that seems
  // harmless.
  acquire(&initproc->lock);
  wakeup1(initproc);
  release(&initproc->lock);

  // grab a copy of p->parent, to ensure that we unlock the same
  // parent we locked. in case our parent gives us away to init while
  // we're waiting for the parent lock. we may then race with an
  // exiting parent, but the result will be a harmless spurious wakeup
  // to a dead or wrong process; proc structs are never re-allocated
  // as anything else.
  acquire(&p->lock);
  struct proc *original_parent = p->parent;
  release(&p->lock);
  
  // we need the parent's lock in order to wake it up from wait().
  // the parent-then-child rule says we have to lock it first.
  acquire(&original_parent->lock);

  acquire(&p->lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup1(original_parent);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&original_parent->lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}
```

7. 修改fork以确保子对象具有与父对象相同的映射区域。不要忘记增加VMA的struct file的引用计数。在子进程的页面错误处理程序中，可以分配新的物理页面，而不是与父级共享页面。添加

```c
  for (i = 0; i < NVMA; i++){
    if (p->vmas[i].used){
      memmove(&np->vmas[i],&p->vmas[i],sizeof(p->vmas[i]));
      filedup(p->vmas[i].vfile);
    }
  }
```