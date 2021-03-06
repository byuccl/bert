#include <stdio.h>
#include "xil_printf.h"
#include "stdint.h"
#include "bert.h"
#include "xilfpga_extension.h"
#include "bzlib_private.h"
#include "mydesign.h"


#define TIME_HUFFMAN_RECOMPUTE

#ifdef TIME_HUFFMAN_RECOMPUTE
#include "xtime_l.h"
#endif

#define HIST_LEN 256
#define RESULT_LEN 512
#define U96_IDCODE 0x04A42093


#define PRINT xil_printf

// debug -- maybe need to get AXI reads to work under -O3
#define VERBOSE_AXI 1
//#undef VERBOSE_AXI


unsigned int * raw =     (unsigned int *)0xA0001000;
unsigned int * huff =    (unsigned int *)0xA0003000;
unsigned int * hist =    (unsigned int *)0xA0002000;
unsigned int * results = (unsigned int *)0xA0004000;
unsigned int * huffRegs = (unsigned int *)0xA0000000;


// double the space to decrease chance of out of bounds write
uint64_t bert_results[1024*2];
uint64_t bert_hist[2048*2];
uint64_t bert_huff[512*2];
uint64_t axi_hist[2048*2];
uint64_t axi_results[1024*2];
uint64_t bert_raw[1024];

XFpga XFpgaInstance = {0U};


void print_time(char *label) {
#ifdef TIME_BERT
	  double time_us[4];
	  double transfuse_time_us=bert_get_last_transfuse_time_us();
	  int whole = transfuse_time_us;
	  int decimal =  (transfuse_time_us - whole) * 100;
	  time_us[0]=bert_get_last_read_time_us();
	  time_us[1]=bert_get_last_write_time_us();
	  time_us[2]=bert_get_last_logical_time_us();
	  time_us[3]=bert_get_last_physical_time_us();
	  int w[4];
	  int d[4];
	  for (int i=0;i<4;i++){
		  w[i]=time_us[i];
		  d[i] =  (time_us[i] - w[i]) * 100;
	  }
	  PRINT("TIME (us) %s: %d.%02d, %d.%02d, %d.%02d, %d.%02d, %d.%02d \r\n",
			  label, whole, decimal, w[0],d[0],w[1],d[1],w[2],d[2],w[3],d[3]);
#endif
}

void extractAxi(int hlen, int rlen) {
    PRINT("Read histogram through axi...\n");
    for (int i=0; i<hlen; i++) {
#ifdef VERBOSE_AXI
    	PRINT(".");
#endif
        axi_hist[i]= *(hist+i);
    }

    PRINT(" Read results through axi...\n");
    for (int i=1; i<=rlen; i++) {
#ifdef VERBOSE_AXI
    	PRINT(".");
#endif
        axi_results[i] = *(results+i);

    }
    PRINT("  finish extractAxi\n");
}

