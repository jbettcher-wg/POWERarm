// SPDX-License-Identifier: MIT
//
// POWERarm implements no SVE and no SME, so every way a guest can ask must
// say so. A guest that believes otherwise runs instructions we do not have:
// glibc's __libc_arm_za_disable, for one, drops into an SME lazy-save path
// that starts with CNTD as soon as HWCAP2 bit 23 is set.
//
// Self-checking, so the Pi golden and the emulated run are the same text:
// the Pi has no SVE either, and the fields that do differ between the two
// (HWCAP as a whole, the ISAR0 atomic field) are deliberately not printed.
#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/prctl.h>

static sigjmp_buf JumpBuffer;
static void OnSigill(int Signal) {
  (void)Signal;
  siglongjmp(JumpBuffer, 1);
}

static void Check(const char* Name, int Ok) {
  printf("%s %s\n", Ok ? "PASS" : "FAIL", Name);
}

// An ID register field must read zero. A SIGILL on the MRS is also a pass:
// the guest then has no way to read the field at all.
static void CheckIDField(const char* Name, uint64_t Value, int Trapped, unsigned Lsb) {
  Check(Name, Trapped || ((Value >> Lsb) & 0xf) == 0);
}

#ifndef PR_SVE_GET_VL
#define PR_SVE_GET_VL 51
#endif
#ifndef PR_SME_GET_VL
#define PR_SME_GET_VL 64
#endif

#define READ_ID(Var, Trapped, Encoding)                     \
  do {                                                      \
    Var = 0;                                                \
    Trapped = 0;                                            \
    if (sigsetjmp(JumpBuffer, 1) == 0) {                    \
      __asm__ __volatile__("mrs %0, " Encoding : "=r"(Var)); \
    } else {                                                \
      Trapped = 1;                                          \
    }                                                       \
  } while (0)

int main(void) {
  struct sigaction Action;
  memset(&Action, 0, sizeof Action);
  Action.sa_handler = OnSigill;
  Action.sa_flags = SA_NODEFER;
  sigaction(SIGILL, &Action, NULL);

  const unsigned long HWCap = getauxval(AT_HWCAP);
  const unsigned long HWCap2 = getauxval(AT_HWCAP2);
  Check("hwcap.sve", ((HWCap >> 22) & 1) == 0);
  Check("hwcap2.sve2", ((HWCap2 >> 1) & 1) == 0);
  Check("hwcap2.sveaes", ((HWCap2 >> 2) & 1) == 0);
  Check("hwcap2.sme", ((HWCap2 >> 23) & 1) == 0);

  uint64_t Value;
  int Trapped;
  READ_ID(Value, Trapped, "S3_0_C0_C4_0"); // ID_AA64PFR0_EL1
  CheckIDField("id_aa64pfr0.sve", Value, Trapped, 32);
  READ_ID(Value, Trapped, "S3_0_C0_C4_1"); // ID_AA64PFR1_EL1
  CheckIDField("id_aa64pfr1.sme", Value, Trapped, 24);
  READ_ID(Value, Trapped, "S3_0_C0_C4_4"); // ID_AA64ZFR0_EL1
  Check("id_aa64zfr0.zero", Trapped || Value == 0);
  READ_ID(Value, Trapped, "S3_0_C0_C4_5"); // ID_AA64SMFR0_EL1
  Check("id_aa64smfr0.zero", Trapped || Value == 0);

  errno = 0;
  int Result = prctl(PR_SVE_GET_VL);
  Check("prctl.sve_get_vl", Result < 0 && errno == EINVAL);
  errno = 0;
  Result = prctl(PR_SME_GET_VL);
  Check("prctl.sme_get_vl", Result < 0 && errno == EINVAL);

  // /proc/cpuinfo must not name any of them either.
  int Found = 0;
  FILE* File = fopen("/proc/cpuinfo", "r");
  if (File) {
    char Line[8192];
    while (fgets(Line, sizeof Line, File)) {
      if (strncmp(Line, "Features", 8) != 0) {
        continue;
      }
      static const char* const Names[] = {" sve ", " sve2 ", " sve2aes ", " sme ", " sme2 "};
      for (size_t i = 0; i < sizeof Names / sizeof *Names; ++i) {
        if (strstr(Line, Names[i])) {
          Found = 1;
        }
      }
    }
    fclose(File);
  }
  Check("cpuinfo.no_sve", !Found);
  return 0;
}
