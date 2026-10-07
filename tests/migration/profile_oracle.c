/* Batch oracle for Python/C diagnostic-predicate equivalence. No hardware I/O. */
#include <stdio.h>
#include <stdint.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include "controller_diagnostic.h"

static uint32_t little_word(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(void) {
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) < 0) return 2;
#endif
    unsigned char record[259 * 4];
    size_t count;
    while ((count = fread(record, 1, sizeof(record), stdin)) != 0) {
        if (count != sizeof(record)) return 3;
        uint32_t profile = little_word(record);
        uint32_t initial = little_word(record + 4);
        uint32_t generation = little_word(record + 8);
        uint32_t words[256];
        for (unsigned i = 0; i < 256; ++i) words[i] = little_word(record + 12 + i * 4);
        bool valid;
        if (profile == 1)
            valid = initial ? app_diagnostic_initial(words) : app_diagnostic_registers(words, generation);
        else if (profile == 2)
            valid = initial ? app_low_diagnostic_initial(words) : app_low_diagnostic_registers(words, generation);
        else return 4;
        if (putchar(valid ? '1' : '0') == EOF) return 5;
    }
    return ferror(stdin) || fflush(stdout) ? 6 : 0;
}
