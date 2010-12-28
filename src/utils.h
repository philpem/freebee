#ifndef _H_UTILS
#define _H_UTILS

#ifndef NDEBUG
/// Log a message to stderr
#  define LOG(x, ...) do { fprintf(stderr, "%s:%d:%s(): " x "\n", __FILE__, __LINE__, __PRETTY_FUNCTION__, ##__VA_ARGS__); } while (0)
/// Log a message to stderr if 'cond' is true
#  define LOG_IF(cond, x, ...) do { if (cond) fprintf(stderr, "%s:%d:%s(): " x "\n", __FILE__, __LINE__, __PRETTY_FUNCTION__, ##__VA_ARGS__); } while (0)
#else
#define LOG(x, ...)
#define LOG_IF(cond, x, ...)
#endif

#endif // _H_UTILS
