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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <libusb.h>
#include "util.h"

// for using libftdi.so
//#define USE_LIBFTDI

#define USB_TIMEOUT     5000
#define ENDPOINT_IN     0x02
#define ENDPOINT_OUT    0x81
#define USB_CHUNKSIZE   4096
#define USB_INDEX          0

#define USBSIO_RESET                     0 /* Reset the port */
#define USBSIO_RESET_PURGE_RX            1
#define USBSIO_RESET_PURGE_TX            2
#define USBSIO_SET_BAUD_RATE             3 /* Set baud rate */
#define USBSIO_SET_LATENCY_TIMER_REQUEST 9
#define USBSIO_SET_BITMODE_REQUEST       11
#define MAX_ITEM_LENGTH 2000

static int logall = 1;
static int datafile_fd = -1;
static void openlogfile(void);

#include "dumpdata.h"

FILE *logfile;
int found_232H;
uint8_t bitswap[256];
int last_read_data_length;
int trace;

#if defined(USE_LOGGING)
static int logging = 1;
#else
static int logging;
#endif
static libusb_device_handle *usbhandle = NULL;
static struct libusb_context *usb_context;
static uint8_t usbreadbuffer[USB_CHUNKSIZE];
static uint8_t *usbreadbuffer_ptr = usbreadbuffer;
static int read_size[MAX_ITEM_LENGTH];
static int read_size_ptr;

static void openlogfile(void)
{
    if (!logfile)
        logfile = fopen("/tmp/xx.logfile2", "w");
    if (datafile_fd < 0)
        datafile_fd = creat("/tmp/xx.datafile2", 0666);
}

void memdump(const uint8_t *p, int len, char *title)
{
int i;

    i = 0;
    while (len > 0) {
        if (title && !(i & 0xf)) {
            if (i > 0)
                printf("\n");
            printf("%s: ",title);
        }
        printf("0x%02x, ", *p++);
        i++;
        len--;
    }
    if (title)
        printf("\n");
}

/*
 * USB interface
 */
void init_usb(const char *serialno)
{
    int cfg, type = 0, i = 0, baudrate = 9600;
    static const char frac_code[8] = {0, 3, 2, 4, 1, 5, 6, 7};
    int best_divisor = 12000000*8 / baudrate;
    unsigned long encdiv = (best_divisor >> 3) | (frac_code[best_divisor & 0x7] << 14);
    libusb_device **device_list, *dev, *usbdev = NULL;
    struct libusb_device_descriptor desc;
    struct libusb_config_descriptor *config_descrip;

    /*
     * Locate USB interface for JTAG
     */
    if (libusb_init(&usb_context) < 0
     || libusb_get_device_list(usb_context, &device_list) < 0) {
        printf("libusb_init failed\n");
        exit(-1);
    }
    while ((dev = device_list[i++]) ) {
        if (libusb_get_device_descriptor(dev, &desc) < 0)
            break;
        if ( desc.idVendor == 0x403 && (desc.idProduct == 0x6001 || desc.idProduct == 0x6010
         || desc.idProduct == 0x6011 || desc.idProduct == 0x6014)) {
            unsigned char serial[64], manuf[64], descrip[128];
            libusb_ref_device(dev);
            if (libusb_open(dev, &usbhandle) < 0
             || libusb_get_string_descriptor_ascii(usbhandle, desc.iManufacturer, manuf, sizeof(manuf)) < 0
             || libusb_get_string_descriptor_ascii(usbhandle, desc.iProduct, descrip, sizeof(descrip)) < 0
             || libusb_get_string_descriptor_ascii(usbhandle, desc.iSerialNumber, serial, sizeof(serial)) < 0)
                goto error;
            printf("[%s] %s:%s:%s\n", __FUNCTION__, manuf, descrip, serial);
            if (!serialno || !strcmp(serialno, (char *)serial)) {
                usbdev = dev;
                break;
            }
            libusb_close (usbhandle);
        }
    }
    libusb_free_device_list(device_list,1);
    if (!usbdev || libusb_get_config_descriptor(usbdev, 0, &config_descrip) < 0)
        goto error;
    int configv = config_descrip->bConfigurationValue;
    libusb_free_config_descriptor (config_descrip);
    libusb_detach_kernel_driver(usbhandle, 0);
#define USBCTRL(A,B,C) \
     libusb_control_transfer(usbhandle, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE \
           | LIBUSB_ENDPOINT_OUT, (A), (B), (C) | USB_INDEX, NULL, 0, USB_TIMEOUT)

    if (libusb_get_configuration (usbhandle, &cfg) < 0
     || (desc.bNumConfigurations > 0 && cfg != configv && libusb_set_configuration(usbhandle, configv) < 0)
     || libusb_claim_interface(usbhandle, 0) < 0
     || USBCTRL(USBSIO_RESET, USBSIO_RESET, 0) < 0
     || USBCTRL(USBSIO_SET_BAUD_RATE, (encdiv | 0x20000) & 0xFFFF, ((encdiv >> 8) & 0xFF00)) < 0
     || USBCTRL(USBSIO_SET_LATENCY_TIMER_REQUEST, 255, 0) < 0
     || USBCTRL(USBSIO_SET_BITMODE_REQUEST, 0, 0) < 0
     || USBCTRL(USBSIO_SET_BITMODE_REQUEST, 2 << 8, 0) < 0
     || USBCTRL(USBSIO_RESET, USBSIO_RESET_PURGE_RX, 0) < 0
     || USBCTRL(USBSIO_RESET, USBSIO_RESET_PURGE_TX, 0) < 0)
        goto error;
    //(desc.bcdDevice == 0x700) //kc       TYPE_2232H
    printf("[%s:%d] bcd %x type %d\n", __FUNCTION__, __LINE__, desc.bcdDevice, type);
    if (desc.bcdDevice == 0x900) //zedboard TYPE_232H
        found_232H = 1;
    return;
error:
    printf("Can't find usable usb interface\n");
    exit(-1);
}

