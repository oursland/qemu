/*
 * SH4 Double-Precision FPU Test
 *
 * Tests that QEMU properly handles SH4 FPU instructions in
 * double-precision mode (FPSCR.PR=1).
 *
 * Build: sh4-linux-gnu-gcc -O -g -static -o test-fpu-dp test-fpu-dp.c -lm
 * Run:   qemu-sh4 ./test-fpu-dp
 *
 * Note: This test uses bit-pattern comparison via memcpy to validate
 * double values, avoiding GCC SH4 calling convention issues when
 * passing doubles through function arguments.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static int failures = 0;

/* Compare double bit patterns to avoid passing doubles through
 * function arguments, which can trigger pre-existing GCC/QEMU
 * double register passing issues on SH4. */
static void check_bits(const char *name, volatile double *got,
                       uint64_t expected_bits)
{
    uint64_t got_bits;
    memcpy(&got_bits, (const void *)got, sizeof(got_bits));
    if (got_bits == expected_bits) {
        printf("PASS: %s (bits=0x%016llx)\n", name,
               (unsigned long long)got_bits);
    } else {
        printf("FAIL: %s: got=0x%016llx, expected=0x%016llx\n", name,
               (unsigned long long)got_bits,
               (unsigned long long)expected_bits);
        failures++;
    }
}

static void check_int(const char *name, int32_t got, int32_t expected)
{
    if (got != expected) {
        printf("FAIL: %s: got %d, expected %d\n", name, got, expected);
        failures++;
    } else {
        printf("PASS: %s = %d\n", name, got);
    }
}

/*
 * Test lds Rm, fpscr: set FPSCR.PR=1 then back to PR=0
 */
static void test_fpscr_switch(void)
{
    uint32_t fpscr_val, fpscr_saved;

    printf("--- test_fpscr_switch ---\n");

    /* Save current FPSCR */
    __asm__ volatile("sts fpscr, %0" : "=r"(fpscr_saved));
    printf("PASS: initial FPSCR = 0x%08x\n", fpscr_saved);

    /* Set PR=1 (bit 19) */
    fpscr_val = fpscr_saved | (1 << 19);
    __asm__ volatile("lds %0, fpscr" : : "r"(fpscr_val));

    /* Read back */
    __asm__ volatile("sts fpscr, %0" : "=r"(fpscr_val));
    if (fpscr_val & (1 << 19)) {
        printf("PASS: FPSCR.PR=1 set correctly (0x%08x)\n", fpscr_val);
    } else {
        printf("FAIL: FPSCR.PR not set (0x%08x)\n", fpscr_val);
        failures++;
    }

    /* Clear PR and verify */
    fpscr_val &= ~(1 << 19);
    __asm__ volatile("lds %0, fpscr" : : "r"(fpscr_val));
    __asm__ volatile("sts fpscr, %0" : "=r"(fpscr_val));
    if (!(fpscr_val & (1 << 19))) {
        printf("PASS: FPSCR.PR=0 cleared correctly (0x%08x)\n", fpscr_val);
    } else {
        printf("FAIL: FPSCR.PR not cleared (0x%08x)\n", fpscr_val);
        failures++;
    }

    /* Restore original FPSCR so subsequent tests work correctly */
    __asm__ volatile("lds %0, fpscr" : : "r"(fpscr_saved));
}

/*
 * Test int -> double conversion
 * Exercises: float fpul,drN in PR=1 mode, __floatdidf
 */
static void test_float_int_to_double(void)
{
    printf("--- test_float_int_to_double ---\n");

    /* int32 -> double(42) = 0x4045000000000000 */
    volatile int32_t vi1 = 42;
    volatile double vd1 = (double)vi1;
    check_bits("int32->double(42)", &vd1, 0x4045000000000000ULL);

    /* int32 -> double(-100) = 0xC059000000000000 */
    volatile int32_t vi2 = -100;
    volatile double vd2 = (double)vi2;
    check_bits("int32->double(-100)", &vd2, 0xC059000000000000ULL);

    /* int32 -> double(0) = 0x0000000000000000 */
    volatile int32_t vi3 = 0;
    volatile double vd3 = (double)vi3;
    check_bits("int32->double(0)", &vd3, 0x0000000000000000ULL);

    /* int64 -> double (exercises __floatdidf) */
    volatile int64_t vll = 1234567890123LL;
    volatile double vd4 = (double)vll;
    check_bits("int64->double(1234567890123)", &vd4, 0x4271F71FB04CB000ULL);
}

