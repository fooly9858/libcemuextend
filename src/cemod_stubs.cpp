#include "cemuextend/guest.hpp"

#if defined(__powerpc__) || defined(__PPC__)

// The Cemu loader validates and replaces exactly one placeholder instruction
// at each public symbol address. Define the stubs as assembler symbols so GCC
// cannot add a prologue before that instruction.
__asm__(
    ".section .text.CEX2Stubs,\"ax\",@progbits\n"
    ".balign 4\n"
    ".global CEX2Query\n.type CEX2Query,@function\nCEX2Query:\n.long 0x0400ffff\n.size CEX2Query,.-CEX2Query\n"
    ".global CEX2Open\n.type CEX2Open,@function\nCEX2Open:\n.long 0x0400ffff\n.size CEX2Open,.-CEX2Open\n"
    ".global CEX2Submit\n.type CEX2Submit,@function\nCEX2Submit:\n.long 0x0400ffff\n.size CEX2Submit,.-CEX2Submit\n"
    ".global CEX2Poll\n.type CEX2Poll,@function\nCEX2Poll:\n.long 0x0400ffff\n.size CEX2Poll,.-CEX2Poll\n"
    ".global CEX2Cancel\n.type CEX2Cancel,@function\nCEX2Cancel:\n.long 0x0400ffff\n.size CEX2Cancel,.-CEX2Cancel\n"
    ".global CEX2Close\n.type CEX2Close,@function\nCEX2Close:\n.long 0x0400ffff\n.size CEX2Close,.-CEX2Close\n"
    ".previous\n");

#endif
