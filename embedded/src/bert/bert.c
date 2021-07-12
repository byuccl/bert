#include "stdint.h"
#include "compressed_bert_types.h"
#include "stdio.h"
#include "stdlib.h"
#include "bert.h"


#ifdef HOST_SIDE
#include <string.h>
#include "../../../host_tools/bitstream_gen/dummy_xilinx.h"
// dummy_xilinx includes the #define's for the correct part (7series vs us+)
#define PRINT printf
#else
#include "xilfpga_extension.h"
#include "readback.h"
#include "strings.h"
//DEBUG on Embedded Platform
#include "xil_printf.h"
#define PRINT xil_printf
#endif

// for compatibility with how this was originally written
//    -- these should now be defined in mydesign.c (linked mydesign.o) -- 
#define WORDS_PER_FRAME words_per_frame
#define FRAMES_PER_BRAM frames_per_bram
#define WORDS_BETWEEN_FRAMES words_between_frames
#define WORDS_AFTER_FRAMES  words_after_frames
#define WORDS_BEFORE_FRAMES words_before_frames
#define PAD_WORDS pad_words
#define WE_BITS_PER_FRAME we_bits_per_frame
extern int words_per_frame, frames_per_bram;
extern int words_between_frames, words_after_frames, words_before_frames, pad_words;
extern int we_bits_per_frame;
extern int bitlocation[];
extern int bram_starts[];


#ifdef TIME_BERT
#include "xtime_l.h" 
XTime tstart, tend, cstart, cend;
XTime wstart;
XTime pend; // DEBUG
double time_us_read=-1;
double time_us_write=-1;
double time_us_logical=-1;
double time_us_physical=-1;
double time_us_transfuse=-1;
double bert_get_last_transfuse_time_us() {return(time_us_transfuse);}
double bert_get_last_read_time_us() {return(time_us_read);}
double bert_get_last_write_time_us() {return(time_us_write);}
double bert_get_last_logical_time_us() {return(time_us_logical);}
double bert_get_last_physical_time_us() {return(time_us_physical);}
#else
double bert_get_last_transfuse_time_us() {return(-1);}
double bert_get_last_read_time_us() {return(-1);}
double bert_get_last_write_time_us() {return(-1);}
double bert_get_last_logical_time_us() {return(-1);}
double bert_get_last_physical_time_us() {return(-1);}
#endif


// note: instead of linking against mydesign.h
extern const char * logical_names[];
extern struct compressed_logical_memory logical_memories[];
extern struct accel_memory accel_memories_logical[];
extern struct accel_memory accel_memories_physical[];

// not necessary for operation, but useful to expose failure when debugging
//  (otherwise, previous data sitting in the allocated frame might happen to be correct)
#define CLEAR_FRAME_DATA

#undef DEBUG_ACCELERATED_TO_LOGICAL
#undef DEBUG_ACCELERATED_TO_LOGICAL

#undef DEBUG_UNION
#undef DEBUG_TRANSFUSE_WE
#undef REGENERATE_UNCOMPRESSED_TO_LOGICAL
#undef REGENERATE_UNCOMPRESSED_TO_PHYSICAL

#undef DEBUG_START_ADDR
#undef DEBUG_BOUNDS

#define min(x,y) ((x<y)?x:y)
#define max(x,y) ((x<y)?y:x)


int find_offset_base(int frame, struct frame_set *the_frame_set)
{
  int num_ranges=the_frame_set->num_ranges;
  // if num_ranges > ?? maybe should do binary search
  //   ...would need to make sure they are in order to do that (not currently)
  for (int i=0;i<num_ranges;i++)
    {
      if ((the_frame_set->ranges[i].frame_base<=frame)
	  && (frame-the_frame_set->ranges[i].frame_base<the_frame_set->ranges[i].len))
	return(the_frame_set->ranges[i].offset);
    }

  PRINT("ERROR: Frame %x not found in the_frame_set\n",frame);
  for (int i=0;i<num_ranges;i++)
    {
      PRINT("\tRange base=%x len=%d\n",
	      the_frame_set->ranges[i].frame_base,
	      the_frame_set->ranges[i].len);
    }

  return -1;
}

int find_offset_from_base(int frame, struct frame_set *the_frame_set)
{
  int num_ranges=the_frame_set->num_ranges;
  // if num_ranges > ?? maybe should do binary search
  //   ...would need to make sure they are in order to do that (not currently)
  for (int i=0;i<num_ranges;i++)
    {
      if ((the_frame_set->ranges[i].frame_base<=frame)
	  && (frame-the_frame_set->ranges[i].frame_base<the_frame_set->ranges[i].len))
	return(frame-the_frame_set->ranges[i].frame_base);
    }

  fprintf(stderr,"ERROR: Frame %x not found in the_frame_set\n",frame);
  for (int i=0;i<num_ranges;i++)
    {
      fprintf(stderr,"\tRange base=%x len=%d\n",
	      the_frame_set->ranges[i].frame_base,
	      the_frame_set->ranges[i].len);
    }

  return -1;
}

int which_frame(int which_mem, int frame_base)
{
  for (int i=0;i<logical_memories[which_mem].nframe_ranges;i++)
    if ((logical_memories[which_mem].frame_ranges[i].first_frame>=frame_base)
    		&& (frame_base<(logical_memories[which_mem].frame_ranges[i].first_frame+
    				        logical_memories[which_mem].frame_ranges[i].len))
							)
      return(i);
  printf("bert.which_frame: frame %x not found in logical memory %d\n",frame_base,which_mem);
  return -1;
  
}
struct frame_set *bert_union(int num, struct bert_meminfo *info)
{

  int max_frame_ranges=0;
  for(int i=0;i<num;i++)
    max_frame_ranges+=logical_memories[info[i].logical_mem].nframe_ranges;

  int current_frame_ranges=0;