/*
 * Test double-precision arithmetic: fadd, fsub, fmul, fdiv
 */
static void test_double_arithmetic(void)
{
    printf("--- test_double_arithmetic ---\n");

    volatile double a = 3.0;
    volatile double b = 2.0;

    volatile double sum = a + b;
    check_bits("3.0 + 2.0", &sum, 0x4014000000000000ULL);  /* 5.0 */

    volatile double diff = a - b;
    check_bits("3.0 - 2.0", &diff, 0x3FF0000000000000ULL); /* 1.0 */

    volatile double prod = a * b;
    check_bits("3.0 * 2.0", &prod, 0x4018000000000000ULL); /* 6.0 */

    volatile double quot = a / b;
    check_bits("3.0 / 2.0", &quot, 0x3FF8000000000000ULL); /* 1.5 */
}

/*
 * Test ftrc: double -> int truncation
 */
static void test_ftrc(void)
{
    printf("--- test_ftrc ---\n");

    volatile double d1 = 42.9;
    volatile int32_t i1 = (int32_t)d1;
    check_int("ftrc(42.9)", i1, 42);

    volatile double d2 = -7.3;
    volatile int32_t i2 = (int32_t)d2;
    check_int("ftrc(-7.3)", i2, -7);
}

/*
 * Test fneg and fabs in double-precision mode
 */
static void test_fneg_fabs(void)
{
    printf("--- test_fneg_fabs ---\n");

    volatile double pos = 3.0;
    volatile double neg = -pos;
    check_bits("fneg(3.0)", &neg, 0xC008000000000000ULL); /* -3.0 */

    volatile double neg2 = -42.5;
    volatile double abs_neg2;
    /* Use fabs via asm or let compiler choose */
    if (neg2 < 0) abs_neg2 = -neg2; else abs_neg2 = neg2;
    check_bits("abs(-42.5)", &abs_neg2, 0x4045400000000000ULL); /* 42.5 */
}

/*
 * Test fcnvsd / fcnvds: single <-> double conversion
 */
static void test_fcnv(void)
{
    printf("--- test_fcnv ---\n");

    /* float -> double (fcnvsd) */
    volatile float f = 1.5f;
    volatile double d = (double)f;
    check_bits("fcnvsd(1.5f)", &d, 0x3FF8000000000000ULL); /* 1.5 */

    /* double -> float (fcnvds) */
    volatile double d2 = 2.5;
    volatile float f2 = (float)d2;
    uint32_t f2_bits;
    memcpy(&f2_bits, (const void *)&f2, sizeof(f2_bits));
    if (f2_bits == 0x40200000) {
        printf("PASS: fcnvds(2.5) (bits=0x%08x)\n", f2_bits);
    } else {
        printf("FAIL: fcnvds(2.5): got=0x%08x, expected=0x40200000\n",
               f2_bits);
        failures++;
    }
}

/*
 * Test fsqrt in double-precision mode
 */
static void test_fsqrt(void)
{
    printf("--- test_fsqrt ---\n");

    volatile double d = 4.0;
    volatile double s;
    __asm__ volatile(
        "sts    fpscr, r0\n\t"
        "mov    r0, r1\n\t"
        "mov    #8, r2\n\t"
        "shll16 r2\n\t"           /* r2 = 0x00080000 = PR bit */
        "or     r2, r0\n\t"
        "lds    r0, fpscr\n\t"    /* ensure PR=1 */
        "fmov.s @%1+, fr1\n\t"   /* load low word */
        "fmov.s @%1, fr0\n\t"    /* load high word */
        "fsqrt  dr0\n\t"
        "add    #4, %0\n\t"
        "fmov.s fr0, @%0\n\t"    /* store high word */
        "add    #-4, %0\n\t"
        "fmov.s fr1, @%0\n\t"    /* store low word */
        "lds    r1, fpscr\n\t"   /* restore fpscr */
        :
        : "r"(&s), "r"(&d)
        : "r0", "r1", "r2", "fr0", "fr1", "memory"
    );
    check_bits("fsqrt(4.0)", &s, 0x4000000000000000ULL); /* 2.0 */
}

