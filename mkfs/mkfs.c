#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#define stat xv6_stat  // avoid clash with host struct stat
#include "kernel/types.h"
#include "kernel/fs.h"
#include "kernel/stat.h"
#include "kernel/param.h"

#ifndef static_assert
#define static_assert(a, b) do { switch (0) case 0: case (a): ; } while (0)
#endif

#define NINODES 200

// Disk layout:
// [ boot block | sb block | log | inode blocks | free bit map | data blocks ]

int nbitmap = FSSIZE/(BSIZE*8) + 1;
int ninodeblocks = NINODES / IPB + 1;
int nlog = LOGSIZE;
int nmeta;    // Number of meta blocks (boot, sb, nlog, inode, bitmap)
int nblocks;  // Number of data blocks

int fsfd;
struct superblock sb;
char zeroes[BSIZE];
uint freeinode = 1;
uint freeblock;


void balloc(int);
void wsect(uint, void*);
void winode(uint, struct dinode*);
void rinode(uint inum, struct dinode *ip);
void rsect(uint sec, void *buf);
uint ialloc(ushort type);
void iappend(uint inum, void *p, int n);

// convert to intel byte order
ushort
xshort(ushort x)
{
  ushort y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  return y;
}

uint
xint(uint x)
{
  uint y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  a[2] = x >> 16;
  a[3] = x >> 24;
  return y;
}

int
main(int argc, char *argv[])
{
  int i, cc, fd;
  uint rootino, inum, off;
  struct dirent de;
  char buf[BSIZE];
  struct dinode din;


  static_assert(sizeof(int) == 4, "Integers must be 4 bytes!");

  if(argc < 2){
    fprintf(stderr, "Usage: mkfs fs.img files...n");
    exit(1);
  }

  assert((BSIZE % sizeof(struct dinode)) == 0);
  assert((BSIZE % sizeof(struct dirent)) == 0);
  
  // argv[1] 是 fs.img 文件系统镜像文件
  fsfd = open(argv[1], O_RDWR|O_CREAT|O_TRUNC, 0666);
  if(fsfd < 0){
    perror(argv[1]);
    exit(1);
  }

  // 1 fs block = 1 disk sector
  // 元数据块数量 = boot block + sb block + log blocks + inode blocks + bit map blocks + data blocks
  nmeta = 2 + nlog + ninodeblocks + nbitmap;
  // 计算数据块数量
  nblocks = FSSIZE - nmeta;
  // 填充super block块内容
  sb.magic = FSMAGIC;  // 魔数
  sb.size = xint(FSSIZE); // 总block数量
  sb.nblocks = xint(nblocks); // 数据块数量
  sb.ninodes = xint(NINODES); // inode块数量
  sb.nlog = xint(nlog);  // 日志块数量
  sb.logstart = xint(2); // 第一个日志块块号 
  sb.inodestart = xint(2+nlog);// 第一个inode块块号
  sb.bmapstart = xint(2+nlog+ninodeblocks); // 第一个bit map块块号

  printf("nmeta %d (boot, super, log blocks %u inode blocks %u, bitmap blocks %u) blocks %d total %dn",
         nmeta, nlog, ninodeblocks, nbitmap, nblocks, FSSIZE);
  // 文件系统初始化情况下可用数据块起始块号
  freeblock = nmeta;     // the first free block that we can allocate
  // 将文件系统所有block都清零
  for(i = 0; i < FSSIZE; i++)
    wsect(i, zeroes);
  // 将buf清零
  memset(buf, 0, sizeof(buf));
  // 向buf写入super block内容
  memmove(buf, &sb, sizeof(sb));
  // 向文件系统编号为1的block写入buf的内容,也就是将super block内容写入到磁盘上
  wsect(1, buf);

  // 分配一个空闲的inode -- 该inode作为root inode的inode number
  // 这里root ionde指的是根目录对应的inode
  rootino = ialloc(T_DIR);
  assert(rootino == ROOTINO);
  
  // 每个目录下都默认存在两个目录条目项: . 和 ..
  // 清空内存中直接块结构体
  bzero(&de, sizeof(de));
  // 清空后重新赋值 -- inode number 和 文件名
  de.inum = xshort(rootino);
  strcpy(de.name, ".");
  // 把这个目录项追加到root目录对应Inode block的直接块中
  iappend(rootino, &de, sizeof(de));
  
  // 清空de inode,重用该结构体,向磁盘追加一个名为 .. 的目录文件
  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, "..");
  // 同样把这个目录项追加到对应的直接块中
  iappend(rootino, &de, sizeof(de));
   
  // 处理需要打包进文件系统镜像的剩余文件 
  // 主要是user目录下的文件
  for(i = 2; i < argc; i++){
    // get rid of "user/"
    // 移除user目录下路径名的目录前缀 -- 如果有的话就移除
    char *shortname;
    if(strncmp(argv[i], "user/", 5) == 0)
      shortname = argv[i] + 5;
    else
      shortname = argv[i];
    
    assert(index(shortname, '/') == 0);

    // 获取第i个用户lib库程序的文件描述符  
    if((fd = open(argv[i], 0)) < 0){
      perror(argv[i]);
      exit(1);
    }

    // Skip leading _ in name when writing to file system.
    // The binaries are named _rm, _cat, etc. to keep the
    // build operating system from trying to execute them
    // in place of system binaries like rm and cat.
    // 传递给mkfs程序的用户程序文件名都是_开头的二进制文件,这里将_移除
    if(shortname[0] == '_')
      shortname += 1;
    // 分配一个空闲inode
    inum = ialloc(T_FILE);
    // 清空inode,然后填入本次获取的用户程序文件相关信息
    bzero(&de, sizeof(de));
    de.inum = xshort(inum);
    strncpy(de.name, shortname, DIRSIZ);
    // 所有用户程序文件都放置在root目录下,所以这里将当前文件对应目录项追加到root目录对应的直接块中
    // 如果文件很多,可能会追加到间接块中
    iappend(rootino, &de, sizeof(de));
    
    // 将当前文件内容读取出来,追加到当前文件inode对应的block中
    while((cc = read(fd, buf, sizeof(buf))) > 0)
      iappend(inum, buf, cc);

    close(fd);
  }

  // fix size of root inode dir
  // 更新root inode的size大小
  rinode(rootino, &din);
  off = xint(din.size);
  off = ((off/BSIZE) + 1) * BSIZE;
  din.size = xint(off);
  winode(rootino, &din);
  
  // 更新bitmap block
  balloc(freeblock);

  exit(0);
}

