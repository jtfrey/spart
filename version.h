#ifndef __SPART_VERSION__
#define __SPART_VERSION__

typedef struct {
    unsigned int    major, minor, patch, tweak;
} spart_version_t;

extern const spart_version_t spart_version;

extern const char *spart_version_str;

#endif /* __SPART_VERSION__ */