  struct frame_range_offset *ranges=(struct frame_range_offset *)malloc(sizeof(struct frame_range_offset)*max_frame_ranges);
  if (ranges == NULL)
    return NULL;
  for (int rw=0;rw<2;rw++)
    {
      for (int i=0;i<num;i++)
	{
	  // process writes first so that they end up first in the frame set
	  if (((rw==0) && ((info[i].operation==BERT_OPERATION_WRITE)
			  ||(info[i].operation==BERT_OPERATION_ACCELERATED_WRITE)))
	       ||((rw==1) && ((info[i].operation==BERT_OPERATION_READ)
			     ||(info[i].operation==BERT_OPERATION_ACCELERATED_READ))))
	    {

	      
	      int slot_base=0;
	      int next_addr=info[i].start_addr;
	      int remaining_length=(info[i].data_length); // note -- in slots not repeats
	      for (int r=0;r<logical_memories[info[i].logical_mem].replicas;r++)
		{
		  if ((rw==1) && (r>1))
		    break; // only need to process one read -- use 0
		  
		  for (int s=0;s<logical_memories[info[i].logical_mem].num_segments[r];s++)
		    {
		      int needs_read=0; // is there something that would force a read before write?
		      //  (a) RAMB18 with live partner (not being written...but not opt. that, yet)
		      //  (b) write to a few addresses not on frame boundary
		      struct segment_repeats current_segment=logical_memories[info[i].logical_mem].repeats[r][s];
		      int len;
		      if ((slot_base+current_segment.slots_in_repeat*current_segment.num_repeats)<info[i].start_addr)
			{
			  len=0;
			  // skip this segment (code at bottom to advance pointers)
			}
		      else
			{

			  int start_offset=next_addr%current_segment.slots_in_repeat;
			  if ((start_offset)!=0)
			    needs_read=1; // write may start in middle of a repeat;
			  // need to read stuff before it.
		      
			  int mem_ranges=current_segment.unique_frames_in_repeat;
		      
			  // len applies to all ranges
			  int slot_len=min(remaining_length+start_offset,(current_segment.num_repeats*current_segment.slots_in_repeat-(next_addr-slot_base)));
			  // rounding up in case partial
			  len=(slot_len+current_segment.slots_in_repeat-1)/current_segment.slots_in_repeat;

			  if (len*current_segment.slots_in_repeat!=slot_len)
			    needs_read=1; // read may end in the middle of repeat;
			  // need to read stuff after it.

			  for (int j=0;j<mem_ranges;j++) 
			    {
			      int current_frame=current_segment.unique_frames[j];
			      int which_frame_in_logical_memory=which_frame(info[i].logical_mem,
									current_frame);
			      if (which_frame_in_logical_memory == -1) {
#ifdef DEBUG_UNION
			      	  printf("which_frame returns -1 for %x for mem %d range %d\n",current_frame,info[i].logical_mem,j);
				      for (int t=0;t<logical_memories[info[i].logical_mem].nframe_ranges;t++)
		    	          printf("  %x",logical_memories[info[i].logical_mem].frame_ranges[t].first_frame);
			      	  printf("\n");
		#endif
				        return NULL;
			      }
			      int webits=logical_memories[info[i].logical_mem].frame_ranges[which_frame_in_logical_memory].we_bits;
			      int has_live_ramb18_partner=logical_memories[info[i].logical_mem].frame_ranges[which_frame_in_logical_memory].has_live_ramb18_partner;
			      if (has_live_ramb18_partner)
				needs_read=1; // conservative -- if partner is in this transfuse operation, may not need...but that may require much more complicated calculation for general case
			  
			      int frame_base=current_segment.unique_frames[j]+(next_addr-slot_base)/current_segment.slots_in_repeat;
#ifdef DEBUG_UNION
			      printf("current_segment.unique_frame[%d]=%x next_addr=%x slot_base=%x\n",
				     j,current_segment.unique_frames[j],
				     next_addr,
				     slot_base);
			      printf("bert_union: rw=%d info=%d mem=%d s=%d frame_range=%d frame_range_in_logical_memory=%d,base=%x webits=%x\n",
				     rw,i,info[i].logical_mem,s,j,which_frame_in_logical_memory,
				     frame_base,webits);
#endif

			      int found=0;
			      for (int k=0;k<current_frame_ranges;k++)
				{
				  // TODO: not handling case where a new range
				  //   would merge two existing ranges into a single range
				  //   spanning all 3 ranges. (fills in a gap)
				  if ((ranges[k].frame_base<=frame_base) &&
				      ((ranges[k].frame_base+ranges[k].len) >= (frame_base+len))
				      )
				    {
				      // this is already in list
				      found=1;
#ifdef DEBUG_UNION
				      printf("bert_union: %x len=%d already covers %x %d\n",
					     ranges[k].frame_base,ranges[k].len,frame_base,len);
#endif				  
				      if (rw==0) // processing writes
					{
					  ranges[k].we_bits|=logical_memories[info[i].logical_mem].frame_ranges[which_frame_in_logical_memory].we_bits;
#ifdef DEBUG_UNION
					  printf("bert_union: frame %x webits now %x\n",
						 frame_base,ranges[k].we_bits);
#endif
					  if (needs_read) ranges[k].has_read=1;
					}
				      else
					ranges[k].has_read=1;
				      break;
				    }
				  else if ((frame_base<=ranges[k].frame_base) &&
					   ((frame_base+len) >= (ranges[k].frame_base+ranges[k].len)))
				    {
				      // this is larger, redefine entry
#ifdef DEBUG_UNION
				      printf("bert_union: %x len=%d smaller than %x %d ... expanding\n",
					     ranges[k].frame_base,ranges[k].len,frame_base,len);
#endif				  
				      found=1;
				      ranges[k].frame_base=frame_base;
				      ranges[k].len=len;
				      if (rw==0) // processing writes
					{
					  ranges[k].we_bits|=logical_memories[info[i].logical_mem].frame_ranges[which_frame_in_logical_memory].we_bits;
					  if (needs_read) ranges[k].has_read=1;
					}
				      else
					ranges[k].has_read=1;
				  
				      break;
				    }
				  else if (((frame_base<ranges[k].frame_base) &&
					    ((frame_base+len) <(ranges[k].frame_base+ranges[k].len)) &&
					    ((frame_base+len)>ranges[k].frame_base))
					   ||
					   ((ranges[k].frame_base<frame_base) &&
					    ((ranges[k].frame_base+ranges[k].len)<(frame_base+len)) &&
					    ((ranges[k].frame_base+ranges[k].len)>frame_base))
					   )
				    {
				      // they intersect, neither subsumes
				      found=1;

				      if (rw==0) // processing writes
					{
					  ranges[k].we_bits|=logical_memories[info[i].logical_mem].frame_ranges[which_frame_in_logical_memory].we_bits;
					  if (needs_read) ranges[k].has_read=1;
					}
				      else
					ranges[k].has_read=1;

				      int new_base=min(frame_base,ranges[k].frame_base);
				      int new_len=max(frame_base+len,ranges[k].frame_base+ranges[k].len)-new_base;
#ifdef DEBUG_UNION
				      printf("bert_union: %x len=%d overlaps  %x %d ... taking union %x len=%d\n",
					     ranges[k].frame_base,ranges[k].len,frame_base,len,
					     new_base,new_len);
#endif				  				  				  
				      ranges[k].frame_base=new_base;
				      ranges[k].len=new_len;
				    }
				}
			      if (found==0)
				{
				  if (rw==0) // writes
				    {
				      //ranges[current_frame_ranges].has_write=1;
				      ranges[current_frame_ranges].we_bits=logical_memories[info[i].logical_mem].frame_ranges[which_frame_in_logical_memory].we_bits;
				      ranges[current_frame_ranges].has_read=needs_read;
				    }
				  else
				    {
				      //ranges[current_frame_ranges].has_write=0;
				      ranges[current_frame_ranges].we_bits=0;
				      ranges[current_frame_ranges].has_read=1;
				    }
#ifdef DEBUG_UNION
				  printf("bert_union: not found, adding  %x %d\n",
					 frame_base,len);
#endif				  				  			      
				  ranges[current_frame_ranges].frame_base=frame_base;
				  ranges[current_frame_ranges].len=len;
				  current_frame_ranges++;
				}
			    } // over mem ranges
			  // if we're in this block of code, then
			  //  start_addr is less than the following
			  //   ...which is the slot after this segment
			  // so, set it to that so we process the rest of the addresses?
			  // new 8/10/2020
			  next_addr=slot_base+current_segment.slots_in_repeat*current_segment.num_repeats;
			} // skip over this segment?
		      remaining_length-=len*current_segment.slots_in_repeat;
		      slot_base+=current_segment.slots_in_repeat*current_segment.num_repeats;
		    } // loop over segments
		} // loop over replicas
	    } // if read or write
	} // all memories
    } // rw
									
