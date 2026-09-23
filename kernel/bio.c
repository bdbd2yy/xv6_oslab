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

struct bcache_bucket {
  struct spinlock lock;
  struct buf head;
};

struct {
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct bcache_bucket hashbucket[NBUCKETS];
} bcache;

static void list_remove(struct buf *b);
static void list_insert_head(int btno, struct buf *b);
static struct buf* bsteal(uint dev, uint blockno);

void
binit(void)
{
  struct buf *b;

  for (int i = 0; i < NBUCKETS; i++) {
    initlock(&bcache.hashbucket[i].lock, "bcache");

    // Create linked list of buffers
    bcache.hashbucket[i].head.prev = &bcache.hashbucket[i].head;
    bcache.hashbucket[i].head.next = &bcache.hashbucket[i].head;
  }
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    int i = (b - bcache.buf) % NBUCKETS;
    b->next = bcache.hashbucket[i].head.next;
    b->prev = &bcache.hashbucket[i].head;
    initsleeplock(&b->lock, "buffer");
    bcache.hashbucket[i].head.next->prev = b;
    bcache.hashbucket[i].head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  int i = blockno % NBUCKETS;

  acquire(&bcache.hashbucket[i].lock);

  // Is the block already cached?
  for(b = bcache.hashbucket[i].head.next; b != &bcache.hashbucket[i].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.hashbucket[i].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(b = bcache.hashbucket[i].head.prev; b != &bcache.hashbucket[i].head; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.hashbucket[i].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  // firstly release the original hashbucket[i]'s lock
  release(&bcache.hashbucket[i].lock);
  // buffer stealing
  b = bsteal(dev, blockno);
  if (b) {
    acquiresleep(&b->lock);
    return b;
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
  int i = b->blockno % NBUCKETS;
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.hashbucket[i].lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    list_remove(b);
    list_insert_head(i, b);
  }
  
  release(&bcache.hashbucket[i].lock);
}

void
bpin(struct buf *b) {
  int i = b->blockno % NBUCKETS;
  acquire(&bcache.hashbucket[i].lock);
  b->refcnt++;
  release(&bcache.hashbucket[i].lock);
}

void
bunpin(struct buf *b) {
  int i = b->blockno % NBUCKETS;
  acquire(&bcache.hashbucket[i].lock);
  b->refcnt--;
  release(&bcache.hashbucket[i].lock);
}

static void list_remove(struct buf *b) {
  b->next->prev = b->prev;
  b->prev->next = b->next;
}

static void list_insert_head(int btno, struct buf *b) {
  b->next = bcache.hashbucket[btno].head.next;
  b->prev = &bcache.hashbucket[btno].head;
  bcache.hashbucket[btno].head.next->prev = b;
  bcache.hashbucket[btno].head.next = b;
}

static struct buf *bsteal(uint dev, uint blockno) {
  int i = blockno % NBUCKETS;
  struct buf *b;

  for (int j = 0; j < NBUCKETS; j++) {
    if (j == i) {
      continue;
    }
    // i need your lock when accessing the hashbucket[j]
    // Attention: there is a dead lock risk
    // You need to ensure that the acquiring of the two lock in some way of
    // order
    int first = i < j ? i : j;
    int second = i < j ? j : i;
    // Always acquire the low number of hashbucket's lock
    acquire(&bcache.hashbucket[first].lock);
    acquire(&bcache.hashbucket[second].lock);
    // check if the block was loaded or there is free buffer can be victim
    // during the period when the lock of hashbucket[i] is released
    for (b = bcache.hashbucket[i].head.prev; b != &bcache.hashbucket[i].head;
         b = b->prev) {
      if (b->dev == dev && b->blockno == blockno) {
        b->refcnt++;
        release(&bcache.hashbucket[second].lock);
        release(&bcache.hashbucket[first].lock);
        return b;
      }
      if (b->refcnt == 0) {
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        release(&bcache.hashbucket[second].lock);
        release(&bcache.hashbucket[first].lock);
        return b;
      }
    }
    // try to steal buffer from hashbucket[j]
    for (b = bcache.hashbucket[j].head.prev; b != &bcache.hashbucket[j].head;
         b = b->prev) {
      if (b->refcnt == 0) {
        // you are the victim in hashbucket[j]
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;

        // release your relationship in the hashbucket[j]
        list_remove(b);

        // establish new relationship in the hashbucket[i]
        list_insert_head(i, b);

        // releasing locks reversely is a good programming practice
        release(&bcache.hashbucket[second].lock);
        release(&bcache.hashbucket[first].lock);
        return b;
      }
    }
    release(&bcache.hashbucket[second].lock);
    release(&bcache.hashbucket[first].lock);
  }
  return 0;
}
