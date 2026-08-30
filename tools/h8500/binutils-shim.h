// Just enough of binutils for opcodes/h8500-dis.c to build on its own.
//
// The disassembler and its opcode table are binutils 2.16.1 verbatim -- the
// last release that still carried an H8/500 target. They want sysdep.h,
// dis-asm.h and opintl.h, which drag in most of bfd; this supplies only the
// handful of names they actually reference.
//
// h8500-dis.c and h8500-opc.h are GPLv2, so anything built from them is too.
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PARAMS(args) args
#define _(text) text

typedef unsigned char  bfd_byte;
typedef unsigned long  bfd_vma;
typedef void*          PTR;

typedef int (*fprintf_ftype)(void*, const char*, ...);

struct disassemble_info
{
    void*         stream;
    fprintf_ftype fprintf_func;
    int  (*read_memory_func)(bfd_vma, bfd_byte*, unsigned int, struct disassemble_info*);
    void (*memory_error_func)(int, bfd_vma, struct disassemble_info*);
    void* private_data;
};

typedef struct disassemble_info disassemble_info;

int print_insn_h8500(bfd_vma addr, disassemble_info* info);