  struct frame_set *result=(struct frame_set *)malloc(sizeof(struct frame_set));
  if (result == NULL)
    return NULL;
  result->num_ranges=current_frame_ranges;
  result->ranges=ranges;
  return(result);
  

}
void print_frame_set (struct frame_set *the_frame_set)
{
  int ranges=the_frame_set->num_ranges;
  for (int i=0;i<ranges;i++)
    {
      printf("FRAME_SET: 0x%x len=%d we_bits=%x has_read=%d offset=%d\n",
	     the_frame_set->ranges[i].frame_base,
	     the_frame_set->ranges[i].len,
	     the_frame_set->ranges[i].we_bits,
	     the_frame_set->ranges[i].has_read,
	     the_frame_set->ranges[i].offset);
    }
}


int bert_to_logical(int logical,uint32_t *frame_data,uint64_t *logical_data,
			       int start_addr, int data_length, struct frame_set *the_frame_set)
{

  
  if (logical_memories[logical].nframe_ranges<1)
    return BST_SUCCESS; // nothing to translate


  int wordlen=logical_memories[logical].wordlen;
  int b64_per_word=(wordlen+63)/64;
  int words=logical_memories[logical].words;
  words=min(words,start_addr+data_length);

  int num_segments=logical_memories[logical].num_segments[0];

  int loc=0;
  int b=0;



    
  for (int seg=0;seg<num_segments;seg++)
    {
      //fprintf(stdout,"DEBUG: to_logical starting segment=%d with loc=%d\n",seg,loc);
      struct segment_repeats ms=logical_memories[logical].repeats[0][seg];
      if ((loc+ms.slots_in_repeat*ms.num_repeats)<start_addr)
	{
	  loc+=ms.slots_in_repeat*ms.num_repeats;
	}
      else
	{
	  int num_repeats=ms.num_repeats;
	  int num_frames=ms.num_frames;
	  int repeat_start=0;
	  int *frame_seq_indicies=(int *)malloc(sizeof(int)*ms.num_frames);
	  int *frame_offsets=(int *)malloc(sizeof(int)*ms.num_frames);
	  if (start_addr>loc)
	    {
	      repeat_start=(start_addr-loc)/ms.slots_in_repeat;
	    }
	  loc+=repeat_start*ms.slots_in_repeat; // fast forward to correct repeat
	  for (int i=0;i<ms.num_frames;i++)
	    {
	      frame_seq_indicies[i]=repeat_start%ms.frame_seq[i].sequence_length;
	      frame_offsets[i]=repeat_start/ms.frame_seq[i].sequence_length;
	    }
	  for (int r=repeat_start;r<num_repeats;r++)
	    {
	      
	      //fprintf(stdout,"DEBUG:   to_logical starting segment=%d r=%d sequence_length=%d num_frames=%d with loc=%d\n",seg,r,ms.frame_seq[0].sequence_length,num_frames,loc);
	      for (int s=0;s<ms.frame_seq[0].sequence_length;s++)
		{
		  for (int f=0;f<num_frames;f++)
		    {
		      struct frame_sequence fseq=ms.frame_seq[f];
		      int frame=fseq.frame_address;
		      int nbits=fseq.frame_bits[frame_seq_indicies[f]].num_bits;
		      if (frame>0)
			{
			  //fprintf(stderr,"DEBUG: frame=%llx f=%d frame_offset=%d sequence_length=%d\n",
			  //frame,f,frame_offsets[f],
			  //ms.frame_seq[f].sequence_length); // DEBUG
			  int offset_base=find_offset_base(frame+frame_offsets[f],
							   the_frame_set);
#ifdef DEBUG_BOUNDS			  
			  if (offset_base==-1)
			    {
			      printf("frame=%x f=%d offset=%x\n",frame,f,frame_offsets[f]);
			      printf("r=%d s=%d f=%d loc=%x b=%d\n",r,s,f,loc,b);
			      exit(3); // DEBUG
			    }
#endif			  
			  if (offset_base == -1)
			    return BST_OFFSET_NOT_FOUND;
			  int offset_from_base=find_offset_from_base(frame+frame_offsets[f],
								     the_frame_set);
			  if (offset_from_base == -1)
			    return BST_OFFSET_NOT_FOUND;
			  int frame_offset=offset_base+(offset_from_base)*WORDS_PER_FRAME;
			  int16_t *bit_location=fseq.frame_bits[frame_seq_indicies[f]].bit_loc;
			  for (int fb=0;fb<nbits;fb++)
			    {
			      // don't go through the stuff that isn't in range
			      if (loc>=(start_addr+data_length))
				return BST_SUCCESS;
			      if ((loc>=start_addr) & (loc<(start_addr+data_length)))
				{
#ifdef REGENERATE_UNCOMPRESSED_TO_LOGICAL
				   printf("    {0x%08x, %d},\n",(frame+r),bit_location[fb]);

				   // regenerate and idenitfy bits
				   // printf("    {0x%08x, %d}, //loc=%x, b=%d\n",(frame+r),bit_location[fb],
				   //loc,b);
#endif
				  
				  int frame_word=bit_location[fb]/32;
				  int bit_in_word=bit_location[fb]%32;
				  int bit_in_logical=b%64;
				  int word_logical=b/64;
				  if (((frame_data[frame_offset+frame_word]>>bit_in_word) & 0x01)==1)
				
				    logical_data[(loc-start_addr)*b64_per_word+word_logical]|=((uint64_t) 1)<<bit_in_logical;
				  else
				    {
				      uint64_t bit_mask=
					(((uint64_t)(0xFFFFFFFF)) | (((uint64_t)(0xFFFFFFFF))<<32))
					-(((uint64_t)1)<<bit_in_logical);
				      logical_data[(loc-start_addr)*b64_per_word+word_logical]&=bit_mask;
				    }
				  if (b<(wordlen-1))
				    b++;
				  else
				    {
				      b=0;
				      loc++;
				    }
				}
			    } // bits in frame
			} // frame not greater than 0
		      else // negative frame
			{
			  // advance by nbits
			  b=(b+nbits);
			  while (b>wordlen) {
			    b=b-wordlen;
			    loc++;
			  }
			}
		      // reset sequence counter as needed
		      frame_seq_indicies[f]++;
		      if (frame_seq_indicies[f]==ms.frame_seq[f].sequence_length)
			{
			  frame_seq_indicies[f]=0;
			  frame_offsets[f]++;
			}
		    } // f, frames
		} // s, sequence (for frame 0)
	    } // r, repeats
	  free(frame_seq_indicies);
	  free(frame_offsets);
	} // skip segements

    } // for each segment (seg)
  return BST_SUCCESS;
}