void close_usb(struct ftdi_context *ftdi)
{
#ifdef USE_LIBFTDI
    ftdi_deinit(ftdi);
#else
    libusb_close (usbhandle);
    libusb_exit(usb_context);
#endif
    fflush(stdout);
    fclose(logfile);
    close(datafile_fd);
}

#ifndef USE_LIBFTDI
int ftdi_write_data(struct ftdi_context *ftdi, const unsigned char *buf, int size)
{
    int actual_length;
    if (logging)
        formatwrite(1, buf, size, "WRITE");
    if (libusb_bulk_transfer(usbhandle, ENDPOINT_IN, (unsigned char *)buf, size, &actual_length, USB_TIMEOUT) < 0)
        printf( "usb bulk write failed");
    return actual_length;
}
int ftdi_read_data(struct ftdi_context *ftdi, unsigned char *buf, int size)
{
    int actual_length = 1;
    do {
        int ret = libusb_bulk_transfer (usbhandle, ENDPOINT_OUT, usbreadbuffer, USB_CHUNKSIZE, &actual_length, USB_TIMEOUT);
        if (ret < 0)
            printf( "usb bulk read failed");
        actual_length -= 2;
    } while (actual_length == 0);
    memcpy (buf, usbreadbuffer+2, actual_length);
    if (actual_length != size) {
        printf("[%s:%d] bozo actual_length %d size %d\n", __FUNCTION__, __LINE__, actual_length, size);
        exit(-1);
        }
    if (logging)
        memdumpfile(buf, actual_length, "READ");
    return actual_length;
}
#endif //end if not USE_LIBFTDI

/*
 * FTDI generic initialization
 */
struct ftdi_context *init_ftdi(void)
{
    static uint8_t illegal_command[] = { 0xaa, SEND_IMMEDIATE };
    static uint8_t command_ab[] = { 0xab, SEND_IMMEDIATE };
    static uint8_t errorcode_aa[] = { 0xfa, 0xaa };
    static uint8_t errorcode_ab[] = { 0xfa, 0xab };
    struct ftdi_context *ftdi = NULL;
    int i;
    uint8_t retcode[2];

#ifdef USE_LIBFTDI
    ftdi = ftdi_new();
    ftdi_set_usbdev(ftdi, usbhandle);
    ftdi->usb_ctx = usb_context;
    ftdi->max_packet_size = 512; //5000;
#endif
    /*
     * Generic command synchronization with ftdi chip
     */
    for (i = 0; i < 4; i++) {
        ftdi_write_data(ftdi, illegal_command, sizeof(illegal_command));
        ftdi_read_data(ftdi, retcode, sizeof(retcode));
        if (memcmp(retcode, errorcode_aa, sizeof(errorcode_aa)))
            memdump(retcode, sizeof(retcode), "RETaa");
    }
    ftdi_write_data(ftdi, command_ab, sizeof(command_ab));
    ftdi_read_data(ftdi, retcode, sizeof(retcode));
    if (memcmp(retcode, errorcode_ab, sizeof(errorcode_ab)))
        memdump(retcode, sizeof(retcode), "RETab");
    return ftdi;
}

/*
 * Write utility functions
 */
void write_data(uint8_t *buf, int size)
{
    memcpy(usbreadbuffer_ptr, buf, size);
    usbreadbuffer_ptr += size;
}

void write_item(uint8_t *buf)
{
    write_data(buf+1, buf[0]);
}

int buffer_current_size(void)
{
    return usbreadbuffer_ptr - usbreadbuffer;
}

