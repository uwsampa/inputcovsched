// FOR KLEE BUILD: a simple, custom openssl config
// We don't want any assembly code
#ifndef OPENSSL_NO_ASM
#define OPENSSL_NO_ASM
#endif 

#ifndef OPENSSL_NO_INLINE_ASM
#define OPENSSL_NO_INLINE_ASM
#endif
