#include "bert.h"
#include "mydesign.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

// Macros to deal with uint64_t discrepancy
#ifdef __linux__
#define FMT_UINT64 "%lX"
#endif
#ifdef __APPLE__
#define FMT_UINT64 "%llX"
#endif

//#define DEBUG
#ifdef DEBUG
#define DEBUG_PRINT(fmt, args...) fprintf(stderr, fmt, ##args)
#else
#define DEBUG_PRINT(fmt, args...)     \
    if (verbose)                      \
    {                                 \
        fprintf(stderr, fmt, ##args); \
    }
#endif

#define CEIL(x, y) (((x) + (y)-1) / (y))
#define LINE_LEN 4096
#define SCRIPT_LEN 4096

#define MAX_WORD_COUNT 4608 // Assuming 1B is smallest word size
#define MAX_WIDTH 72        // Bits per word in memory
#define ARR_SIZE (CEIL(MAX_WIDTH, 64) * MAX_WORD_COUNT)
uint64_t mem_write[NUM_LOGICAL][ARR_SIZE] = {0U};

enum
{
    BIN,
    DAT
};

XFpga XFpgaInstance = {0U};
FILE *output = NULL;
struct bert_meminfo ops[NUM_LOGICAL] = {0U};
bool verbose = false;
int format = BIN;

/*
* TODO:
* - Bigendian file support
* - Show file mismatch
* - Target memories besides using number indices
*/

int readWord(uint64_t *arr, int bytes, FILE *f)
{
    switch (format)
    {
    case BIN:
        return fread(arr, bytes, 1, f);

    case DAT:
        return fscanf(f, FMT_UINT64 "\n", arr);
    default:
        fprintf(stderr, "Invalid format.\n");
        exit(1);
    }
}

void setOutput(char *fname)
{
    if (output != NULL)
    {
        fclose(output);
        output = NULL;
    }
    output = fopen(fname, "w");
    if (output == NULL)
    {
        perror(fname);
        exit(1);
    }
}

void writeBitstream()
{
    if (output == NULL)
    {
        printf("No output file specified\n");
        exit(0);
    }

    if (hostwrite_Init(&XFpgaInstance, PART_ID, output) != 0)
    { // IDCODE should be defined in mydesign.h
        printf("readback_Init failed\r\n");
        exit(1);
    }

    struct bert_meminfo write_ops[NUM_LOGICAL] = {0U};
    int opc = 0;
    for (int i = 0; i < NUM_LOGICAL; i++)
    {
        if (ops[i].operation == BERT_OPERATION_WRITE)
        {
            write_ops[opc].operation = ops[i].operation;
            write_ops[opc].data = ops[i].data;
            write_ops[opc].data_length = ops[i].data_length;
            write_ops[opc].start_addr = ops[i].start_addr;
            write_ops[opc].logical_mem = ops[i].logical_mem;
            opc++;
        }
    }
    DEBUG_PRINT("Transfusing %d staged memories\n", opc);
    if (bert_transfuse(opc, write_ops, &XFpgaInstance) != 0)
    {
        printf("bert_write failed\r\n");
        exit(1);
    }

    DEBUG_PRINT("Bitstream written to file\n");

    fclose(output);
    output = NULL;

    for (int i = 0; i < NUM_LOGICAL; i++)
    {
        if (ops[i].operation == BERT_OPERATION_WRITE)
        {

            for (int j = 0; j < ops[i].data_length; j++)
            {
                mem_write[i][j] = 0;
            }
            ops[i].operation = 0;
        }
    }
    fprintf(stderr, "INFO: Bitstream written\n");
}

void prepData(int mem, FILE *input)
{
    if (mem < 0 || mem >= NUM_LOGICAL)
    {
        fprintf(stderr, "WARNING: Invalid memory index %d\n", mem);
        return;
    }
    if (input == NULL)
    {
        fprintf(stderr, "prepData: input is null\n");
        exit(1);
    }
    struct bert_meminfo *op = &(ops[mem]);
    struct compressed_logical_memory *logicalmem = &(logical_memories[mem]);
    int cellsPerWord = CEIL(logicalmem->wordlen, 64);
    int words = logicalmem->words;
    size_t wordSize = CEIL(logicalmem->wordlen, 8);
    DEBUG_PRINT("Mem %d info: wordlen: %d, cellsPerWord: %d, words: %d, wordSize: %ld\n", mem, logicalmem->wordlen, cellsPerWord, words, wordSize);
    for (int i = 0; i < words; i++)
    {
        int res = 0;
        int k = 0;
        for (int j = wordSize - 8; j >= 0; j -= 8)
        {
            res = readWord(&(mem_write[mem][i * cellsPerWord + k]), 8, input);
            if (res < 1)
            {
                fprintf(stderr, "WARNING: File ended before filling memory %d.\n", mem);
                goto prepDataEpilogue;
            }
            DEBUG_PRINT("prepData: Staged 0x" FMT_UINT64 " in word %d of mem %d\n", mem_write[mem][i * cellsPerWord + k], i * cellsPerWord + k, mem);
            k++;
        }

        if (wordSize % 8 != 0)
        {
            res = readWord(&(mem_write[mem][i * cellsPerWord + k]), wordSize % 8, input);
            if (res < 1)
            {
                fprintf(stderr, "WARNING: File ended before filling memory %d.\n", mem);
                goto prepDataEpilogue;
            }
            DEBUG_PRINT("prepData: Staged 0x" FMT_UINT64 " in word %d of mem %d\n", mem_write[mem][i * cellsPerWord + k], i * cellsPerWord + k, mem);
        }
    }
    if (!feof(input))
    {
        long int fin = ftell(input);
        fseek(input, 0, SEEK_END);
        long int end = ftell(input);
        fprintf(stderr, "WARNING: File size mismatch. Input file for mem %d is larger than the memory by %ld bytes (This can be ignored)\n", mem, end - fin);
    }

prepDataEpilogue:
    if (op->operation != 0)
        fprintf(stderr, "WARNING: Overwriting operation for mem %d\n", mem);
    op->operation = BERT_OPERATION_WRITE;
    op->data = mem_write[mem];
    op->data_length = words;
    op->start_addr = 0;
    op->logical_mem = mem;

    fprintf(stderr, "INFO: Mem %d staged for writing of %d words\n", mem, words);
}

void stageWithFname(int id, char *fname)
{
    FILE *hex = fopen(fname, "r");
    if (hex == NULL)
    {
        perror(fname);
        exit(1);
    }
    prepData(id, hex);
    fclose(hex);
}

void parseArgs(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stdout, "bitstream_gen --help for usage\n");
        exit(1);
    }
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            fprintf(stdout, "BERT Bitstream Gen - matth2k@seas.upenn.edu\n"
                            "args:\n\n"
                            "--stage BY_<ID/NAME>:<mem>:<input file>    (i.e --stage BY_NAME:v1_buffer_U:deadbeef.dat)\n"
                            "                                           BY_NAME <mem> searches for memories containing the string <mem>\n"
                            "                                           BY_ID <mem> selects the memory with integer id <mem>\n"
                            "--output -o <output file>                  Where to write partial bitstream to\n"
                            "--read_format <format>                     Values: 'bin' (default), 'dat'. File format used by reader when staging memories.\n"
                            "--mlist -l                                 List logical memories available to program\n"
                            "--help -h                                  Prints help info\n"
                            "--verbose -v                               Debug print statements\n");

            exit(0);
        }

        if (strcmp(argv[i], "--mlist") == 0 || strcmp(argv[i], "-l") == 0)
        {
            printf("BERT Bitstream Gen - matth2k@seas.upenn.edu\n"
                   "ID  |  Memory Logical Name\n\n");

            for (int j = 0; j < NUM_LOGICAL; j++)
            {
                printf("%d     %s\n", j, logical_names[j]);
            }

            exit(0);
        }
    }

    printf("BERT Bitstream Gen - matth2k@seas.upenn.edu\n");
}

