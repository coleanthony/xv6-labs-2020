## Lab7: Multithreading

### Uthread: switching between threads

```c
uthread_switch.S
        .text

        /*
         * save the old thread's registers,
         * restore the new thread's registers.
         */

        .globl thread_switch
thread_switch:
        /* YOUR CODE HERE */
    sd ra, 0(a0)
    sd sp, 8(a0)
    sd s0, 16(a0)
    sd s1, 24(a0)
    sd s2, 32(a0)
    sd s3, 40(a0)
    sd s4, 48(a0)
    sd s5, 56(a0)
    sd s6, 64(a0)
    sd s7, 72(a0)
    sd s8, 80(a0)
    sd s9, 88(a0)
    sd s10, 96(a0)
    sd s11, 104(a0)

    ld ra, 0(a1)
    ld sp, 8(a1)
    ld s0, 16(a1)
    ld s1, 24(a1)
    ld s2, 32(a1)
    ld s3, 40(a1)
    ld s4, 48(a1)
    ld s5, 56(a1)
    ld s6, 64(a1)
    ld s7, 72(a1)
    ld s8, 80(a1)
    ld s9, 88(a1)
    ld s10, 96(a1)
    ld s11, 104(a1)

        ret    /* return to ra */
```

修改thread_schedule和thread_create

```c
void 
thread_schedule(void)
{
  struct thread *t, *next_thread;

  /* Find another runnable thread. */
  next_thread = 0;
  t = current_thread + 1;
  for(int i = 0; i < MAX_THREAD; i++){
    if(t >= all_thread + MAX_THREAD)
      t = all_thread;
    if(t->state == RUNNABLE) {
      next_thread = t;
      break;
    }
    t = t + 1;
  }

  if (next_thread == 0) {
    printf("thread_schedule: no runnable threads\n");
    exit(-1);
  }

  if (current_thread != next_thread) {         /* switch threads?  */
    next_thread->state = RUNNING;
    t = current_thread;
    current_thread = next_thread;
    /* YOUR CODE HERE
     * Invoke thread_switch to switch from t to next_thread:
     * thread_switch(??, ??);
     */
    thread_switch(&t->context,&current_thread->context);

  } else
    next_thread = 0;
}

void 
thread_create(void (*func)())
{
  struct thread *t;

  for (t = all_thread; t < all_thread + MAX_THREAD; t++) {
    if (t->state == FREE) break;
  }
  t->state = RUNNABLE;
  // YOUR CODE HERE
  memset(&t->context, 0, sizeof(t->context));
  t->context.ra = (uint64)func;
  t->context.sp = (uint64)t->stack + STACK_SIZE;
}
```

---

### Using threads

首先通过ph_safe，单纯的在put和get上加两把大锁就能通过。

```c
static 
void put(int key, int value)
{
  int i = key % NBUCKET;

  // is the key already present?
  pthread_mutex_lock(&lock); 
  struct entry *e = 0;
  for (e = table[i]; e != 0; e = e->next) {
    if (e->key == key)
      break;
  }
  if(e){
    // update the existing key.
    e->value = value;
  } else {
    // the new is new.
    insert(key, value, &table[i], table[i]);
  }
  pthread_mutex_unlock(&lock); 

}

static struct entry*
get(int key)
{
  int i = key % NBUCKET;

  pthread_mutex_lock(&lock); 
  struct entry *e = 0;
  for (e = table[i]; e != 0; e = e->next) {
    if (e->key == key) break;
  }
  pthread_mutex_unlock(&lock); 
  return e;
}
```

加锁后多线程的性能变得比单线程还要低了，虽然不会出现数据丢失，但是失去了多线程并行计算的意义：提升性能。为整个操作加上了互斥锁，每一时刻只能有一个线程在操作哈希表，这里实际上等同于将哈希表的操作变回单线程了，又由于锁操作（加锁、解锁、锁竞争）是有开销的，所以性能甚至不如单线程版本。因此需要优化。
在哈希表中，不同的 bucket 是互不影响的。只需要确保两个线程不会同时操作同一个 bucket 即可，并不需要确保不会同时操作整个哈希表。所以可以将加锁的粒度，从整个哈希表一个锁降低到每个 bucket 一个锁。

```c
static 
void put(int key, int value)
{
  int i = key % NBUCKET;

  // is the key already present?
  pthread_mutex_lock(&locks[i]); 
  struct entry *e = 0;
  for (e = table[i]; e != 0; e = e->next) {
    if (e->key == key)
      break;
  }
  if(e){
    // update the existing key.
    e->value = value;
  } else {
    // the new is new.
    insert(key, value, &table[i], table[i]);
  }
  pthread_mutex_unlock(&locks[i]); 

}

static struct entry*
get(int key)
{
  int i = key % NBUCKET;

  pthread_mutex_lock(&locks[i]); 
  struct entry *e = 0;
  for (e = table[i]; e != 0; e = e->next) {
    if (e->key == key) break;
  }
  pthread_mutex_unlock(&locks[i]); 
  return e;
}
```

初始化时也是在main中每个锁都要初始化。

---

### Barrier

```c
static void 
barrier()
{
  // YOUR CODE HERE
  //
  // Block until all threads have called barrier() and
  // then increment bstate.round.
  //
  pthread_mutex_lock(&bstate.barrier_mutex);
  bstate.nthread++;
  if (bstate.nthread==nthread){
    bstate.round++;
    bstate.nthread=0;
    pthread_cond_broadcast(&bstate.barrier_cond);
  }else{
    pthread_cond_wait(&bstate.barrier_cond, &bstate.barrier_mutex);
  }
  pthread_mutex_unlock(&bstate.barrier_mutex);
}
```



