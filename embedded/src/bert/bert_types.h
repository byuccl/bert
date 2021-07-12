#ifndef BERT_TYPES_H
#define BERT_TYPES_H
struct frame_range {
   int first_frame;
   int len;
  int we_bits; // BRAM18 write enables, so needs to be grouped in pairs before actually
               //  use with frame write enables that are at RAMB36 granularity
  // will need this to read newest compressed files -- looks like tolerates having it on old files?
   int has_live_ramb18_partner; // coming soon
};


struct frame_bits {
  int num_bits;
  int frame_address;
  int *bit_loc;
};
  

struct segment_repeats {
  int num_repeats;
  int num_frames;
  int bits_in_repeat;
  int slots_in_repeat;
  int unique_frames_in_repeat;
  struct frame_bits *frame_bits;
  int *unique_frames;
};
  

struct logical_memory {
  int nframe_ranges;
  int wordlen;
  int words;
  int num_segments;
  struct frame_range *frame_ranges;
  struct segment_repeats *repeats;
};

// for multiple frame range use
struct frame_range_offset {
  int frame_base;
  int len;
  int we_bits;
  int has_read;
  // may also need the has live partner bit
  // ...not need for convservative -- has_read is enough
  int offset;
};

struct frame_set {
  int num_ranges;
  struct frame_range_offset *ranges;
};

struct accel_memory {
  int lookup_tables;
  int u64_per_lookup;
  int lookup_quanta;
  int frame_bit_offset;
  int bit_low;
  int bit_high; // for RAMB18 case
  uint64_t **trans_tables;
};
#endif