int bert_accelerated_to_logical(int logical,uint32_t *frame_data,uint64_t *logical_data,
				int start_addr, int data_length, struct frame_set *the_frame_set)
{

  
  if (logical_memories[logical].nframe_ranges<1)
    return BST_SUCCESS; // nothing to translate

  int lookup_quanta=accel_memories_logical[logical].lookup_quanta;
  int lookup_tables=accel_memories_logical[logical].lookup_tables;
  int u64_per_lookup=accel_memories_logical[logical].u64_per_lookup;
  int tabsize=(u64_per_lookup*(1<<lookup_quanta));
  uint64_t **translation_tables=accel_memories_logical[logical].trans_tables;

  int wordlen=logical_memories[logical].wordlen;
  int b64_per_word=(wordlen+63)/64;
  int words=logical_memories[logical].words;
  words=min(words,start_addr+data_length);

  int num_segments=logical_memories[logical].num_segments[0];


  if (logical_memories[logical].nframe_ranges!=1)
    {
      printf("bert_accelerated_to_logical should only have one frame range\n");
      return BST_EXCESS_FRAME_RANGES;
    }
  if (logical_memories[logical].num_segments[0]!=1)
    {
      printf("bert_accelerated_to_logical should only have one segment\n");
      return BST_EXCESS_SEGMENTS;
    }

  struct segment_repeats ms=logical_memories[logical].repeats[0][0];
  if (ms.num_frames!=1)
    {
      printf("bert_accelerated_to_logical segment should only have one frame\n");
      return BST_EXCESS_FRAMES;
    }

  //int slots_per_u64=64/wordlen; // will be 0 for wordlen between 65 and 72
  int slots_per_u64=max(min(64/wordlen,ms.slots_in_repeat),1); // REVIEW
  uint64_t word_mask = (((uint64_t)1)<<wordlen)-1; // only used with wordlen<=32


  if (((wordlen<=64) && (ms.slots_in_repeat!=u64_per_lookup*slots_per_u64))
      || ((wordlen>64) && (ms.slots_in_repeat!=u64_per_lookup)))
    {
      printf("bert_accelerated_to_logical mismatch between slots_in_repeat %d and u64_per_lookup %d, slots_per_u64=%d\n",
	     ms.slots_in_repeat,u64_per_lookup,slots_per_u64);
      return BST_SLOT_MISMATCH;
      
    }

  uint64_t *new_logical_data=(uint64_t *)malloc(sizeof(uint64_t)*(u64_per_lookup));
  if (new_logical_data == NULL)
    return BST_NULL_PTR;
  int loc=0;


  int repeat_start=0;
  if (start_addr>loc)
    repeat_start=(start_addr-loc)/ms.slots_in_repeat;
  loc+=repeat_start*ms.slots_in_repeat; // fast forward to correct repeat

  // only one frame
  int num_repeats=ms.num_repeats;
  struct frame_bits fbits=ms.frame_seq[0].frame_bits[0];
  int frame=ms.frame_seq[0].frame_address;
  int nbits=ms.frame_seq[0].sequence_length*ms.frame_seq[0].frame_bits[0].num_bits;
  int offset_base=find_offset_base(frame+repeat_start,
				   the_frame_set);
  if (offset_base == -1)
    return BST_OFFSET_NOT_FOUND;
  int offset_from_base=find_offset_from_base(frame+repeat_start,
					     the_frame_set);
  if (offset_from_base == -1)
    return BST_OFFSET_NOT_FOUND;
  uint64_t mask_quanta=(((uint64_t) 1)<<lookup_quanta)-1;
  int offset_in_frame=logical_memories[logical].repeats[0][0].frame_seq[0].frame_bits[0].bit_loc[0];

  
  for (int r=repeat_start;r<num_repeats;r++)
    {
      int frame_offset=offset_base+(offset_from_base+(r-repeat_start))*WORDS_PER_FRAME;

      for (int j=0;j<(u64_per_lookup);j++)
	new_logical_data[j]=0;
      
      for(int i=0;i<lookup_tables;i++)
	{
	  int frame_word=frame_offset+(offset_in_frame+i*lookup_quanta)/32;
	  int bit_offset=(offset_in_frame+i*lookup_quanta)%32;
	  int key=(frame_data[frame_word]>>bit_offset)&mask_quanta;
	  // 12/20/20 -- looks like this is necessary now
	  //   might could be tightened by moving some of these computations
	  //   out of loop.
	  if ((bit_offset+lookup_quanta)>32)
	    key|=(frame_data[frame_word+1]<<(bit_offset+lookup_quanta))&mask_quanta;
	  // DEBUG -- all keys coming up zeros!
#ifdef DEBUG_ACCELERATED_TO_LOGICAL
	  printf("r=%d table=%d frame_word=%d bit_offset=%d key=%x\n",
		 r,i,frame_word,bit_offset,key);
	  for (int j=0;j<u64_per_lookup;j++)
	    {
	      printf(" %016llx new_data now %016llx",translation_tables[i][key*u64_per_lookup+j],new_logical_data[j]);
	    }
	  printf("\n");
#endif	  
	  //	  for (int j=0;j<u64_per_lookup;j++)
	  // new_logical_data[j]|=translation_tables[i][key*u64_per_lookup+j];
	  // unroll loop
	  switch(u64_per_lookup)
	    {
	    case 1:
	      new_logical_data[0]|=translation_tables[i][key*u64_per_lookup+0];
	      break;
	    case 2:
	      new_logical_data[0]|=translation_tables[i][key*u64_per_lookup+0];
	      new_logical_data[1]|=translation_tables[i][key*u64_per_lookup+1];
	      break;
	    case 3:
	      new_logical_data[0]|=translation_tables[i][key*u64_per_lookup+0];
	      new_logical_data[1]|=translation_tables[i][key*u64_per_lookup+1];
	      new_logical_data[2]|=translation_tables[i][key*u64_per_lookup+2];
	      break;
	    default:
	      printf("accelerated_to_logical: Expect u64_per_lookup to be 1--3, got %d\n",
		     u64_per_lookup);
	      return BST_BAD_U64_COUNT;
	    }
	}
      
      for (int j=0;j<(u64_per_lookup);j++)
	{
	  //0 or 1 ... things are in the correct place
	  if (slots_per_u64>1)
	    {
	      uint64_t packed_data=new_logical_data[j];
	      for (int k=0;k<slots_per_u64;k++)
		{
		  logical_data[loc+j*slots_per_u64+k]=
		    ((packed_data>>(k*wordlen)) & word_mask);
		}
	    }
	  else //0 or 1 ... things are in the correct place
	    {
	      logical_data[loc+j]=new_logical_data[j];
	    }
	}


      
#ifdef DEBUG_ACCELERATED_TO_LOGICAL
      printf("loc %d: ",loc);
      for (int j=0;j<(u64_per_lookup);j++)
	printf(" %016llx",new_logical_data[j]);
      printf("\n");

#endif	  
      
      loc+=ms.slots_in_repeat;
    }

  return BST_SUCCESS;

}