/*
int
main(int argc, char *argv[])
{
  int i, cc, fd;
  uint rootino, inum, off;
  struct dirent de;
  char buf[BSIZE];
  struct dinode din;


  static_assert(sizeof(int) == 4, "Integers must be 4 bytes!");

  if(argc < 2){
    fprintf(stderr, "Usage: mkfs fs.img files...\n");
    exit(1);
  }

  assert((BSIZE % sizeof(struct dinode)) == 0);
  assert((BSIZE % sizeof(struct dirent)) == 0);

  fsfd = open(argv[1], O_RDWR|O_CREAT|O_TRUNC, 0666);
  if(fsfd < 0){
    perror(argv[1]);
    exit(1);
  }

  // 1 fs block = 1 disk sector
  nmeta = 2 + nlog + ninodeblocks + nbitmap;
  nblocks = FSSIZE - nmeta;

  sb.magic = FSMAGIC;
  sb.size = xint(FSSIZE);
  sb.nblocks = xint(nblocks);
  sb.ninodes = xint(NINODES);
  sb.nlog = xint(nlog);
  sb.logstart = xint(2);
  sb.inodestart = xint(2+nlog);
  sb.bmapstart = xint(2+nlog+ninodeblocks);

  printf("nmeta %d (boot, super, log blocks %u inode blocks %u, bitmap blocks %u) blocks %d total %d\n",
         nmeta, nlog, ninodeblocks, nbitmap, nblocks, FSSIZE);

  freeblock = nmeta;     // the first free block that we can allocate

  for(i = 0; i < FSSIZE; i++)
    wsect(i, zeroes);

  memset(buf, 0, sizeof(buf));
  memmove(buf, &sb, sizeof(sb));
  wsect(1, buf);

  rootino = ialloc(T_DIR);
  assert(rootino == ROOTINO);

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, ".");
  iappend(rootino, &de, sizeof(de));

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, "..");
  iappend(rootino, &de, sizeof(de));

  for(i = 2; i < argc; i++){
    // get rid of "user/"
    char *shortname;
    if(strncmp(argv[i], "user/", 5) == 0)
      shortname = argv[i] + 5;
    else
      shortname = argv[i];
    
    assert(index(shortname, '/') == 0);

    if((fd = open(argv[i], 0)) < 0){
      perror(argv[i]);
      exit(1);
    }

    // Skip leading _ in name when writing to file system.
    // The binaries are named _rm, _cat, etc. to keep the
    // build operating system from trying to execute them
    // in place of system binaries like rm and cat.
    if(shortname[0] == '_')
      shortname += 1;

    inum = ialloc(T_FILE);

    bzero(&de, sizeof(de));
    de.inum = xshort(inum);
    strncpy(de.name, shortname, DIRSIZ);
    iappend(rootino, &de, sizeof(de));

    while((cc = read(fd, buf, sizeof(buf))) > 0)
      iappend(inum, buf, cc);

    close(fd);
  }

  // fix size of root inode dir
  rinode(rootino, &din);
  off = xint(din.size);
  off = ((off/BSIZE) + 1) * BSIZE;
  din.size = xint(off);
  winode(rootino, &din);

  balloc(freeblock);

  exit(0);
}*/

