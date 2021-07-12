#ifndef DUMMY_XILINX_H
#define DUMMY_XILINX_H
#include <stdio.h>
#include <stdint.h>


#define XST_SUCCESS 0
#define XST_FAILURE 1
#define s32 int
#define u32 uint32_t

#define UINTPTR uintptr_t
#define XFPGA_PARTIAL_EN 0x01
#define XFPGA_ONLY_BIN_EN 0x02

#define header_LEN 196
#define footer_LEN 54
#define CFGDATA_DSTDMA_OFFSET	0x1FCU
#define BIN_READBACK_OFFSET (93 + 25) * 4
#define DATA_DMA_OFFSET 0x1FCU
//#define PAD_WORDS	25 // dummy words from pipelining

typedef struct XFpga {
  uint64_t value;
  uint32_t idcode;
  FILE* file;
} XFpga;


int XFpga_GetPlConfigDataRange(XFpga *XFpgaInstance, UINTPTR frame_data, int words, int frame_base);
int XFpga_PL_Frames_Load(XFpga *InstancePtr, UINTPTR ReadbackAddr, u32 Flags, u32 WrdCnt);
int hostwrite_Init(XFpga *XFpgaInstance, uint32_t IDCODE, FILE* out);

extern const uint32_t header[196];

extern const uint32_t footer[54];
#endif