int bert_to_physical(int logical,uint32_t *frame_data,uint64_t *logical_data,
				int start_addr, int data_length, struct frame_set *the_frame_set)
{


  if (logical_memories[logical].nframe_ranges<1)
    return BST_SUCCESS; // nothing to translate



  int wordlen=logical_memories[logical].wordlen;
  int b64_per_word=(wordlen+63)/64;  
  int words=logical_memories[logical].words;
  words=min(words,start_addr+data_length);

  int loc=0;
  int b=0;

  for (int replica=0;replica<logical_memories[logical].replicas;replica++)
    {

      int num_segments=logical_memories[logical].num_segments[replica];

      for (int seg=0;seg<num_segments;seg++)
	{
	  //fprintf(stdout,"DEBUG: to_physical starting segment=%d with loc=%d\n",seg,loc);
	  struct segment_repeats ms=logical_memories[logical].repeats[replica][seg];
	  if ((loc+ms.slots_in_repeat*ms.num_repeats)<start_addr)
	    {
	      loc+=ms.slots_in_repeat*ms.num_repeats;
	    }
	  else
	    {
	      int num_repeats=ms.num_repeats;
	      int num_frames=ms.num_frames;
	      int repeat_start=0;
	      int *frame_seq_indicies=(int *)malloc(sizeof(int)*ms.num_frames);
	      int *frame_offsets=(int *)malloc(sizeof(int)*ms.num_frames);

	      if (start_addr>loc) {
		repeat_start=(start_addr-loc)/ms.slots_in_repeat;
#ifdef DEBUG_START_ADDR
		PRINT("start_addr=%x dat_length=%d loc=%x b=%x\n",start_addr,data_length,loc,b);
#endif
	      }
	      loc+=repeat_start*ms.slots_in_repeat; // fast forward to correct repeat
#ifdef DEBUG_START_ADDR
	      PRINT("after fast forward: start_addr=%x repeat_start=%d loc=%x b=%x num_repeats=% num_frames=%d\n",start_addr,repeat_start,loc,b,num_repeats,num_frames);
#endif
	      for (int i=0;i<ms.num_frames;i++)
		{
		  frame_seq_indicies[i]=repeat_start%ms.frame_seq[i].sequence_length;
		  frame_offsets[i]=repeat_start/ms.frame_seq[i].sequence_length;
		}

	      for (int r=repeat_start;r<num_repeats;r++)
		{
		  for (int s=0;s<ms.frame_seq[0].sequence_length;s++)
		    {
		      for (int f=0;f<num_frames;f++)
			{
			  struct frame_sequence fseq=ms.frame_seq[f];

			  int frame=fseq.frame_address;
			  int nbits=fseq.frame_bits[frame_seq_indicies[f]].num_bits;
			  if (frame>0)
			    {
			  
			      // TODO: may not work if unioned with something...
			      int offset_base=find_offset_base(frame+frame_offsets[f],
							       the_frame_set);
			      if (offset_base == -1)
				return BST_OFFSET_NOT_FOUND;
			      int offset_from_base=find_offset_from_base(frame+frame_offsets[f],
									 the_frame_set);
			      int frame_offset=offset_base+(offset_from_base)*WORDS_PER_FRAME;		      
			      //  to here
			      int16_t *bit_location=fseq.frame_bits[frame_seq_indicies[f]].bit_loc;
			  
			      for (int fb=0;fb<nbits;fb++)
				{
				  // don't go through the stuff that isn't in range
				  if (loc>=(start_addr+data_length))
				    {
#ifdef DEBUG_START_ADDR
				      PRINT("returning success: start_addr=%x data_length=%d loc=%x b=%x\n",start_addr,data_length,loc,b);
#endif
				      return BST_SUCCESS;
				    }
				  if ((loc>=start_addr) && (loc<(start_addr+data_length)))
				    {		      
				      // only needs to be in one, but this gets called first...
#ifdef REGENERATE_UNCOMPRESSED_TO_PHYSICAL
				      PRINT("\t{0x%x,%d},\n",(frame+r),bit_location[fb]);
				      //PRINT("frame_offset=%x frame_offsets=%x repeat=%x loc=%d bit=%d f=%d\n",frame_offset,
				      //frame_offsets[f],r,loc,b,f); // DEBUG
#endif
				  
				    int frame_word=bit_location[fb]/32;
				    int bit_in_word=bit_location[fb]%32;
				    
				    int bit_in_logical=b%64;
				    int word_logical=b/64;
				    int bit=(logical_data[(loc-start_addr)*b64_per_word+word_logical]>>bit_in_logical) & 0x01;
			      
				    uint32_t bit_mask=0xFFFFFFFF-(1<<bit_in_word);
				    if (bit==0)
				      frame_data[frame_offset+frame_word]=
					frame_data[frame_offset+frame_word]&bit_mask;
				    else
				      frame_data[frame_offset+frame_word]=
					frame_data[frame_offset+frame_word] | (1<<bit_in_word);
				    } // if bit in range
				  if (b<(wordlen-1))
				    b++;
				  else
				    {
				      b=0;
				      loc++;
				    }
				} // fb, nbits
			    }
			  else // negative frame
			    {
			      // advance by nbits
			      b=(b+nbits);
			      while (b>wordlen) {
				b=b-wordlen;
				loc++;
			      }
			    }
			  // reset sequence counter as needed
			  frame_seq_indicies[f]++;
			  if (frame_seq_indicies[f]==ms.frame_seq[f].sequence_length)
			    {
			      frame_seq_indicies[f]=0;
			      frame_offsets[f]++;
			    }
			} // f , num_frames
		    } // s, frame sequences in first frame
		} // r, num_repeates
	      free(frame_seq_indicies);
	      free(frame_offsets);
	    } // skip ahead 
	} // for each segment
    } // replicas
  return BST_SUCCESS;
}

