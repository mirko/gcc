/* { dg-require-effective-target arm_v8_1m_mve_ok } */
/* { dg-add-options arm_v8_1m_mve } */
/* { dg-additional-options "-O2" } */

#include "arm_mve.h"

uint32x4_t
foo (uint32x4_t a, uint32x4_t b)
{
  return vhaddq_u32 (a, b);
}

/* { dg-final { scan-assembler "vhadd.u32"  }  } */

uint32x4_t
foo1 (uint32x4_t a, uint32x4_t b)
{
  return vhaddq (a, b);
}

/* { dg-final { scan-assembler "vhadd.u32"  }  } */
