// Copyright (c) 2014 Quanta Research Cambridge, Inc.
// Original author: John Ankcorn

// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use, copy,
// modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
// BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifdef USE_LIBFTDI
#include "ftdi.h"
#else
#define MPSSE_WRITE_NEG 0x01   /* Write TDI/DO on negative TCK/SK edge*/
#define MPSSE_BITMODE   0x02   /* Write bits, not bytes */
#define MPSSE_READ_NEG  0x04   /* Sample TDO/DI on negative TCK/SK edge */
#define MPSSE_LSB       0x08   /* LSB first */
#define MPSSE_DO_WRITE  0x10   /* Write TDI/DO */
#define MPSSE_DO_READ   0x20   /* Read TDO/DI */
#define MPSSE_WRITE_TMS 0x40   /* Write TMS/CS */
#define SET_BITS_LOW    0x80
#define SET_BITS_HIGH   0x82
#define LOOPBACK_END    0x85
#define TCK_DIVISOR     0x86
#define DIS_DIV_5       0x8a
#define CLK_BYTES       0x8f
#define SEND_IMMEDIATE  0x87
struct ftdi_context;
#endif

extern FILE *logfile;
extern int usb_bcddevice;
extern uint8_t bitswap[256];
extern int last_read_data_length;
extern int trace;

void memdump(const uint8_t *p, int len, char *title);

void init_usb(const char *serialno);
void close_usb(struct ftdi_context *ftdi);
struct ftdi_context *init_ftdi(void);

void write_data(uint8_t *buf, int size);
void write_item(uint8_t *buf);
void flush_write(struct ftdi_context *ftdi, uint8_t *req);
int buffer_current_size(void);

uint8_t *read_data(int linenumber, struct ftdi_context *ftdi, int size);
uint64_t read_data_int(int linenumber, struct ftdi_context *ftdi, int size);
uint8_t *check_read_data(int linenumber, struct ftdi_context *ftdi, uint8_t *buf);