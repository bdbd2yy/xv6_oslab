// Buffer cache.
//
// The buffer cache uses per-bucket circular lists of buf structures holding
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
  struct buf *hand; // Next node to inspect in the circular CLOCK scan.
};

struct {
  struct buf buf[NBUF];

  // Per-bucket circular lists of buffers, through prev/next.
  // Each bucket uses CLOCK to select an idle buffer for replacement.
  struct bcache_bucket hashbucket[NBUCKETS];
} bcache;

static void list_remove(struct buf *b);
static void list_insert_head(int btno, struct buf *b);
static struct buf* bsteal(uint dev, uint blockno);
static struct buf *find_victim_locked(struct bcache_bucket *bucket);

void
binit(void)
{
  struct buf *b;

  for (int i = 0; i < NBUCKETS; i++) {
    initlock(&bcache.hashbucket[i].lock, "bcache");

    // Create linked list of buffers
    bcache.hashbucket[i].head.prev = &bcache.hashbucket[i].head;
    bcache.hashbucket[i].head.next = &bcache.hashbucket[i].head;
    bcache.hashbucket[i].hand = &bcache.hashbucket[i].head;
  }
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    int i = (b - bcache.buf) % NBUCKETS;
    b->referenced = 0;
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
      b->referenced = 1;
      release(&bcache.hashbucket[i].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Use CLOCK to choose an idle victim.
  struct buf *victim = find_victim_locked(&bcache.hashbucket[i]);
  if (victim) {
    victim->dev = dev;
    victim->blockno = blockno;
    victim->valid = 0;
    victim->refcnt = 1;
    victim->referenced = 1;
    release(&bcache.hashbucket[i].lock);
    acquiresleep(&victim->lock);
    return victim;
  }

  // firstly release the original hashbucket[i]'s lock
  release(&bcache.hashbucket[i].lock);
  // buffer stealing
  victim = bsteal(dev, blockno);
  if (victim) {
    acquiresleep(&victim->lock);
    return victim;
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
void
brelse(struct buf *b)
{
  int i = b->blockno % NBUCKETS;
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.hashbucket[i].lock);
  b->refcnt--;
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
    // Attention: there is a dead lock risk
    // You need to ensure that the acquiring of the two lock in some way of
    // order
    int first = i < j ? i : j;
    int second = i < j ? j : i;
    // Always acquire the low number of hashbucket's lock to avoid dead lock
    acquire(&bcache.hashbucket[first].lock);
    acquire(&bcache.hashbucket[second].lock);
    // there is a period when the lock of hashbucket[i] is released
    // check if the block was loaded
    for (b = bcache.hashbucket[i].head.prev; b != &bcache.hashbucket[i].head;
         b = b->prev) {
      if (b->dev == dev && b->blockno == blockno) {
        b->refcnt++;
        b->referenced = 1;
        release(&bcache.hashbucket[second].lock);
        release(&bcache.hashbucket[first].lock);
        return b;
      }
    }
    // check if there is free buffer can be victim
    struct buf *victim = find_victim_locked(&bcache.hashbucket[i]);
    if (victim) {
      victim->dev = dev;
      victim->blockno = blockno;
      victim->valid = 0;
      victim->refcnt = 1;
      victim->referenced = 1;
      release(&bcache.hashbucket[second].lock);
      release(&bcache.hashbucket[first].lock);
      return victim;
    }
    // try to steal buffer from hashbucket[j]
    victim = find_victim_locked(&bcache.hashbucket[j]);
    if (victim) {
      victim->dev = dev;
      victim->blockno = blockno;
      victim->valid = 0;
      victim->refcnt = 1;
      victim->referenced = 1;

      // The CLOCK scan advanced j's hand before returning victim.
      // Move the buffer from list j to list i.
      list_remove(victim);
      list_insert_head(i, victim);

      release(&bcache.hashbucket[second].lock);
      release(&bcache.hashbucket[first].lock);
      return victim;
    }
    release(&bcache.hashbucket[second].lock);
    release(&bcache.hashbucket[first].lock);
  }
  return 0;
}

// Caller must hold bucket->lock. The hand stays in this bucket even when
// the returned victim is subsequently moved to another bucket.
static struct buf *find_victim_locked(struct bcache_bucket *bucket) {
  // the list is empty
  if (bucket->head.next == &bucket->head)
    return 0;

  // One pass clears reference bits; a second can select an idle buffer.
  // Stop after two passes even if every buffer is in use or pinned.
  for (int pass = 0; pass < 2; pass++) {
    struct buf *start = bucket->hand;
    do {
      // The hand may point at the sentinel between scans.
      if (bucket->hand == &bucket->head) {
        bucket->hand = bucket->head.next;
        continue;
      }
      struct buf *b = bucket->hand;
      // Advance before returning so removing b cannot move the hand into
      // another bucket.
      bucket->hand = b->next;

      if (b->refcnt == 0) {
        if (b->referenced == 0)
          return b;
        b->referenced = 0;
      }
    } while (bucket->hand != start);
  }
  return 0;
}
