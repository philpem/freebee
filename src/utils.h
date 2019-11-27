#ifndef _UTILS_H
#define _UTILS_H

#include <stdio.h>

#ifndef NDEBUG
/// Log a message to stderr
#  define LOG(x, ...) do { fprintf(stderr, "%s:%d:%s(): " x "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__); } while (0)
#  define LOGS(x) do { fprintf(stderr, "%s:%d:%s(): " x "\n", __FILE__, __LINE__, __func__); } while (0)
/// Log a message to stderr if 'cond' is true
#  define LOG_IF(cond, x, ...) do { if (cond) fprintf(stderr, "%s:%d:%s(): " x "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__); } while (0)
#  define LOG_IFS(cond, x) do { if (cond) fprintf(stderr, "%s:%d:%s(): " x "\n", __FILE__, __LINE__, __func__); } while (0)
#else
#define LOG(x, ...)
#define LOGS(x)
#define LOG_IF(cond, x, ...)
#define LOG_IFS(cond, x)
#endif

/// Get the number of elements in an array
#define NELEMS(x) (sizeof(x)/sizeof(x[0]))

#endif // _H_UTILS
