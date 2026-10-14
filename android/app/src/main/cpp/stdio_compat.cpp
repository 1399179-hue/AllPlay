// Android API < 23 的 bionic 不导出 stdin/stdout/stderr 符号（只导出 __sF，见 <stdio.h>）。
// 但 NDK 预编译的 libc++（iostream.o，std::cin/cout/cerr 的缓冲实现）直接引用这三个符号，
// 用 <fstream> 会把它拉进链接 → undefined symbol。这里按 bionic 老命名定义别名，指向 __sF。
#if defined(__ANDROID__) && __ANDROID_API__ < 23
#include <cstdio>

#undef stdin
#undef stdout
#undef stderr

extern "C" FILE __sF[3];

extern "C" {
FILE* stdin  = &__sF[0];
FILE* stdout = &__sF[1];
FILE* stderr = &__sF[2];
}
#endif