// fsfd是fs.img文件系统镜像文件的文件描述符
// 将buf内容写入文件系统第sec个block中
void
wsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE){
    perror("lseek");
    exit(1);
  }
  if(write(fsfd, buf, BSIZE) != BSIZE){
    perror("write");
    exit(1);
  }
}

// 更新磁盘上对应inode的信息
void
winode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;
  // 获取当前inode所在的inode block number
  bn = IBLOCK(inum, sb);
  // 将该inode block读取到内存中来
  rsect(bn, buf);
  // 通过偏移得到buf中inode的地址
  dip = ((struct dinode*)buf) + (inum % IPB);
  // 将内存中Inode的值赋值为传入的ip
  *dip = *ip;
  // 重新将这块Block写入磁盘
  wsect(bn, buf);
}

// 从磁盘上读取出对应inode的信息,然后赋值给ip
void
rinode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *ip = *dip;
}

// 读取文件系统第sec个块到buf中
void
rsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE){
    perror("lseek");
    exit(1);
  }
  if(read(fsfd, buf, BSIZE) != BSIZE){
    perror("read");
    exit(1);
  }
}

// 分配下一个空闲的inode
uint
ialloc(ushort type)
{
  uint inum = freeinode++;
  struct dinode din;
  // 将dinode清零 
  bzero(&din, sizeof(din));
  // 清零后,重新赋值
  // inode类型,链接数和大小
  din.type = xshort(type);
  din.nlink = xshort(1);
  din.size = xint(0);
  // 将该inode写入磁盘
  winode(inum, &din);
  return inum;
}

// 分配bitmap block,used表示已经使用的block数量
void
balloc(int used)
{
  uchar buf[BSIZE];
  int i;
  printf("balloc: first %d blocks have been allocatedn", used);
  assert(used < BSIZE*8);
  bzero(buf, BSIZE);
  // 将已经使用的block,在bitmap block中对应为设置为1
  for(i = 0; i < used; i++){
    buf[i/8] = buf[i/8] | (0x1 << (i%8));
  }
  printf("balloc: write bitmap block at sector %dn", sb.bmapstart);
  // 更新bitmap block到磁盘
  wsect(sb.bmapstart, buf);
}

#define min(a, b) ((a) < (b) ? (a) : (b))

