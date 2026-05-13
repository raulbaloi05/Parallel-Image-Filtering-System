#ifndef PROCESSING_H
#define PROCESSING_H

#include <stddef.h>
#include "dataTypes.h"

int process_image(const unsigned char *input, size_t in_size,
                  unsigned char **output, size_t *out_size,
                  const char *filter, ProcessInfo *proc_array);

#endif