void parseCMD(char *cmd)
{
    char *tok[2048] = {0U};
    tok[0] = strtok(cmd, " ");
    int i = 1;
    if (tok[0] == NULL)
        return;
    while ((tok[i] = strtok(NULL, " :")) != NULL)
        i++;
    if (strcmp(tok[0], "--add") == 0 || strcmp(tok[0], "--stage") == 0)
    {

        if (i == 4)
        {
            if (strcmp(tok[1], "BY_NAME") == 0)
            {
                int id = -1;
                for (int j = 0; j < NUM_LOGICAL; j++)
                {
                    if (strlen(tok[2]) > 2 && strstr(logical_names[j], tok[2]) != NULL)
                    {
                        printf("INFO: BY_NAME:%s matched memory '%s'\n", tok[2], logical_names[j]);
                        stageWithFname(j, tok[3]);
                    }
                }
            }
            else if (strcmp(tok[1], "BY_ID") == 0)
            {
                stageWithFname(atoi(tok[2]), tok[3]);
            }
            else
            {
                fprintf(stderr, "stage: bad args\n");
                exit(1);
            }
        }
        else
        {
            fprintf(stderr, "stage: incorrect number of args\n");
            exit(1);
        }
    }
    else if (strcmp(tok[0], "--output") == 0 || strcmp(tok[0], "-o") == 0)
    {
        if (tok[1] != NULL)
        {
            setOutput(tok[1]);
            //writeBitstream();
        }
        else
        {
            fprintf(stderr, "output: file arg missing\n");
            exit(1);
        }
    }
    else if (strcmp(tok[0], "--read_format") == 0)
    {
        if (tok[1] != NULL)
        {
            if (strcmp(tok[1], "bin") == 0)
            {
                format = BIN;
                printf("INFO: Read format set to .bin.m\n");
            }
            else if (strcmp(tok[1], "dat") == 0)
            {
                format = DAT;
                printf("INFO: Read format set to .dat.\n");
            }
        }
        else
        {
            fprintf(stderr, "read_format: format arg missing or invalid\n");
            exit(1);
        }
    }
    else if (strcmp(tok[0], "--verbose") == 0 || strcmp(tok[0], "-v") == 0)
    {
        printf("Verbose\n");
        verbose = true;
    }
    else
    {
        fprintf(stderr, "WARNING: Unrecognized command %s\n", tok[0]);
    }
}

void parseScript(int argc, char **argv)
{
    char buf[LINE_LEN] = {0U};
    for (int i = 1; i < argc; i++)
    {
        if (argv[i][0] == '-')
        {
            parseCMD(buf);
            buf[0] = '\0';
            strcat(buf, argv[i]);
        }
        else
        {
            strcat(buf, " ");
            strcat(buf, argv[i]);
        }
    }
    parseCMD(buf);
}

int main(int argc, char **argv)
{
    parseArgs(argc, argv);
    parseScript(argc, argv);
    writeBitstream();

    return 0;
}