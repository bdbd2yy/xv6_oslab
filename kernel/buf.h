struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  uint referenced; // CLOCK reference bit, protected by the bucket lock
  struct buf *prev; // Bucket circular list
  struct buf *next;
  uchar data[BSIZE];
};

