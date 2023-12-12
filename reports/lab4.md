## Lab4 : Trap

### RISC-V assembly 

```
1. Which registers contain arguments to functions? 
For example, which register holds 13 in main's call to printf?
a0-a7都会保存函数的参数，a2保存13

2. Where is the call to function f in the assembly code for main? 
Where is the call to g? (Hint: the compiler may inline functions.)
f(8)+1为12，放在了a1

3. At what address is the function printf located?
从 34:        600080e7                  jalr        1536(ra) # 630 <printf> 可以看出，放在了0x630

4. What value is in the register ra just after the jalr to printf in main?
jalr指令会将当前PC+4保存在ra中,0x38

5. Run the following code.

           unsigned int i = 0x00646c72;
           printf("H%x Wo%s", 57616, &i);

   What is the output? Here's an ASCII table that maps bytes to characters.
   The output depends on that fact that the RISC-V is little-endian.
   If the RISC-V were instead big-endian what would you set i to in order to yield the same output? 
   Would you need to change 57616 to a different value?

He110 World
若为大端存储，i应改为0x726c6400，不需改变57616

6. In the following code, what is going to be printed after 'y='? 
(note: the answer is not a specific value.) Why does this happen?

printf("x=%d y=%d", 3);

原本需要两个参数，却只传入了一个，因此y=后面打印的结果取决于之前a2中保存的数据
```

---

### Alarm(Hard)

proc新增字段，allocproc和freeproc中注意初始化和析构

```c
int alarminterval;
void (*handler)();
int tickacount;
int in_alarm;
struct trapframe *trapframe_cpy;
```

usertrap主要修改：

```c
  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2){
    p->tickacount++;
    if (p->tickacount==p->alarminterval&&p->in_alarm==0)
    {
      memmove(myproc()->trapframe_cpy,myproc()->trapframe,sizeof(struct trapframe));
      p->in_alarm=1;
      p->tickacount=0;
      p->trapframe->epc=(uint64)p->handler;
    }
    yield();
  }
```

添加两个系统调用，具体步骤和lab2一样。

```c
uint64
sys_sigreturn(void){
  memmove(myproc()->trapframe,myproc()->trapframe_cpy,sizeof(struct trapframe));
  myproc()->in_alarm=0;
  return 0;
}

uint64
sys_sigalarm(void){
  struct proc *p = myproc();
  int interval;
  if (argint(0,&interval)<0)
  {
    return -1;
  }
  uint64 h;
  if (argaddr(1,&h)<0)
  {
    return -1;
  }
  p->alarminterval=interval;
  p->handler=(void(*)())h;
  p->tickacount=0;

  return 0;
}
```