int bert_accelerated_to_physical(int logical,uint32_t *frame_data,uint64_t *logical_data,
				 int start_addr, int data_length, struct frame_set *the_frame_set)
{


  if (logical_memories[logical].nframe_ranges<1)
    return BST_SUCCESS; // nothing to translate

  int lookup_quanta=accel_memories_physical[logical].lookup_quanta;
  int lookup_tables=accel_memories_physical[logical].lookup_tables;
  int u64_per_lookup=accel_memories_physical[logical].u64_per_lookup;
  int tabsize=(u64_per_lookup*(1<<lookup_quanta));
  uint64_t **translation_tables=accel_memories_physical[logical].trans_tables;

  int bit_high=accel_memories_physical[logical].bit_high;
  int bit_low=accel_memories_physical[logical].bit_low;
  
  int wordlen=logical_memories[logical].wordlen;
  int words=logical_memories[logical].words;
  uint64_t mask_quanta=(((uint64_t) 1)<<lookup_quanta)-1;
  int offset=the_frame_set->ranges[0].offset;
  int offset_in_frame=logical_memories[logical].repeats[0][0].frame_seq[0].frame_bits[0].bit_loc[0];
  int frame_word_offset=offset_in_frame/32;
  int slots_per_frame=logical_memories[logical].repeats[0][0].slots_in_repeat;
  uint64_t *new_frame_data=(uint64_t *)malloc(sizeof(uint64_t)*(u64_per_lookup));
  if (new_frame_data == NULL)
    return BST_NULL_PTR;
#ifdef DEBUG
  printf("bert_accelerated_to_physical starting tables=%d, tabsize=%d\n",lookup_tables,tabsize);
  printf("spot check trans_table inside bert_accelerated_to_physical\n");
  for (int i=0;i<lookup_tables;i++)
    for (int j=1;j<3;j++)
      {
	printf("table %d entry %x: ",i,j);
	for (int k=0;k<u64_per_lookup;k++)
	  printf("%llx ",translation_tables[i][j*u64_per_lookup+k]);
	printf("\n");
      }  
#endif  

  // mask for bits that should be written for this memory
  //  useful for ramb18 case where u64s may overlap between intended memory bits
  //  and the partner memory bits
  uint32_t *write_mask=(uint32_t *)malloc(sizeof(uint32_t)*(2*u64_per_lookup));
  for (int i=0;i<2*u64_per_lookup;i++)
    {
      write_mask[i]=0;
      for (int j=0;j<32;j++)
	{
	  int current_bit=(frame_word_offset+i)*32+j;
	  if ((current_bit>=bit_low) && (current_bit<=bit_high))
	    write_mask[i]|=(0x01<<j);
	}
    }
  
  for (int i=0;i<the_frame_set->ranges[0].len;i++)
    {
      int addr=start_addr+i*slots_per_frame;
      for (int j=0;j<(u64_per_lookup);j++)
	new_frame_data[j]=0;
#ifdef DEBUG
      printf("Looking up data in tables for frame %d\n",i);
#endif  
      for (int j=0;j<lookup_tables;j++)
	{
	  int addr_offset=(j*lookup_quanta)/wordlen;
	  int bit_offset=(j*lookup_quanta)%wordlen;
	  int key=((logical_data[addr+addr_offset]>>bit_offset) & mask_quanta);
#ifdef DEBUG_ADDRESS
	  printf("frame %d lookup %d key %x tt=%llx\n",i,j,key,(uint64_t)translation_tables);
#endif  	  
	  for (int k=0;k<u64_per_lookup;k++)
	    {
#ifdef DEBUG_ADDRESS
	      printf("frame %d lookup %d key %x table %d entry %d address %llx\n",i,j,key,k,
		     key*u64_per_lookup+k,
		     (uint64_t)(&translation_tables[j]));
#endif  	  
#ifdef DEBUG
	      printf("frame %d lookup %d key %x table %d entry %d value %llx\n",i,j,key,k,
		     key*u64_per_lookup+k,
		     translation_tables[j][key*u64_per_lookup+k]);
#endif  	  
	      new_frame_data[k]=new_frame_data[k]|=translation_tables[j][key*u64_per_lookup+k];
#ifdef DEBUG_ADDRESS	      
	      printf("\tnew_frame_data[%d]=%llx at %llx\n",k,new_frame_data[k],
		     (uint64_t)&new_frame_data[k]);
#endif	      
#ifdef DEBUG	      
	      printf("\tnew_frame_data[%d]=%llx\n",k,new_frame_data[k]);
#endif	      
	    }
	}
      // int frame_bit_offset=offset_in_frame%32; -- assume already taken care of
      // may not be zero -- assume table was built to be aligned
      // TODO ...could record that data and check here to make sure consistent
#ifdef DEBUG
      printf("putting data into frame %d\n",i);
#endif  
      
      for (int j=0;j<u64_per_lookup;j++)
	{
#ifdef DEBUG
	  printf("frame_data[%d]\n",(offset+i*WORDS_PER_FRAME+frame_word_offset+j*2));
#endif	  
	      frame_data[offset+i*WORDS_PER_FRAME+frame_word_offset+j*2]=
		((new_frame_data[j]&((uint64_t)0xFFFFFFFF))&write_mask[j*2])
		|((frame_data[offset+i*WORDS_PER_FRAME+frame_word_offset+j*2])&~write_mask[j*2]);
	      frame_data[offset+i*WORDS_PER_FRAME+frame_word_offset+j*2+1]=
		((new_frame_data[j]>>32)&((uint64_t)0xFFFFFFFF)&write_mask[j*2+1])
		|((frame_data[offset+i*WORDS_PER_FRAME+frame_word_offset+j*2+1])&~write_mask[j*2+1]);
	}
    }

#ifdef DEBUG
  printf("bert_accelerated_to_physical returning...\n");
#endif  
  return BST_SUCCESS;
} 


