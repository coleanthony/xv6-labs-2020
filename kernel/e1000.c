#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"

#define TX_RING_SIZE 16
// 网卡的发送数据队列 tx_ring, 其中每个元素是一个发送数据描述符, 
// 同时有 addr 字段指向发送的以太网帧数据的缓冲区地址, 对应 tx_mbufs.
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");
// 拿到e1000寄存器的内存地址
  regs = xregs;

  // Reset the device
  // 重置设备: 禁用中断->触发设置重置操作->再次禁用中断
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  // 传输初始化
  // 1.tx_ring传输描述符数组清空
  memset(tx_ring, 0, sizeof(tx_ring));
  // 2.设置每个传输描述符状态为可用 
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0;
  }
  // 将传输描述符数组基地址赋值给传输描述符基地址寄存器
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  // 设置传输描述符长度寄存器 
  regs[E1000_TDLEN] = sizeof(tx_ring);
  // 设置传输描述符头寄存器和尾寄存器
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  // 接收初始化
  // 1.rx_ring接收描述符数组清空
  memset(rx_ring, 0, sizeof(rx_ring));
  // 2.为每个接收描述符每个一个mbuf
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

int
e1000_transmit(struct mbuf *m)
{
  //
  // Your code here.
  //
  // the mbuf contains an ethernet frame; program it into
  // the TX descriptor ring so that the e1000 sends it. Stash
  // a pointer so that it can be freed after sending.
  //
  uint32 tail;
  struct tx_desc *desc;
  acquire(&e1000_lock);
  tail=regs[E1000_TDT];
  desc=&tx_ring[tail];
  if ((desc->status&E1000_TXD_STAT_DD)==0){
    release(&e1000_lock);
    return -1;
  }
  
  if (tx_mbufs[tail]){
    mbuffree(tx_mbufs[tail]);
  }
  
  desc->addr=(uint64)m->head;
  desc->length=m->len;
  desc->cmd=E1000_TXD_CMD_EOP|E1000_TXD_CMD_RS;
  tx_mbufs[tail]=m;

  __sync_synchronize();
  
  regs[E1000_TDT]=(tail+1)%TX_RING_SIZE;
  release(&e1000_lock);
  return 0;
}

static void
e1000_recv(void)
{
  //
  // Your code here.
  //
  // Check for packets that have arrived from the e1000
  // Create and deliver an mbuf for each packet (using net_rx()).
  //
  uint32 tail=(regs[E1000_RDT]+1)%RX_RING_SIZE;
  struct rx_desc *desc=&rx_ring[tail];
  while(desc->status&E1000_RXD_STAT_DD){
    if (desc->length>MBUF_SIZE){
      panic("e1000 len");
    }
    rx_mbufs[tail]->len=desc->length;
    net_rx(rx_mbufs[tail]);

    rx_mbufs[tail]=mbufalloc(0);
    desc->addr=(uint64)rx_mbufs[tail]->head;
    desc->status=0;
    
    tail=(tail+1)%RX_RING_SIZE;
    desc=&rx_ring[tail];
  }
  regs[E1000_RDT]=(tail-1)%RX_RING_SIZE;
}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
