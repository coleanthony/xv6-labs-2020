## Lab9:fs

### Large files
修改bmap()，以便除了直接块和一级间接块之外，它还实现二级间接块。你只需要有11个直接块，而不是12个，为你的新的二级间接块腾出空间；不允许更改磁盘inode的大小。ip->addrs[]的前11个元素应该是直接块；第12个应该是一个一级间接块（与当前的一样）；13号应该是你的新二级间接块。

理解bmap()。写出ip->addrs[]、间接块、二级间接块和它所指向的一级间接块以及数据块之间的关系图。更改NDIRECT的定义。确保struct inode和struct dinode在其addrs[]数组中具有相同数量的元素。

```c
#define NDIRECT 11
#define NINDIRECT (BSIZE / sizeof(uint))
#define TWOLEVELDIRECT (BSIZE / sizeof(uint))*(BSIZE / sizeof(uint))
#define MAXFILE (NDIRECT + NINDIRECT+TWOLEVELDIRECT)
#define NADDR_PER_BLOCK (BSIZE / sizeof(uint))

// On-disk inode structure
struct dinode {
  short type;           // File type
  short major;          // Major device number (T_DEVICE only)
  short minor;          // Minor device number (T_DEVICE only)
  short nlink;          // Number of links to inode in file system
  uint size;            // Size of file (bytes)
  uint addrs[NDIRECT+2];   // Data block addresses
};

struct inode {
  uint dev;           // Device number
  uint inum;          // Inode number
  int ref;            // Reference count
  struct sleeplock lock; // protects everything below here
  int valid;          // inode has been read from disk?

  short type;         // copy of disk inode
  short major;
  short minor;
  short nlink;
  uint size;
  uint addrs[NDIRECT+2];
};
```

更改bmap

```c
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0)
      ip->addrs[bn] = addr = balloc(ip->dev);
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0)
      ip->addrs[NDIRECT] = addr = balloc(ip->dev);
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      a[bn] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }
  bn-=NINDIRECT;

  if (bn<TWOLEVELDIRECT){
    uint level1_id=bn/NADDR_PER_BLOCK;
    uint level2_id=bn%NADDR_PER_BLOCK;
    
    //分配一级索引
    if((addr = ip->addrs[NDIRECT+1]) == 0)
      ip->addrs[NDIRECT+1] = addr = balloc(ip->dev);
    bp=bread(ip->dev,addr);
    a = (uint*)bp->data;

    //分配二级索引
    if((addr = a[level1_id]) == 0){
      a[level1_id] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    
    bp=bread(ip->dev,addr);
    a = (uint*)bp->data;
    if((addr = a[level2_id]) == 0){
      a[level2_id] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }

  panic("bmap: out of range");
}
```

更改itrunc

```c
void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;

  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j])
        bfree(ip->dev, a[j]);
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  if (ip->addrs[NDIRECT+1]){
    bp = bread(ip->dev, ip->addrs[NDIRECT+1]);
    a = (uint*)bp->data;
    for (i = 0; i < NADDR_PER_BLOCK; i++){
      if (a[i]){
        struct  buf *tmpbp=bread(ip->dev,a[i]);
        uint *tmpa=(uint*)tmpbp->data;
        for (int j = 0; j < NINDIRECT; j++){
          if (tmpa[j]){
            bfree(ip->dev,tmpa[j]);
          }
        }
        brelse(tmpbp);
        bfree(ip->dev,a[i]);
        a[i]=0;
      }
    }
    brelse(bp);
    bfree(ip->dev,ip->addrs[NDIRECT+1]);
    ip->addrs[NDIRECT+1]=0;
  }

  ip->size = 0;
  iupdate(ip);
}
```

---

### Symbolic links
在本练习中，将向xv6添加符号链接。符号链接（或软链接）是指按路径名链接的文件；当一个符号链接打开时，内核跟随该链接指向引用的文件。符号链接类似于硬链接，但硬链接仅限于指向同一磁盘上的文件，而符号链接可以跨磁盘设备。

（1）为symlink创建一个新的系统调用号，在user/usys.pl、user/user.h中添加一个条目，并在kernel/sysfile.c中实现一个空的sys_symlink。这里就不放代码了，参考之前添加系统调用的标准步骤。

（2）向kernel/stat.h添加新的文件类型（T_SYMLINK）以表示符号链接。在kernel/fcntl.h中添加一个新标志（O_NOFOLLOW），该标志可用于open系统调用。

（3）实现symlink(target, path)系统调用，以在path处创建一个新的指向target的符号链接。请注意，系统调用的成功不需要target已经存在。您需要选择存储符号链接目标路径的位置，例如在inode的数据块中。symlink应返回一个表示成功（0）或失败（-1）的整数，类似于link和unlink。

```c
uint64
sys_symlink(void){
  char target[MAXPATH], path[MAXPATH];
  struct inode *ip;

  // 从用户态获取参数 old 和 new，分别表示旧路径和新路径
  if(argstr(0, target, MAXPATH) < 0 || argstr(1, path, MAXPATH) < 0)
    return -1;

  // 开始文件系统操作事务
  begin_op();

  // create 函数返回锁定的 inode
  ip=create(path, T_SYMLINK, 0, 0);
  if(ip == 0){
    end_op();
    return -1;
  }

  if (writei(ip,0,(uint64)target,0,MAXPATH)<MAXPATH){
    iunlockput(ip);
    end_op();
    return -1;
  }
  
  // 解锁目录的 inode 并释放引用
  iunlockput(ip);
  // 结束文件系统操作事务
  end_op();
  return 0;
}
```

（4）修改open系统调用以处理路径指向符号链接的情况。如果文件不存在，则打开必须失败。当进程向open传递O_NOFOLLOW标志时，open应打开符号链接（而不是跟随符号链接）。如果链接文件也是符号链接，则必须递归地跟随它，直到到达非链接文件为止。如果链接形成循环，则必须返回错误代码。你可以通过以下方式估算存在循环：通过在链接深度达到某个阈值（例如10）时返回错误代码。

```c
uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if (ip->type==T_SYMLINK&&!(omode&O_NOFOLLOW)){
    // 若符号链接指向的仍然是符号链接，则递归地跟随它，直到找到真正指向的文件
    // 但深度不能超过 10
    for (int i = 0; i < 10; i++){
      if(readi(ip, 0, (uint64)path, 0, MAXPATH) != MAXPATH){
        iunlockput(ip);
        end_op();
        return -1;
      }
      iunlockput(ip);
      if((ip = namei(path)) == 0){
        end_op();
        return -1;
      }
      ilock(ip);
      if (ip->type!=T_SYMLINK){
        break;
      }
    }
    if (ip->type==T_SYMLINK){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }
  
  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}
```

