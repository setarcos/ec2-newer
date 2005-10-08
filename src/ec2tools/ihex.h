#ifndef _IHEX_H
#define _IHEX_H

#include <stdint.h>

void ihex_load_file( char *filename, char *memory, uint16_t *start, uint16_t *end );

void ihex_save_file( char *filename, char *memory, uint16_t start_addr, uint16_t len );
void ihex_hexout( FILE *fhex, int byte, int memory_location, int end );

#endif
