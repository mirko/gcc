/* { dg-require-effective-target arm_v8_1m_mve_ok } */
/* { dg-add-options arm_v8_1m_mve } */
/* { dg-additional-options "-O2" } */

#include "arm_mve.h"

int32x4_t
foo (int32x4_t inactive, int32x4_t a, mve_pred16_t p)
{
  return vshrq_m_n_s32 (inactive, a, 32, p);
}

/* { dg-final { scan-assembler "vpst" } } */
/* { dg-final { scan-assembler "vshrt.s32"  }  } */

int32x4_t
foo1 (int32x4_t inactive, int32x4_t a, mve_pred16_t p)
{
  return vshrq_m (inactive, a, 32, p);
}

/* { dg-final { scan-assembler "vpst" } } */
/* { dg-final { scan-assembler "vshrt.s32"  }  } */
