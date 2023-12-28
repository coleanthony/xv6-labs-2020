//
// simple PCI-Express initialization, only
// works for qemu and its e1000 card.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

void
pci_init()
{
  // we'll place the e1000 registers at this address.
  // vm.c maps this range.
  // e1000寄存器的地址
  uint64 e1000_regs = 0x40000000L;

  // qemu -machine virt puts PCIe config space here.
  // vm.c maps this range.
   // PCI配置空间的地址
  uint32  *ecam = (uint32 *) 0x30000000L;
  
  // look at each possible PCI device on bus 0.
  // "bus 0"指的是PCI总线上的第一个总线 
  // 使用循环遍历PCI总线上每个设备
  for(int dev = 0; dev < 32; dev++){
    int bus = 0;
    int func = 0;
    int offset = 0;
    // 构造访问PCI配置空间的偏移量（offset）
    uint32 off = (bus << 16) | (dev << 11) | (func << 8) | (offset);
    // 通过将基地址（ecam）与偏移量相加，得到指向设备的寄存器的指针（base）
    volatile uint32 *base = ecam + off;
    uint32 id = base[0];
    
    // 100e:8086 is an e1000
    if(id == 0x100e8086){
      // command and status register.
      // bit 0 : I/O access enable
      // bit 1 : memory access enable
      // bit 2 : enable mastering
      base[1] = 7;
      __sync_synchronize();

      // 对设备的BAR（Base Address Registers）进行处理，通过向BAR写入全1的值，然后再恢复原来的值，从而获取BAR的大小
      // 通过将全1的值写入BAR寄存器，促使设备对BAR进行处理，并返回BAR对应资源的大小。
      // 设备在接收到全1的值后，会将其替换为对应资源的大小值。将原始值重新写回BAR寄存器是为了保持原有的基地址信息。
      for(int i = 0; i < 6; i++){
        uint32 old = base[4+i];

        // writing all 1's to the BAR causes it to be
        // replaced with its size.
        // 将e1000寄存器的物理地址设置给e1000网卡的BAR,使其将寄存器映射到该地址
        base[4+i] = 0xffffffff;
        __sync_synchronize();

        base[4+i] = old;
      }

      // tell the e1000 to reveal its registers at
      // physical address 0x40000000.
      base[4+0] = e1000_regs;
      // 对e1000网卡进行初始化,传递e1000寄存器的地址作为参数
      e1000_init((uint32*)e1000_regs);
    }
  }
}
