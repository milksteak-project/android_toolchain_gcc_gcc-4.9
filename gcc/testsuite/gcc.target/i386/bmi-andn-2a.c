/* { dg-do compile } */
/* { dg-options "-O2 -mbmi -fno-inline -dp  --param max-default-completely-peeled-insns=0" } */

#include "bmi-andn-2.c"

/* { dg-final { scan-assembler-times "bmi_andn_si" 1 } } */
