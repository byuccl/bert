# The directory containing the .bit, .mdd, and .ll files
DESIGN_DIR=.
DESIGN_NAME=top

# Use aarch64-linux-gnu-gcc for petalinux build
CC=gcc

IDIR =../../embedded/src/bert
CFLAGS=-I$(IDIR) -D HOST_SIDE -D DISABLE_ACCEL -g

ODIR=.
LDIR =../../embedded/src/bert


_DEPS = dummy_xilinx.h mydesign.h
DEPS = $(patsubst %,$(IDIR)/%,$(_DEPS))

_OBJ = dummy_xilinx.o mydesign.o bitstream_gen.o $(LDIR)/bert.o
OBJ = $(patsubst %,$(ODIR)/%,$(_OBJ))

.PHONY: help

help:
	@echo "1. Place your .ll, .mdd, and .bit in your directroy of choice, sharing the same name stem (i.e. top.ll, top.mdd, top.bit)"
	@echo "2. Run make all DESIGN_NAME=<name stem> DESIGN_DIR=<directory containing files>"
	@echo "   DESIGN_DIR defaults to current working directory."

$(DESIGN_DIR)/$(DESIGN_NAME).ll:
	echo "$(DESIGN_DIR)/$(DESIGN_NAME).ll does not exist! Run make help"

$(DESIGN_DIR)/$(DESIGN_NAME).mdd:
	echo "$(DESIGN_DIR)/$(DESIGN_NAME).mdd does not exist! Run make help"

$(DESIGN_DIR)/$(DESIGN_NAME).bit:
	echo "$(DESIGN_DIR)/$(DESIGN_NAME).bit does not exist! Run make help"

../../host_tools/bert_gen/$(DESIGN_NAME)_src:
	mkdir ../../host_tools/bert_gen/$(DESIGN_NAME)_src

mydesign.c: $(DESIGN_DIR)/$(DESIGN_NAME).ll $(DESIGN_DIR)/$(DESIGN_NAME).mdd $(DESIGN_DIR)/$(DESIGN_NAME).bit | ../../host_tools/bert_gen/$(DESIGN_NAME)_src
	cp $(DESIGN_DIR)/$(DESIGN_NAME).ll ../../host_tools/bert_gen/$(DESIGN_NAME)_src/top.ll
	cp $(DESIGN_DIR)/$(DESIGN_NAME).mdd ../../host_tools/bert_gen/$(DESIGN_NAME)_src/top.mdd
	cp $(DESIGN_DIR)/$(DESIGN_NAME).bit ../../host_tools/bert_gen/$(DESIGN_NAME)_src/top.bit
	cd ../../host_tools/bert_gen; cmake CMakeLists.txt
	+$(MAKE) -C ../../host_tools/bert_gen
	cd ../../host_tools/bert_gen; ./gen.sh -gen $(DESIGN_NAME)_src
	cp ../../host_tools/bert_gen/$(DESIGN_NAME)_src/mydesign.c mydesign.c
	cp ../../host_tools/bert_gen/$(DESIGN_NAME)_src/mydesign.h mydesign.h


$(ODIR)/%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

bitstream_gen: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS)
	@if [ $(DESIGN_DIR) != "." ]; then mv -vf bitstream_gen $(DESIGN_DIR)/; fi
	rm -f mydesign.c mydesign.h
	@echo "DONE"
	@echo "Try running $(DESIGN_DIR)/bitstream_gen --help"

all: bitstream_gen

.PHONY: clean

clean:
	rm -f $(ODIR)/*.o *~
	rm -f $(LDIR)/*.o
	rm -f test.bit
	rm -f bitstream_gen
	rm -f mydesign.c
	rm -f mydesign.h