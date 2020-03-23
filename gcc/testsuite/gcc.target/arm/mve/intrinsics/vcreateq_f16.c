/* { dg-require-effective-target arm_v8_1m_mve_fp_ok } */
/* { dg-add-options arm_v8_1m_mve_fp } */
/* { dg-additional-options "-O2" } */

#include "arm_mve.h"

float16x8_t
foo (uint64_t a, uint64_t b)
{
  return vcreateq_f16 (a, b);
}

/* { dg-final { scan-assembler "vmov"  }  } */