/*
 * Test long long -> double (the specific __floatdidf trigger)
 */
static void test_floatdidf(void)
{
    printf("--- test_floatdidf ---\n");

    volatile long long ll1 = 1LL;
    volatile double d1 = (double)ll1;
    check_bits("__floatdidf(1)", &d1, 0x3FF0000000000000ULL);

    volatile long long ll2 = -1LL;
    volatile double d2 = (double)ll2;
    check_bits("__floatdidf(-1)", &d2, 0xBFF0000000000000ULL);

    volatile long long ll3 = 0x7FFFFFFFLL;
    volatile double d3 = (double)ll3;
    check_bits("__floatdidf(0x7FFFFFFF)", &d3, 0x41DFFFFFFFC00000ULL);

    volatile long long ll4 = 0x100000000LL;
    volatile double d4 = (double)ll4;
    check_bits("__floatdidf(0x100000000)", &d4, 0x41F0000000000000ULL);
}

/*
 * Test flds/fsts: FPUL load and store
 */
static void test_flds_fsts(void)
{
    printf("--- test_flds_fsts ---\n");

    float fin = 2.5f;
    float fout;
    uint32_t fpul_bits;

    /* flds frN, fpul  then  fsts fpul, frM */
    __asm__ volatile(
        "flds  %1, fpul\n\t"
        "sts   fpul, %0\n\t"
        : "=r"(fpul_bits)
        : "f"(fin)
        : "fpul"
    );
    if (fpul_bits == 0x40200000) {  /* 2.5f */
        printf("PASS: flds/sts fpul (bits=0x%08x)\n", fpul_bits);
    } else {
        printf("FAIL: flds/sts fpul: got=0x%08x, expected=0x40200000\n",
               fpul_bits);
        failures++;
    }

    __asm__ volatile(
        "lds   %1, fpul\n\t"
        "fsts  fpul, %0\n\t"
        : "=f"(fout)
        : "r"(fpul_bits)
        : "fpul"
    );
    uint32_t fout_bits;
    memcpy(&fout_bits, &fout, sizeof(fout_bits));
    if (fout_bits == 0x40200000) {
        printf("PASS: lds/fsts fpul (bits=0x%08x)\n", fout_bits);
    } else {
        printf("FAIL: lds/fsts fpul: got=0x%08x, expected=0x40200000\n",
               fout_bits);
        failures++;
    }
}

/*
 * Test fcnvsd with dynamic PR switch (glibc pattern)
 *
 * This is the exact pattern that glibc's printf uses:
 *   lds r1, fpscr    ; switch to PR=1
 *   flds fr0, fpul   ; load single to FPUL
 *   fcnvsd fpul, drN ; convert single→double
 *
 * The test starts with PR=0, switches to PR=1, executes fcnvsd,
 * then restores PR.
 */
