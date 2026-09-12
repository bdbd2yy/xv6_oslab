#include "kernel/types.h"
#include "user/user.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

// Directory is a file containing a sequence of dirent structures.
// #define DIRSIZ 14
//
// struct dirent {
//   ushort inum;
//   char name[DIRSIZ];
// };

// #define T_DIR     1   // Directory
// #define T_FILE    2   // File
// #define T_DEVICE  3   // Device
//
// struct stat {
//   int dev;     // File system's disk device
//   uint ino;    // Inode number
//   short type;  // Type of file
//   short nlink; // Number of links to file
//   uint64 size; // Size of file in bytes
// };

void find(char *path, char *target);
char *getname(char *path);

int main(int argc, char *argv[]) {
  if (argc < 3) {
    printf("error: missing argument");
    exit(1);
  }
  find(argv[1], argv[2]);
  exit(0);
}

void find(char *path, char *target) {
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;

  // you need the fd to read the content of the file
  // if (stat(path, &st) < 0) {
  //   printf("find: cann't fstat %s", path);
  //   return;
  // }
  if ((fd = open(path, O_RDONLY)) < 0) {
    fprintf(2, "find: cann't open %s\n", path);
    return;
  }
  if (fstat(fd, &st) < 0) {
    fprintf(2, "find: cann't fstat %s\n", path);
    close(fd);
    return;
  }
  // cmp and print
  if (!strcmp(getname(path), target)) {
    printf("%s\n", path);
  }
  switch (st.type) {
    case T_DIR:
      // concatenate parent directory name and find the target in each path recursively
      if (strlen(path) + 1 + DIRSIZ + 1 > sizeof(buf)) {
        printf("find: path too long\n");
        return;
      }
      strcpy(buf, path);
      p = buf + strlen(buf);
      *p++ = '/';
      while (read(fd, &de, sizeof(de)) == sizeof(de)) {
        // exclude the current dir and parent dir
        if (de.inum == 0) continue;
        if (!strcmp(de.name, ".") || !strcmp(de.name, "..")) {
          continue;
        }
        memmove(p, de.name, DIRSIZ);
        p[DIRSIZ] = 0;
        find(buf, target);
      }
      break;
    case T_FILE:
      // do nothing
      break;
  }
  close(fd);
}

char *getname(char *path) {
  char *p;

  // Find first character after last slash.
  for (p = path + strlen(path); p >= path && *p != '/'; p--);
  p++;

  return p;
}
