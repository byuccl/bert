#include <stdio.h>
#include "platform.h"
#include "xil_printf.h"
#include "stdint.h"
#include "bert.h"
#include "readback.h"
#include "bzlib_private.h"
#include "mydesign.h"

#define HIST_LEN 256
#define RESULT_LEN 512

#define PRINT xil_printf

unsigned int * raw =     (unsigned int *)0xA0001000;
unsigned int * huff =    (unsigned int *)0xA0004000;
unsigned int * hist =    (unsigned int *)0xA0002000;
unsigned int * results = (unsigned int *)0xA0005000;
unsigned int * huffRegs = (unsigned int *)0xA0000000;

// double the space to decrease chance of out of bounds write
uint64_t bert_results[1024*2];
uint64_t bert_hist[2048*2];
uint64_t bert_huff[512*2];
uint64_t axi_hist[2048*2];
uint64_t axi_results[1024*2];
uint64_t bert_raw[1024];

XFpga XFpgaInstance = {0U};


void extractAxi(int hlen, int rlen) {
    PRINT("Read histogram through axi...\n");
    for (int i=0; i<hlen; i++) {
        axi_hist[i]= *(hist+i);
    }

    PRINT("Read results through axi...\n");
    for (int i=1; i<=rlen; i++) {
        axi_results[i] = *(results+i);
    }
}

void extractBert() {
	// Clear just in case, remove later
	for (int i = 0; i < HIST_LEN; i++) {
		  bert_hist[i] = 0;
	}
	for (int i = 0; i < RESULT_LEN; i++) {
		  bert_results[i] = 0;
	}
	for (int i = 0; i < 1024; i++) {
			  bert_raw[i] = 0;
	}
	  bert_read(MEM_HIST,bert_hist,&XFpgaInstance);
	  bert_read(MEM_RESULT,bert_results,&XFpgaInstance);
	  bert_read(MEM_INPUT,bert_raw,&XFpgaInstance);
}


void readRegs() {
    for (int i=0;i<4;i++)
      PRINT("  Reg[%d] = %d\n", i, *(huffRegs+i));
}

void clrHuff() {
    PRINT("Resetting huffman...\n");
    *huffRegs = 1;
    *huffRegs = 0;
}

// This routine is overkill, huffman is always done when n=0 (obviously due to the time required for the PRINT)
// But, it may be done when n=0 even without the print, haven't tested that.
void waitHuff() {
    PRINT("\nWaiting for done ");
    int n=0;
    while (*(huffRegs+1) == 0) {
      n++;
      if (n % 1000 == 0)
        PRINT(".");
      if (n > 100000) {
        PRINT("\n  Failed to get done, aborting.\n");
        return;
      }
    }
    PRINT("\n  Success, received done in %d tries\n", n);
}


void compare(int hlen, int rlen)
{
  int histerr=0;
  int resulterr=0;
  int histzeros = 0;
  int resultzeros = 0;
  for(int i=0; i<hlen; i++)
    if (axi_hist[i]!=bert_hist[i])
      {
	PRINT("hist mismatch at %d axi=%llx bert=%llx\r\n",i,axi_hist[i],bert_hist[i]);
	histerr++;
      } else if (axi_hist[i] == 0) {
    	  //PRINT("axi and bert hist are 0 at %d\r\n", i);
    	  histzeros++;
      }
  for(int i=0; i<rlen; i++)
    if (axi_results[i]!=bert_results[i])
      {
	PRINT("result mismatch at %d axi=%llx bert=%llx\r\n",i,axi_results[i],bert_results[i]);
	resulterr++;
      } else if (axi_results[i] == 0) {
    	  //PRINT("axi and bert result are 0 at %d\r\n", i);
    	  resultzeros++;
      }
  if ((histerr+resulterr)==0)
    {
      PRINT("SUCCESS: axi and bert match\n");
      PRINT("SUCCESS: hist entries that are zero: %d\n", histzeros);
      PRINT("SUCCESS: result entries that are zero: %d\n", resultzeros);
    }
  else
    {
      PRINT("FAIL: %d hist errors and %d result errors\n",histerr,resulterr);
    }
}

void recompute_huffman(int *result_code) {
  UChar len[256];
  Int32 code[256];

  BZ2_hbMakeCodeLengths (len,bert_hist,256,16);
  BZ2_hbAssignCodes (code,len,1,16,256);
  // setup result_code;
  for (int i=0;i<256;i++)
    {
      // note from encoder.sv
          // Split apart the incoming character
      //assign huffmanLen = code[19:16];
      //assign huffmanCode = code[15:0];
      result_code[i]=(len[i]<<15) | code[i]; // Is this supposed to be code[i]? code[] is type Int32*
    }
}



int main()
{
    init_platform();
    readback_Init(&XFpgaInstance, U96_IDCODE);

    uint64_t new_code[512];

    PRINT("Testing huffmanreg4_bram36 with bert compressed\n");

    // On configuration, the huffman will run since it initializes to the init state
    // Check that that is the case (done should be high)
    waitHuff();


    /*
     * INITIAL READ
     */
    xil_printf("\r\nINITIAL READ\r\n");
    extractAxi(HIST_LEN, RESULT_LEN);
    extractBert();
    compare(HIST_LEN, RESULT_LEN);
    xil_printf("result[1] = %x\r\n", axi_results[1]);
    xil_printf("result[2] = %x\r\n", axi_results[2]);
    xil_printf("result[3] = %x\r\n", axi_results[3]);


    /*
     * WRITE NEW ENCODING TO HUFFMAN THROUGH BERT
     */
    xil_printf("\r\nWRITE NEW ENCODING TO HUFFMAN THROUGH BERT USING huffman.c\r\n");
    recompute_huffman(new_code);
    bert_write(MEM_HUFFMAN,new_code,&XFpgaInstance);
    clrHuff();
    waitHuff();
    extractAxi(HIST_LEN, RESULT_LEN);
    extractBert();
    compare(HIST_LEN, RESULT_LEN);
    xil_printf("result[1] = %x\r\n", axi_results[1]);
    xil_printf("result[2] = %x\r\n", axi_results[2]);
    xil_printf("result[3] = %x\r\n", axi_results[3]);

    /*
     * WRITE IDENTITY ENCODING TO HUFFMAN THROUGH BERT
     * WRITE ASCENDING RAW INPUT
     */
    xil_printf("\r\nWRITE IDENTITY ENCODING TO HUFFMAN THROUGH BERT\r\n");
    xil_printf("WRITE ASCENDING RAW INPUT\r\n");
    for (int i = 0; i < 512; i++) {
    	new_code[i] = (8 << 16) | (i % 256);
    	bert_raw[i] = (i % 256);
    	bert_raw[i+512] = (i % 256);
    }
    bert_write(MEM_HUFFMAN,new_code,&XFpgaInstance);
    bert_write(MEM_INPUT,bert_raw,&XFpgaInstance);
    clrHuff();
    waitHuff();
    extractAxi(HIST_LEN, RESULT_LEN);
    extractBert();
    compare(HIST_LEN, RESULT_LEN);
    xil_printf("result[1] = %x\r\n", axi_results[1]);
    xil_printf("result[2] = %x\r\n", axi_results[2]);
    xil_printf("result[3] = %x\r\n", axi_results[3]);


    return 0;

}