static void test_fcnvsd_with_pr_switch(void)
{
    printf("--- test_fcnvsd_with_pr_switch ---\n");

    volatile float fin = 3.14f;
    volatile double dout;
    uint32_t fin_bits;
    memcpy(&fin_bits, (const void *)&fin, sizeof(fin_bits));

    /* Exact glibc sequence: start in PR=0, switch to PR=1, fcnvsd */
    __asm__ volatile(
        "sts   fpscr, r3\n\t"      /* save original FPSCR */
        /* Clear PR (set PR=0) */
        "mov   r3, r0\n\t"
        "mov   #8, r2\n\t"
        "shll16 r2\n\t"            /* r2 = 0x00080000 = PR bit */
        "not   r2, r2\n\t"         /* r2 = ~PR */
        "and   r2, r0\n\t"
        "lds   r0, fpscr\n\t"      /* FPSCR.PR = 0 */
        /* Now switch to PR=1 (like glibc does) */
        "mov   r3, r0\n\t"
        "mov   #8, r2\n\t"
        "shll16 r2\n\t"
        "or    r2, r0\n\t"
        "lds   r0, fpscr\n\t"      /* FPSCR.PR = 1 */
        /* Load float and convert */
        "lds   %1, fpul\n\t"       /* load float bits into FPUL */
        "fcnvsd fpul, dr4\n\t"     /* single → double */
        /* Store result: dr4 = (fr4-high, fr5-low) */
        "add   #4, %0\n\t"
        "fmov.s fr4, @%0\n\t"      /* store high word */
        "add   #-4, %0\n\t"
        "fmov.s fr5, @%0\n\t"      /* store low word */
        /* Restore FPSCR */
        "lds   r3, fpscr\n\t"
        :
        : "r"(&dout), "r"(fin_bits)
        : "r0", "r2", "r3", "fpul", "fr4", "fr5", "memory"
    );

    /* 3.14f as double = 0x40091EB860000000
     * (float 3.14 = 0x4048F5C3, promoted to double preserving value) */
    check_bits("fcnvsd(3.14f) via PR switch", &dout, 0x40091EB860000000ULL);
}

/*
 * Test fcnvsd with FPSCR.FR=1 (register bank swap)
 *
 * When FR=1, frN maps to bank 1 registers (XF0-XF15).
 * fcnvsd should still work correctly since it operates
 * on the current bank's registers.
 */
static void test_fcnvsd_with_fr_bank(void)
{
    printf("--- test_fcnvsd_with_fr_bank ---\n");

    volatile float fin = 1.0f;
    volatile double dout;
    uint32_t fin_bits;
    memcpy(&fin_bits, (const void *)&fin, sizeof(fin_bits));

    __asm__ volatile(
        "sts   fpscr, r3\n\t"      /* save original FPSCR */
        /* Set FR=1 and PR=1 */
        "mov   r3, r0\n\t"
        "mov   #0x28, r2\n\t"
        "shll16 r2\n\t"            /* r2 = 0x00280000 = FR|PR bits */
        "or    r2, r0\n\t"
        "lds   r0, fpscr\n\t"      /* FPSCR: FR=1, PR=1 */
        /* Load float and convert */
        "lds   %1, fpul\n\t"
        "fcnvsd fpul, dr0\n\t"     /* single → double in bank 1 */
        /* Store result manually */
        "add   #4, %0\n\t"
        "fmov.s fr0, @%0\n\t"      /* store high word */
        "add   #-4, %0\n\t"
        "fmov.s fr1, @%0\n\t"      /* store low word */
        /* Restore FPSCR */
        "lds   r3, fpscr\n\t"
        :
        : "r"(&dout), "r"(fin_bits)
        : "r0", "r2", "r3", "fpul", "fr0", "fr1", "memory"
    );

    /* 1.0f promoted to double = 0x3FF0000000000000 */
    check_bits("fcnvsd(1.0f) with FR=1", &dout, 0x3FF0000000000000ULL);
}

int main(void)
{
    printf("SH4 Double-Precision FPU Tests\n");
    printf("==============================\n\n");

    test_fpscr_switch();
    test_float_int_to_double();
    test_double_arithmetic();
    test_ftrc();
    test_fneg_fabs();
    test_fcnv();
    test_fsqrt();
    test_floatdidf();
    test_flds_fsts();
    test_fcnvsd_with_pr_switch();
    test_fcnvsd_with_fr_bank();

    printf("\n==============================\n");
    if (failures == 0) {
        printf("All tests passed!\n");
    } else {
        printf("%d test(s) FAILED!\n", failures);
    }

    return failures ? 1 : 0;
}