void extractBert(int raw, int hist, int result) {
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
	  bert_read(hist,bert_hist,&XFpgaInstance);
#ifdef TIME_BERT
	  print_time("histogram read");
#endif

	  bert_read(result,bert_results,&XFpgaInstance);
#ifdef TIME_BERT
	  print_time("result read");
#endif


	  bert_read(raw,bert_raw,&XFpgaInstance);
#ifdef TIME_BERT
	  print_time("input read");
#endif
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

void recompute_huffman(uint64_t *result_code) {
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



int main() {

#ifdef TIME_HUFFMAN_RECOMPUTE
	XTime hrstart, hrend;
	double time_us_recompute_huffman;
#endif


	PRINT("Testing huffman with bert compressed\n");

	int mem_input_slot=logical_memory_slot("design_1_i/top_0/inst/HUFFMAN/PRODUCER/inst/HUFFMAN/PRODUCER/rawTextMem",NUM_LOGICAL);
	int mem_huffman_slot=logical_memory_slot("design_1_i/top_0/inst/HUFFMAN/ENCODER/inst/HUFFMAN/ENCODER/huffmanMem",NUM_LOGICAL);
	int mem_hist_slot=logical_memory_slot("design_1_i/top_0/inst/HUFFMAN/ENCODER/HIST/histMem",NUM_LOGICAL);
	int mem_result_slot=logical_memory_slot("design_1_i/top_0/inst/HUFFMAN/CONSUMER/resultsMem",NUM_LOGICAL);


	PRINT("input=%d huffman=%d hist=%d result=%d\n",mem_input_slot,mem_huffman_slot,mem_hist_slot,mem_result_slot);
	if (mem_input_slot<0) {
		PRINT("unable to find specified input slot\n");
		return -1;
	}
	if (mem_huffman_slot<0) {
		PRINT("unable to find specified huffman encoder slot\n");
		return -1;
	}
	if (mem_hist_slot<0) {
		PRINT("unable to find specified histogram slot\n");
		return -1;
	}
	if (mem_result_slot<0) {
		PRINT("unable to find specified result slot\n");
		return -1;
	}
    readback_Init(&XFpgaInstance, U96_IDCODE);

    uint64_t new_code[512];


    // On configuration, the huffman will run since it initializes to the init state
    // Check that that is the case (done should be high)
    waitHuff();


    /*
     * INITIAL READ
     */
    xil_printf("\r\nINITIAL READ\r\n");
    extractAxi(HIST_LEN, RESULT_LEN);
    extractBert(mem_input_slot,mem_hist_slot,mem_result_slot);
    compare(HIST_LEN, RESULT_LEN);
    xil_printf("result[1] = %x\r\n", axi_results[1]);
    xil_printf("result[2] = %x\r\n", axi_results[2]);
    xil_printf("result[3] = %x\r\n", axi_results[3]);


    /*
     * WRITE NEW ENCODING TO HUFFMAN THROUGH BERT
     */
    xil_printf("\r\nWRITE NEW ENCODING TO HUFFMAN THROUGH BERT USING huffman.c\r\n");

#ifdef TIME_HUFFMAN_RECOMPUTE
	XTime_GetTime(&hrstart);
#endif
    recompute_huffman(new_code);
#ifdef TIME_HUFFMAN_RECOMPUTE
    XTime_GetTime(&hrend);
	time_us_recompute_huffman=(double) ((hrend - hrstart) * 1000000.0) / COUNTS_PER_SECOND;
	int hr_whole = time_us_recompute_huffman;
	int hr_decimal =  (time_us_recompute_huffman - hr_whole) * 100;
	PRINT("TIME (us) recompute huffman: %d.%02d\n",hr_whole,hr_decimal);
#endif


    bert_write(mem_huffman_slot,new_code,&XFpgaInstance);
#ifdef TIME_BERT
	  print_time("huffman write");
#endif

	clrHuff();
    waitHuff();
    extractAxi(HIST_LEN, RESULT_LEN);
    extractBert(mem_input_slot,mem_hist_slot,mem_result_slot);
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

    bert_write(mem_huffman_slot,new_code,&XFpgaInstance);
#ifdef TIME_BERT
	  print_time("huffman write");
#endif

    bert_write(mem_input_slot,bert_raw,&XFpgaInstance);
#ifdef TIME_BERT
	  print_time("input write");
#endif

    clrHuff();
    waitHuff();
    extractAxi(HIST_LEN, RESULT_LEN);
    extractBert(mem_input_slot,mem_hist_slot,mem_result_slot);
    compare(HIST_LEN, RESULT_LEN);
    xil_printf("result[1] = %x\r\n", axi_results[1]);
    xil_printf("result[2] = %x\r\n", axi_results[2]);
    xil_printf("result[3] = %x\r\n", axi_results[3]);

    // final run reading all memories for timing
	  bert_read(mem_hist_slot,bert_hist,&XFpgaInstance);
#ifdef TIME_BERT
	  print_time("histogram read");
#endif

	  bert_read(mem_result_slot,bert_results,&XFpgaInstance);
#ifdef TIME_BERT
	  print_time("result read");
#endif

	  bert_read(mem_input_slot,bert_raw,&XFpgaInstance);
#ifdef TIME_BERT
	  print_time("input read");
#endif
	  bert_read(mem_huffman_slot,bert_huff,&XFpgaInstance);
#ifdef TIME_BERT
	  print_time("huffman read");
#endif

  // final run writing all memories for timing
	  bert_write(mem_hist_slot,bert_hist,&XFpgaInstance);
#ifdef TIME_BERT
	  print_time("histogram write");
#endif

	  bert_write(mem_result_slot,bert_results,&XFpgaInstance);
#ifdef TIME_BERT
	  print_time("result write");
#endif

	  bert_write(mem_input_slot,bert_raw,&XFpgaInstance);
#ifdef TIME_BERT
	  print_time("input write");
#endif
	  bert_write(mem_huffman_slot,bert_huff,&XFpgaInstance);
#ifdef TIME_BERT
	  print_time("huffman write");
#endif
    
	  // printing again so easy to find...
#ifdef TIME_HUFFMAN_RECOMPUTE
	PRINT("TIME (us) recompute huffman: %d.%02d\n",hr_whole,hr_decimal);
#endif
    return 0;

}