// xp是数据缓冲区,n是追加数据大小
void
iappend(uint inum, void *xp, int n)
{
  char *p = (char*)xp;
  uint fbn, off, n1;
  struct dinode din;
  char buf[BSIZE];
  uint indirect[NINDIRECT];
  uint x;
  // 从磁盘读取出inum对应的inode信息到din中
  rinode(inum, &din);
  // 每个inode代表一个文件,size表示文件的已有的数据量大小
  off = xint(din.size);
  // printf("append inum %d at off %d sz %dn", inum, off, n);
  // 如果写入数据量比较大，那么会分批次多次写入
  while(n > 0){
    // 计算写入地址在当前inode的blocks数组中对应的索引
    fbn = off / BSIZE;
    assert(fbn < MAXFILE);
    // 1.定位数据需要写入到哪个block中  
    // 默认情况下,每个Inode都有12个直接块和1个间接块
    // 如果处于直接块范畴
    if(fbn < NDIRECT){
      // 如果当前直接块条目的块号还没确定,那么赋值为当前空闲可用块的block number
      if(xint(din.addrs[fbn]) == 0){
        din.addrs[fbn] = xint(freeblock++);
      }
      // 获取这个直接块条目记录的block number
      x = xint(din.addrs[fbn]);
    } else {
      // 如果属于间接块,并且该间接块条目的块号没有确定,那么赋值为当前空闲可用块的block number
      if(xint(din.addrs[NDIRECT]) == 0){
        din.addrs[NDIRECT] = xint(freeblock++);
      }
      // 从磁盘读取出这个间接块 --- 间接块中记录的都是block number
      rsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      // 判断对应间接块中的条目是否为0,如果为0赋值为新的空闲块号
      if(indirect[fbn - NDIRECT] == 0){
        indirect[fbn - NDIRECT] = xint(freeblock++);
        // 该间接块内容被修改了,需要重新写入磁盘
        wsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      }
      // 获得对应的间接块条目号记录的block number
      x = xint(indirect[fbn-NDIRECT]);
    }
    // 2. 现在x记录着数据需要写入的block number,下一步将数据写入对应的Block中
    // 在目标写入块剩余大小和当前要写入数据大小之间取较小者 
    n1 = min(n, (fbn + 1) * BSIZE - off);
    // 读取对应目标块,然后在指定位置写入对应的数据,最后将目标块重新写入磁盘
    rsect(x, buf);
    bcopy(p, buf + off - (fbn * BSIZE), n1);
    wsect(x, buf);
    // 剩余写入数据量减少 -- 一次写不完,会分多次写入
    n -= n1;
    // 当前inode写入数据偏移量增加
    off += n1;
    // 源数据缓冲区写入指针前推
    p += n1;
  }
  // 更新当前Inode的写入偏移量,然后写入磁盘
  din.size = xint(off);
  winode(inum, &din);
}

/*
void
iappend(uint inum, void *xp, int n)
{
  char *p = (char*)xp;
  uint fbn, off, n1;
  struct dinode din;
  char buf[BSIZE];
  uint indirect[NINDIRECT];
  uint x;

  rinode(inum, &din);
  off = xint(din.size);
  // printf("append inum %d at off %d sz %d\n", inum, off, n);
  while(n > 0){
    fbn = off / BSIZE;
    assert(fbn < MAXFILE);
    if(fbn < NDIRECT){
      if(xint(din.addrs[fbn]) == 0){
        din.addrs[fbn] = xint(freeblock++);
      }
      x = xint(din.addrs[fbn]);
    } else {
      if(xint(din.addrs[NDIRECT]) == 0){
        din.addrs[NDIRECT] = xint(freeblock++);
      }
      rsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      if(indirect[fbn - NDIRECT] == 0){
        indirect[fbn - NDIRECT] = xint(freeblock++);
        wsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      }
      x = xint(indirect[fbn-NDIRECT]);
    }
    n1 = min(n, (fbn + 1) * BSIZE - off);
    rsect(x, buf);
    bcopy(p, buf + off - (fbn * BSIZE), n1);
    wsect(x, buf);
    n -= n1;
    off += n1;
    p += n1;
  }
  din.size = xint(off);
  winode(inum, &din);
}*/
