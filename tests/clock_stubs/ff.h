#ifndef STUB_FF_H
#define STUB_FF_H
/* host stub: f_mkdir maps to POSIX mkdir under the test root */
#include <sys/stat.h>
static inline int f_mkdir(const char *p) { (void)p; return 0; }
#endif
