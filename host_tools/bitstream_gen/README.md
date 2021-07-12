# bitstream_gen - Generate BRAM configurations

## To Build

* Generate your bert_gen source files. An example on how to do this with Vivado can be found [here](../../docs/tutorials/huffman/fileprep.md#first-steps-vivado). Simply swap out the example project in the tutorial with your own.
* Run `make all DESIGN_NAME=top DESIGN_DIR=<dir>` where `dir` is the directory containing the files generated in the previous step.
* This will build `bitstream_gen` in the specified directory.

## Running

The general formula to run `bitstream_gen` is:

```console
$ ./bitstream_gen --read_format <fmt> --stage <stage args 1> ... --stage <stage args n> --output <bitstream>
```

* `<fmt>` can take values of either 'bin' or 'dat'. It specifies how the program will read the input data files.
  * 'bin' is raw bytes in a file
  * 'dat' is text-based, where each line is a hexadecimal word
* `<stage args n>` are the arguments to stage a logical memory.
  * Some examples
    * `--stage BY_NAME:v1_buffer_U:deadbeef.dat` will stage all logical memories containing 'v1_buffer_U' in its full name.
    * `--stage BY_ID:0:deadbeef.dat` will stage the first logical memory defined in mydesign.c.
* `<bitstream>` specifies where to write the configuration into a bitstream file

## Help

```console
$ ./bitstream_gen -h
BERT Bitstream Gen - matth2k@seas.upenn.edu
args:

--stage BY_<ID/NAME>:<mem>:<input file>    (i.e --stage BY_NAME:v1_buffer_U:deadbeef.dat)
                                           BY_NAME <mem> searches for memories containing the string <mem>
                                           BY_ID <mem> selects the memory with integer id <mem>
--output -o <output file>                  Where to write partial bitstream to
--read_format <format>                     Values: 'bin' (default), 'dat'. File format used by reader when staging memories.
--mlist -l                                 List logical memories available to program
--help -h                                  Prints help info
--verbose -v                               Debug print statements
```