int  bert_read(int logicalm, uint64_t *data, XFpga* XFpgaInstance)
{
  struct bert_meminfo *meminfo=(struct bert_meminfo *)malloc(sizeof(struct bert_meminfo));
  if (meminfo == NULL)
    return BST_NULL_PTR;
  meminfo->logical_mem=logicalm;
  meminfo->operation=BERT_OPERATION_READ;
  meminfo->data=data;
  meminfo->start_addr=0;
  meminfo->data_length=logical_memories[logicalm].words;

  int res=bert_transfuse(1,meminfo,XFpgaInstance);
  free(meminfo);
  return(res);
}



int  bert_write(int logicalm, uint64_t *data, XFpga* XFpgaInstance)
{
  struct bert_meminfo *meminfo=(struct bert_meminfo *)malloc(sizeof(struct bert_meminfo));
  if (meminfo == NULL)
    return BST_NULL_PTR;
  meminfo->logical_mem=logicalm;
  meminfo->operation=BERT_OPERATION_WRITE;
  meminfo->data=data;
  meminfo->start_addr=0;
  meminfo->data_length=logical_memories[logicalm].words;

  int res=bert_transfuse(1,meminfo,XFpgaInstance);
  free(meminfo);
  return(res);
}  



// was: int  bert_transfuse_we(int num, struct bert_meminfo *meminfo, XFpga* XFpgaInstance)
int  bert_transfuse(int num, struct bert_meminfo *meminfo, XFpga* XFpgaInstance)
{

#ifdef TIME_BERT
	time_us_transfuse=-2; // for error exits
	XTime_GetTime(&tstart);
#endif
  s32 Status;

  struct frame_set *the_frame_set=bert_union(num,meminfo);
  if (the_frame_set == NULL)
    return BST_UNION_FAILURE;

  //gives a composite frame set, but still needs to calculate the offsets
  int offset=WORDS_PER_FRAME+PAD_WORDS+DATA_DMA_OFFSET/4;
  for (int i=0;i<the_frame_set->num_ranges;i++)
    {
      the_frame_set->ranges[i].offset=offset;
      offset+=WORDS_PER_FRAME*the_frame_set->ranges[i].len+WORDS_BETWEEN_FRAMES+WORDS_PER_FRAME; 
    }
  offset+=WORDS_AFTER_FRAMES;

#ifdef TIME_BERT
	time_us_read=-1;
	time_us_write=-1;
	time_us_logical=-1;
	time_us_physical=-1;
	XTime_GetTime(&cstart);
#endif

  // allocate frame data
  uint32_t *frame_data=(uint32_t *)malloc(sizeof(uint32_t)*offset);

  if (frame_data == NULL)
    return BST_NULL_PTR;

#ifdef CLEAR_FRAME_DATA
    for(int i=0;i<offset;i++) frame_data[i]=0;
#endif

  // read back to front due to padding data that comes with readback
  for (int i=the_frame_set->num_ranges-1;i>-1;i--)
    {
      if (the_frame_set->ranges[i].has_read) // only read data for reads
	{
      
	  uint32_t WrdCnt = WORDS_PER_FRAME * (the_frame_set->ranges[i].len + 1)
	    + PAD_WORDS; // AMD: match corrections above + DATA_DMA_OFFSET/4;
	  Status = XFpga_GetPlConfigDataRange(XFpgaInstance,
					 (UINTPTR)&frame_data[the_frame_set->ranges[i].offset-(WORDS_PER_FRAME+PAD_WORDS+DATA_DMA_OFFSET/4)],
					 WrdCnt,
					 the_frame_set->ranges[i].frame_base);
	  if (Status != XST_SUCCESS) {
	    return BST_XILFPGA_FAILURE;
	  }
	}
    }
#ifdef TIME_BERT
	XTime_GetTime(&cend);
	time_us_read=(double) ((cend - cstart) * 1000000.0) / COUNTS_PER_SECOND;
	XTime_GetTime(&cstart);
#endif

  //  uint32_t *pointer_from_readback=&frame_data[WORDS_PER_FRAME + PAD_WORDS + DATA_DMA_OFFSET/4];
    int status = BST_GENERAL_FAIL;
    for (int i=0;i<num;i++)
    {
      if (meminfo[i].operation==BERT_OPERATION_READ) {

	if ((accel_memories_logical!=(struct accel_memory *)NULL)
	    && (accel_memories_logical[meminfo[i].logical_mem].lookup_quanta>1))
	  status = bert_accelerated_to_logical(meminfo[i].logical_mem,
				   frame_data,
				   meminfo[i].data,
				   meminfo[i].start_addr, meminfo[i].data_length,
				   the_frame_set);

	else
	  status = bert_to_logical(meminfo[i].logical_mem,
				   frame_data,
				   meminfo[i].data,
				   meminfo[i].start_addr, meminfo[i].data_length,
				   the_frame_set);
	  
        if (status != BST_SUCCESS)
          return status;
      }

      if (meminfo[i].operation==BERT_OPERATION_ACCELERATED_READ) {
        status = bert_accelerated_to_logical(meminfo[i].logical_mem,
				     frame_data,
				     meminfo[i].data,
				     meminfo[i].start_addr,
				     meminfo[i].data_length,
				     the_frame_set
				     );      
        if (status != BST_SUCCESS)
          return status;
      }
    }

#ifdef TIME_BERT
	XTime_GetTime(&cend);
	time_us_logical=(double) ((cend - cstart) * 1000000.0) / COUNTS_PER_SECOND;
	XTime_GetTime(&cstart);
#endif
  
  for (int i=0;i<num;i++)
    {
      if (meminfo[i].operation==BERT_OPERATION_WRITE) {

	if ((accel_memories_physical!=(struct accel_memory *)NULL)
	    && (accel_memories_physical[meminfo[i].logical_mem].lookup_quanta>1))
	  status = bert_accelerated_to_physical(meminfo[i].logical_mem,
						frame_data,
						meminfo[i].data,
						meminfo[i].start_addr, meminfo[i].data_length,
						the_frame_set);

	else
	  status = bert_to_physical(meminfo[i].logical_mem,
				    frame_data,
				    meminfo[i].data,
				    meminfo[i].start_addr, meminfo[i].data_length,
				    the_frame_set);
	if (status != BST_SUCCESS)
	      return status;
      }

      if (meminfo[i].operation==BERT_OPERATION_ACCELERATED_WRITE) {
        status = bert_accelerated_to_physical(meminfo[i].logical_mem,
				     frame_data,
				     meminfo[i].data,
				     meminfo[i].start_addr,
				     meminfo[i].data_length,
				     the_frame_set
				     );    
        if (status != BST_SUCCESS)
          return status;
	  }
    }

#ifdef TIME_BERT
	XTime_GetTime(&cend);
	time_us_physical=(double) ((cend - cstart) * 1000000.0) / COUNTS_PER_SECOND;
#endif

    // write control data into slots left in frame data .... and find length of write data
  int len=0;
  for (int i = 0; i < the_frame_set->num_ranges; i++) {
      if (the_frame_set->ranges[i].we_bits==0)
	      break; // drop out when hit first read
      uint32_t* arr = &frame_data[the_frame_set->ranges[i].offset - WORDS_BETWEEN_FRAMES];
      // Add ctrl commands
      arr[0] = 0x30002001; // COR 0
      arr[1] = the_frame_set->ranges[i].frame_base;
      arr[2] = 0x30008001; // write CMD
      arr[3] = 0x00000001; //  WCFG
      arr[4] = 0x20000000;
      arr[5] = 0x30004000;
      int wrds = (the_frame_set->ranges[i].len + 1) * WORDS_PER_FRAME;
      arr[6] = 0x50000000 | wrds;
      len += wrds + WORDS_BETWEEN_FRAMES;
      // Clear write mask bits
      int we_bits=the_frame_set->ranges[i].we_bits;
#ifdef DEBUG_TRANSFUSE_WE
      PRINT("test_transfuse_we webits=%x at frame_base=%x\n",we_bits,the_frame_set->ranges[i].frame_base);
#endif
      for (int j = 0; j < the_frame_set->ranges[i].len; j++) {
	     arr = &frame_data[the_frame_set->ranges[i].offset + j*WORDS_PER_FRAME];
	for (int k = 0; k < WE_BITS_PER_FRAME; k++) {
	  int b = bitlocation[k];
	  if (((we_bits>>(2*k)) & 0x03)!=0)  //we_bits is at BRAM18 granularity -- change to BRAM36 here
	    arr[b / 32] &= ~(1 << (31 - (b % 32)));
          else
	    arr[b / 32] |= (1 << (31 - (b % 32))); // This could overwrite bits that were reset on purpose?
	}
      }      
  }
  u32 Flags=XFPGA_PARTIAL_EN | XFPGA_ONLY_BIN_EN;
  
#ifdef TIME_BERT
	XTime_GetTime(&cstart);
#endif

  // 3: write back
  if (len>0) {
     Status=XFpga_PL_Frames_Load(XFpgaInstance,(UINTPTR)frame_data,Flags,len);
     if (Status != XST_SUCCESS) {
       return BST_XILFPGA_FAILURE;
     }
  }

#ifdef TIME_BERT
	XTime_GetTime(&cend);
	time_us_write=(double) ((cend - cstart) * 1000000.0) / COUNTS_PER_SECOND;
#endif

  free(frame_data);
  free(the_frame_set->ranges);
  free(the_frame_set);

#ifdef TIME_BERT
  XTime_GetTime(&tend);
  time_us_transfuse = (double) ((tend - tstart) * 1000000.0) / COUNTS_PER_SECOND;
#endif
  
  return BST_SUCCESS;
  
}

int logical_memory_slot(char *mname, int total_names)
{
		for(int i=0;i<total_names;i++) {
			if (strcmp(mname,logical_names[i])==0)
				return(i);
		}
		return(-1);
}

