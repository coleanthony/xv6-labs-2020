## Lab8:Locks

### Memory allocator

本实验完成的任务是为每个CPU都维护一个空闲列表，初始时将所有的空闲内存分配到某个CPU，此后各个CPU需要内存时，如果当前CPU的空闲列表上没有，则窃取其他CPU的。

```c
struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

void
kinit()
{
  for (int i = 0; i < NCPU; i++){
    initlock(&kmem[i].lock, "kmem");
  }
  freerange(end, (void*)PHYSTOP);
}
```

修改kfree和kalloc，使用cpuid()和它返回的结果时必须关中断。

```c
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();
  int id = cpuid();
  pop_off();

  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);
}

void *
kalloc(void)
{
  struct run *r;

  push_off();
  int id = cpuid();
  pop_off();

  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if(r)
    kmem[id].freelist = r->next;
  else{
  //在CPU的空闲列表为空时进行窃取
    for (int i = 0; i < NCPU; i++)
    { 
      int ok=0;
      if (i!=id)
      {
        acquire(&kmem[i].lock);
        r=kmem[i].freelist;
        if (r)
        {
          kmem[i].freelist=r->next;
          ok=1;
        }
        release(&kmem[i].lock);
      }
      if (ok==1)
      {
        break;
      }
    }
  }
  release(&kmem[id].lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
```

---

### Buffer cache

根据提示：

（1）可以使用固定数量的散列桶，而不动态调整哈希表的大小。使用素数个存储桶（例如13）来降低散列冲突的可能性。

```c
#define NBUCKET 13

struct hashbucket{
  struct spinlock lock;
  struct buf head;
};

struct {
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct hashbucket hashbuck[NBUCKET];
} bcache;
```

（2）删除保存了所有缓冲区的列表（bcache.head等），改为标记上次使用时间的时间戳缓冲区（即使用kernel/trap.c中的ticks）。通过此更改，brelse不需要获取bcache锁，并且bget可以根据时间戳选择最近使用最少的块。因此在buf.h中增加新字段timestamp，这样我们就无需在brelse中进行头插法更改结点位置。

```c
struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *prev; // LRU cache list
  struct buf *next;
  uchar data[BSIZE];
  uint timestamp;
};

void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int hashblockid=b->blockno%NBUCKET;

  acquire(&bcache.hashbuck[hashblockid].lock);
  b->refcnt--;
  
  acquire(&tickslock);
  b->timestamp=ticks;
  release(&tickslock);
  
  release(&bcache.hashbuck[hashblockid].lock);
}
```

（3）修正bpin和bunpin

```c
void
bpin(struct buf *b) {
  int hashblockid=b->blockno%NBUCKET;
  acquire(&bcache.hashbuck[hashblockid].lock);
  b->refcnt++;
  release(&bcache.hashbuck[hashblockid].lock);
}

void
bunpin(struct buf *b) {
  int hashblockid=b->blockno%NBUCKET;
  acquire(&bcache.hashbuck[hashblockid].lock);
  b->refcnt--;
  release(&bcache.hashbuck[hashblockid].lock);
}
```

（4）修改bget，在bget中串行化回收（即bget中的一部分：当缓存中的查找未命中时，它选择要复用的缓冲区）。当没有找到指定的缓冲区时进行分配，分配方式是优先从当前列表遍历，找到一个没有引用且timestamp最小的缓冲区，如果没有就申请下一个桶的锁，并遍历该桶，找到后将该缓冲区从原来的桶移动到当前桶中，最多将所有桶都遍历完。注意：
- 在某些情况下，您的解决方案可能需要持有两个锁；例如，在回收过程中，您可能需要持有bcache锁和每个bucket（散列桶）一个锁。确保避免死锁。
- 替换块时，您可能会将struct buf从一个bucket移动到另一个bucket，因为新块散列到不同的bucket。您可能会遇到一个棘手的情况：新块可能会散列到与旧块相同的bucket中。在这种情况下，请确保避免死锁。

```c
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  int hashblockid=blockno%NBUCKET;
  acquire(&bcache.hashbuck[hashblockid].lock);
  // Is the block already cached?
  for(b = bcache.hashbuck[hashblockid].head.next; b != &bcache.hashbuck[hashblockid].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++; 

      //allocate a buffer and update the timestamp
      acquire(&tickslock);
      b->timestamp=ticks;
      release(&tickslock);

      release(&bcache.hashbuck[hashblockid].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  //first get from block_hashblockid
  b=0;
  for (int bid = hashblockid,count=0; count < NBUCKET; bid=(bid+1)%NBUCKET){
    count++;
    if (bid!=hashblockid){
      if (!holding(&bcache.hashbuck[bid].lock)){
        acquire(&bcache.hashbuck[bid].lock);
      }else{
        continue;
      }
    }

    struct buf *tmp;
    for(tmp = bcache.hashbuck[bid].head.next; tmp != &bcache.hashbuck[bid].head; tmp = tmp->next){
      if (tmp->refcnt==0&&(b==0||tmp->timestamp<b->timestamp)){
        b=tmp;
      }
    }

    if (b){
      //if get from another bucket, insert it into current bucket
      if (bid!=hashblockid){
        b->next->prev=b->prev;
        b->prev->next=b->next;
        release(&bcache.hashbuck[bid].lock);
        b->next = bcache.hashbuck[hashblockid].head.next;
        b->prev = &bcache.hashbuck[hashblockid].head;
        bcache.hashbuck[hashblockid].head.next->prev = b;
        bcache.hashbuck[hashblockid].head.next = b;
      }

      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;

      acquire(&tickslock);
      b->timestamp=ticks;
      release(&tickslock);

      release(&bcache.hashbuck[hashblockid].lock);
      acquiresleep(&b->lock);
      return b;
    }

    if (bid!=hashblockid){
      release(&bcache.hashbuck[bid].lock);
    }
  }

  panic("bget: no buffers");
}
```