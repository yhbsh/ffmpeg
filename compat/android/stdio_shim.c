/* Bridge for libraries built against older Android NDK stdio where
 * stderr/stdin/stdout were plain extern symbols. Modern bionic exposes
 * __sF[] and macros stderr = &__sF[2], etc.
 */
#include <stdio.h>
#undef stderr
#undef stdin
#undef stdout
extern FILE __sF[];
FILE *stderr = &__sF[2];
FILE *stdin  = &__sF[0];
FILE *stdout = &__sF[1];
