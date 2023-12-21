// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

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

void
binit(void)
{
  struct buf *b;

  for (int i = 0; i < NBUCKET; i++)
  {
    initlock(&bcache.hashbuck[i].lock,"bcache");

    // Create linked list of buffers
    bcache.hashbuck[i].head.prev = &bcache.hashbuck[i].head;
    bcache.hashbuck[i].head.next = &bcache.hashbuck[i].head;
  }

  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.hashbuck[0].head.next;
    b->prev = &bcache.hashbuck[0].head;
    initsleeplock(&b->lock, "buffer");
    bcache.hashbuck[0].head.next->prev = b;
    bcache.hashbuck[0].head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
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

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
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