void flush_write(struct ftdi_context *ftdi, uint8_t *req)
{
    if (req)
        write_item(req);
    int write_length = buffer_current_size();
    usbreadbuffer_ptr = usbreadbuffer;
    if (!write_length)
        return;
    ftdi_write_data(ftdi, usbreadbuffer, write_length);
    read_size_ptr = 0;

    const uint8_t *p = usbreadbuffer;
    while (write_length > 0) {
        int plen = 1;
        uint8_t ch = *p;
        unsigned tlen = (p[2] << 8 | p[1]) + 1;
        switch(ch) {
        case 0x85: case 0x87: case 0x8a: case 0xaa: case 0xab:
            break;
        case 0x2e:
            plen = 2;
            break;
        case 0x19: case 0x1b: case 0x2c: case 0x3d: case 0x3f: case 0x4b:
        case 0x6f: case 0x80: case 0x82: case 0x86: case 0x8f:
            plen = 3;
            break;
        default:
            memdump(p-1, write_length, "FOO");
            exit(-1);
        }
        if (ch & MPSSE_DO_READ) {
            if (ch & MPSSE_BITMODE) {
                int bitsize = *(p+1)+1;
                if (ch & MPSSE_WRITE_TMS)
                    bitsize = 1;
                read_size[read_size_ptr] = -bitsize; /* number of bits */
            }
            else if (ch == 0x2c || ch == 0x3d)       /* DATAR or DATARW */
                read_size[read_size_ptr] = tlen;     /* number of bytes */
            else
                read_size[read_size_ptr] = *(p+1);   /* number of bytes */
            read_size_ptr++;
        }
        p += plen;
        write_length -= plen;
        if (ch == 0x19 || ch == 0x3d) {
            p += tlen;
            write_length -= tlen;
        }
    }
}

/*
 * Read utility functions
 */
uint8_t *read_data(int linenumber, struct ftdi_context *ftdi, int size)
{
    static uint8_t last_read_data[10000];
    int i, j, expected_len = 0, extra_bytes = 0;
    flush_write(ftdi, NULL);
    last_read_data_length = 0;
    for (i = 0; i < read_size_ptr; i++) {
//printf("[%s:%d] %d\n", __FUNCTION__, linenumber, read_size[i]);
        if (read_size[i] > 0)
            expected_len += read_size[i];
        else {
            if (i == 0 || read_size[i-1] > 0)
                extra_bytes++;
            expected_len++; /* we will squeeze out partial bytes in the processing below */
        }
    }
    if (expected_len - extra_bytes != size) {
printf("[%s:%d] expected len %d.=0x%x extra %d size %d\n", __FUNCTION__, linenumber, expected_len, expected_len, extra_bytes, size);
        //exit(-1);
    }
    if (size) {
        last_read_data_length = ftdi_read_data(ftdi, last_read_data, expected_len);
        uint8_t *p = last_read_data;
        int validbits = 0;
        for (i = 0; i < read_size_ptr; i++) {
            if (read_size[i] < 0) {
                validbits -= read_size[i];
                if (validbits < 0 || validbits > 8) {
                    printf("[%s:%d] validbits %d\n", __FUNCTION__, __LINE__, validbits);
                    exit(-1);
                }
                *p &= (0xff << (8-validbits));
                if (i > 0 && read_size[i-1] < 0) {
                    *(p-1) = *p;
                    /* delete unused byte from read result */
                    last_read_data_length--;
                    for (j = 0; j < size; j++)  /* copies too much, but... */
                        *(p+j) = *(p+j+1);  /* move the data down in the buffer 1 byte */
                }
                else
                    p++;
            }
            else {
                p += read_size[i];
                validbits = 0;
            }
        }
    }
    return last_read_data;
}

uint64_t read_data_int(int linenumber, struct ftdi_context *ftdi, int size)
{
    uint8_t *bufp = read_data(linenumber, ftdi, size);
    uint64_t ret = 0;
    uint8_t *backp = bufp + size;
    while (backp > bufp)
        ret = (ret << 8) | bitswap[*--backp];  //each byte is bitswapped
    return ret;
}

uint8_t *check_read_data(int linenumber, struct ftdi_context *ftdi, uint8_t *buf)
{
    if (trace)
        printf("[%s:%d]\n", __FUNCTION__, linenumber);
    uint8_t *rdata = read_data(linenumber, ftdi, buf[0]);
    if (last_read_data_length != buf[0] || memcmp(buf+1, rdata, buf[0])) {
        printf("[%s] mismatch on line %d\n", __FUNCTION__, linenumber);
        memdump(buf+1, buf[0], "EXPECT");
        memdump(rdata, last_read_data_length, "ACTUAL");
    }
    return rdata;
}
