## Lab2 : Syscall

### System call tracing

基本上就是照着实验手册翻译一遍

1.在Makefile的UPROGS中添加$U/_trace

2.将系统调用的原型添加到user/user.h，存根添加到user/usys.pl，以及将系统调用编号添加到kernel/syscall.h

```c
// user/user.h
// system calls
// ...
int trace(int);

# user/usys.pl
#...
entry("trace");

// kernel/syscall.h
// ...
#define SYS_trace  22
```

3.在kernel/sysproc.c中添加一个sys_trace()函数

```c
uint64
sys_trace(void){
  int tracemask;
  if(argint(0, &tracemask) < 0)
    return -1;
  myproc()->tracemask=tracemask;
  return 0;
} 
```

4.修改fork()（请参阅kernel/proc.c）将跟踪掩码从父进程复制到子进程。

在proc.h proc结构新增tracemask

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
  struct trapframe *trapframe; // data page for trampoline.S
  struct context context;      // swtch() here to run process
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
  uint64 tracemask;            // syscall mask
};
```

在proc.c  allocproc函数新增、freeproc函数新增 、fork新增

```c
  // ...
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  p->tracemask=0;  //<--
  return p;

  // ...
  p->xstate = 0;
  p->state = UNUSED;
  p->tracemask = 0;   //<--


  np->tracemask=p->tracemask;
```

5.修改kernel/syscall.c中的syscall()函数以打印跟踪输出。syscall.c中对应的extern函数都要加，static uint64 (*syscalls[])(void)也要加上trace

```c
char *tracenames[]={
    "",
    "fork",
    "exit",
    "wait",
    "pipe",
    "read",
    "kill", 
    "exec",    
    "fstat",   
    "chdir",  
    "dup",    
    "getpid", 
    "sbrk",   
    "sleep",  
    "uptime", 
    "open" ,  
    "write",  
    "mknod",  
    "unlink", 
    "link",   
    "mkdir",  
    "close",  
    "trace",  
};

void
syscall(void)
{
  int num;
  struct proc *p = myproc();

  num = p->trapframe->a7;
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    p->trapframe->a0 = syscalls[num]();
    if ((1<<num)&p->tracemask){
      printf("%d: syscall %s -> %d\n",p->pid,tracenames[num],p->trapframe->a0);
    }
  } else {
    printf("%d %s: unknown sys call %d\n",
            p->pid, p->name, num);
    p->trapframe->a0 = -1;
  }
}
```

---

### Sysinfo

如何添加系统调用和trace是一样的，这里主要展示三个该lab主要写的三个函数。

- 要获取空闲内存量，在kernel/kalloc.c中添加一个函数

```c
uint64 getfreemem(void){
  struct run *r;
  uint64 freemem=0;
  acquire(&kmem.lock);
  r=kmem.freelist;
  while (r){
    freemem+=PGSIZE;
    r=r->next;
  }
  release(&kmem.lock);
  return freemem;
}
```

- 要获取进程数，在kernel/proc.c中添加一个函数

```c
uint64 getnproc(void){
  struct proc *p;
  uint64 nproc=0;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state != UNUSED) {
      nproc++;
    } 
    release(&p->lock);
  }
  return nproc;
}
```

- 新增sysinfo系统调用

```c
uint64 sys_sysinfo(void)
{
  uint64 addr;
  if(argaddr(0, &addr) < 0)
    return -1;
  struct proc *p=myproc();
  struct sysinfo s;
  s.freemem=getfreemem();
  s.nproc=getnproc();
  if(copyout(p->pagetable, addr, (char *)&s, sizeof(s)) < 0)
    return -1;
  return 0;
}
```