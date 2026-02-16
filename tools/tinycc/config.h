/* TCC config for Survival Workstation (UEFI, multi-arch) */
#define TCC_VERSION "0.9.28"
#define ONE_SOURCE 1
#define CONFIG_TCC_STATIC 1
#define CONFIG_TCC_PREDEFS 1
#define CONFIG_TCC_SEMLOCK 0
#define CONFIG_TCC_BACKTRACE 0
#define CONFIG_TCC_BCHECK 0
#define TCC_USING_DOUBLE_FOR_LDOUBLE 1
#define CONFIG_TCCDIR "/include"

/* UEFI uses UTF-16 — make wchar_t / L"..." 16-bit like PE */
#define TCC_WCHAR_16 1

/* Architecture-specific target — set by Makefile via -D */
#if !defined(TCC_TARGET_ARM64) && !defined(TCC_TARGET_X86_64)
# ifdef __aarch64__
#  define TCC_TARGET_ARM64 1
# elif defined(__x86_64__)
#  define TCC_TARGET_X86_64 1
# endif
#endif

/* x86_64 UEFI uses PE/COFF output and MS ABI calling convention.
   TCC_TARGET_PE enables: no GOT, MS ABI in code generator, PE linker. */
#ifdef TCC_TARGET_X86_64
# define TCC_TARGET_PE 1
#endif

/* Force native support (F5 in-memory compile+run via tcc_relocate).
   tcc.h's auto-detection fails because _WIN32 != TCC_TARGET_PE on UEFI. */
#define TCC_IS_NATIVE 1
