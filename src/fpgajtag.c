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

//
// FTDI interface documented at:
//     http://www.ftdichip.com/Documents/AppNotes/AN2232C-01_MPSSE_Cmnd.pdf
// Xilinx Series7 Configuation documented at:
//     ug470_7Series_Config.pdf
// ARM JTAG-DP registers documented at:
//     DDI0314H_coresight_components_trm.pdf
// ARM DPACC/APACC programming documented at:
//     IHI0031C_debug_interface_as.pdf

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include "util.h"
#include "fpga.h"

#define BUFFER_MAX_LEN      1000000
#define FILE_READSIZE          6464
#define MAX_SINGLE_USB_DATA    4046

static int number_of_devices = 1;
static int device_type;
static int found_cortex;
static uint8_t *idle_to_reset = DITEM(IDLE_TO_RESET);
static uint8_t *shift_to_exit1 = DITEM(SHIFT_TO_EXIT1(0, 0));
static int opcode_bits = 4;
static int irreg_extrabit = 0;
static int first_time_idcode_read = 1;

/*
 * Support for GPIOs from Digilent JTAG module to h/w design.
 *
 * SMT1 does not have any external GPIO connections (KC705).
 *
 * SMT2 has GPIO0/1/2 for user use.  In the datasheet for
 * the SMT2, it has an example connecting GPIO2 -> PS_SRST_B
 * on the Zynq-7000. (but the zedboard uses SMT1)
 */
static void pulse_gpio(struct ftdi_context *ftdi, int delay)
{
#define GPIO_DONE            0x10
#define GPIO_01              0x01
#define SET_LSB_DIRECTION(A) SET_BITS_LOW, 0xe0, (0xea | (A))

    write_item(DITEM(SET_LSB_DIRECTION(GPIO_DONE | GPIO_01),
                     SET_LSB_DIRECTION(GPIO_DONE)));
    while(delay > 65536) {
        write_item(DITEM(CLK_BYTES, INT16(65536 - 1)));
        delay -= 65536;
    }
    write_item(DITEM(CLK_BYTES, INT16(delay-1)));
    write_item(DITEM(SET_LSB_DIRECTION(GPIO_DONE | GPIO_01),
                     SET_LSB_DIRECTION(GPIO_01)));
}

static void write_dataw(uint32_t value)
{
    write_item(DITEM(DATAW(0, value)));
}

static void swap32(uint32_t value)
{
    int i;
    union {
        uint32_t i;
        uint8_t c[4];
    } temp;
    temp.i = value;
    for (i = 0; i < 4; i++) {
        uint8_t ch = bitswap[temp.c[3-i]];
        write_data(&ch, 1);
    }
}

static void write_dswap32(uint32_t value)
{
    write_dataw(4);
    swap32(value);
}

static void write_one_word(int dread, int short_format, int value)
{
    if (short_format)
        write_item(DITEM(INT32(value)));
    else
        write_item(DITEM(value, 0x00, 0x00, DATAWBIT | dread, 0x06, 0x00));
}

static void write_eight_bytes(void)
{
    if (found_cortex) {
        write_item(DITEM(DATAW(0, 7), INT32(0)));
        write_one_word(0, 0, 0);
    }
}

static void loaddr(int aread, uint32_t v, int extra3bits)
{
    uint64_t temp = (((uint64_t)v) << 3) | extra3bits;
    write_item(DITEM(IDLE_TO_SHIFT_DR, DATAWBIT, 0x00, 0x00,
                     DATAW(aread, 4), INT32(temp), (DATAWBIT | aread), 0x01, (v>>29) & 0x3f,
                     SHIFT_TO_UPDATE_TO_IDLE(aread,((v>>24) & 0x80))));
}
static void read_rdbuff(void)
{
    loaddr(DREAD, 0, DPACC_RDBUFF | DPACC_WRITE);
}

static void exit1_to_idle(void)
{
    write_item(DITEM(EXIT1_TO_IDLE));
}

static void send_data_frame(struct ftdi_context *ftdi, uint8_t read_param,
    uint8_t *tail, uint8_t *ptrin, int size, int limit_len)
{
    while (size > 0) {
        int rlen = size;
        size -= limit_len;
        if (rlen > limit_len)
            rlen = limit_len;
        int tail_tms = (rlen < limit_len && (tail[1] & MPSSE_WRITE_TMS));
        int tlen = rlen;
        if (tail_tms)
            tlen--;                   // last byte is actually loaded with DATAWBIT command
        write_item(DITEM(read_param, INT16(tlen-1)));
        if (read_param & MPSSE_DO_WRITE)
            write_data(ptrin, rlen);
        ptrin += rlen;
        if (tail_tms) {
            uint8_t *cptr = buffer_current_pop();
            uint8_t ch = *cptr;
            write_item(DITEM(read_param | MPSSE_BITMODE, 6)); // 7 bits of data here
            if (read_param & MPSSE_DO_WRITE)
                write_data(&ch, 1);
            write_item(tail);
            cptr[3] |= (read_param & DREAD); // this is a TMS instruction to shift state
            cptr[5] |= 0x80 & ch; // insert 1 bit of data here
            if (read_param & MPSSE_DO_READ)  // we will be waiting for the result
                write_item(DITEM(SEND_IMMEDIATE));
        }
        flush_write(ftdi, NULL);
    }
}

static void send_data_file(struct ftdi_context *ftdi, int inputfd)
{
    int size, i;
    uint8_t *tailp = DITEM(SHIFT_TO_PAUSE(0,0));
    write_item(DITEM(IDLE_TO_SHIFT_DR));
    write_eight_bytes();
    write_dswap32(0);
    int limit_len = MAX_SINGLE_USB_DATA - buffer_current_size();
    printf("Starting to send file\n");
    do {
        static uint8_t filebuffer[FILE_READSIZE];
        size = read(inputfd, filebuffer, FILE_READSIZE);
        for (i = 0; i < size; i++)
            filebuffer[i] = bitswap[filebuffer[i]];
        if (size < FILE_READSIZE) {
            if (found_cortex)
                tailp = DITEM(EXIT1_TO_IDLE, EXIT1_TO_IDLE, SHIFT_TO_EXIT1(0, 0x80), EXIT1_TO_IDLE);
            else
                tailp = DITEM(SHIFT_TO_EXIT1(0, 0), EXIT1_TO_IDLE);
        }
        send_data_frame(ftdi, DWRITE, tailp, filebuffer, size, limit_len);
        if (size != FILE_READSIZE)
            break;
        write_item(DITEM(PAUSE_TO_SHIFT));
        limit_len = MAX_SINGLE_USB_DATA;
    } while(size == FILE_READSIZE);
    printf("Done sending file\n");
}

/*
 * Functions for setting Instruction Register(IR)
 */
static void write_irreg(int read, int command, int next_state)
{
    //if (trace)
    //    printf("[%s] read %x command %x goto %x\n", __FUNCTION__, read, command, next_state);
    /* send out first part of IR bit pattern */
    write_item(DITEM(IDLE_TO_SHIFT_IR, DATAWBIT | (read), opcode_bits, M(command)));
    if (found_cortex)     /* 3 extra bits of IR are sent here */
        write_item(DITEM(DATAWBIT | read, 0x02,
            M((IRREG_BYPASS<<4) | ((command >> EXTRA_IRREG_BIT_SHIFT) & 0xf))));
    if (next_state == 2)
        write_item(DITEM(SHIFT_TO_UPDATE(0, EXTRA_BIT_ADDITION(command))));
    else if (next_state)
        write_item(DITEM(SHIFT_TO_UPDATE_TO_IDLE((read), EXTRA_BIT_ADDITION(command))));
    else {
        write_item(DITEM(SHIFT_TO_EXIT1((read), EXTRA_BIT_ADDITION(command))));
        exit1_to_idle();
    }
}

static void write_combo_irreg(int linenumber, struct ftdi_context *ftdi, int read, int command, uint32_t expect)
{
    write_item(DITEM(IDLE_TO_SHIFT_IR, DATAWBIT | (read), 4, M(command)));
    if (found_cortex)
        write_item(DITEM(SHIFT_TO_PAUSE(DREAD, EXTRA_BIT_ADDITION(command))));
    else
        write_item(DITEM(SHIFT_TO_EXIT1((read), EXTRA_BIT_ADDITION(command))));
    if (read) {
        uint16_t ret = read_data_int(linenumber, ftdi, 1);
        if (found_cortex) {
            write_item(DITEM(PAUSE_TO_SHIFT));
            write_item(DITEM(DATAWBIT, 0x02, 0xff, SHIFT_TO_EXIT1(0, 0x80)));
        }
        if (ret != expect)
            printf("[%s:%d] mismatch %x\n", __FUNCTION__, linenumber, ret);
    }
    exit1_to_idle();
}

static uint32_t write_bypass(struct ftdi_context *ftdi)
{
    if (found_cortex)
        write_item(DITEM(IDLE_TO_SHIFT_IR, DATAW(DREAD, 1), 0xff,
                  DATARWBIT, 0x00, 0xff, SHIFT_TO_UPDATE_TO_IDLE(DREAD, 0x80)));
    else
        write_irreg(DREAD, EXTEND_EXTRA | IRREG_BYPASS, 1);
    return read_data_int(__LINE__, ftdi, 1 + found_cortex);
}

/*
 * Functions used in testing Cortex core
 */
static void check_read_cortex(int linenumber, struct ftdi_context *ftdi, uint32_t *buf, int load)
{
    int i;
    uint8_t *rdata;
    uint32_t *testp = buf+1;

    if (load)
        write_irreg(0, IRREGA_DPACC, 2);
    loaddr(DREAD, 0, DPACC_CTRL | DPACC_WRITE);
    read_rdbuff();
    rdata = read_data(linenumber, ftdi, buf[0] * 5); /* each item read is 35 bits -> 5 bytes */
    for (i = 0; i < last_read_data_length/6; i++) {
        uint64_t ret = 0;              // Clear out MSB before copy
        memcpy(&ret, rdata, 5);        // copy into bottom of uint64 data target
        if ((ret & 7) != DPACC_RESPONSE_OK)       /* IHI0031C_debug_interface_as.pdf: 3.4.3 */
            printf("fpgajtag:%d Error in cortex response %x \n", linenumber, (int)(ret & 7));
        uint32_t ret32 = ret >> 3;
        if (ret32 != *testp) {
            printf("fpgajtag:%d Error [%ld] act %x expect %x\n", linenumber, testp - buf, ret32, *testp);
            memdump(rdata, 5, "RX");
        }
        testp++;
        rdata += 5;
    }
}

#define DEBUGID_VAL1  0x0310c002   /* DebugID output? */
#define DEBUGID_VAL2  0x03008002   /* DebugID output? */

static void cortex_pair(uint32_t v)
{
    loaddr(0, DEBUG_REGISTER_BASE | v, AP_TAR);
    loaddr(DREAD, 0x0300c002, AP_DRW);     /* ARM instruction: MOVW R12, #2 */
    loaddr(DREAD, 0x0310c002, AP_DRW);
}

static void write_select(int bus)
{
    write_irreg(0, IRREGA_DPACC, 2);
    loaddr(0,      // Coresight: Table 2-11
        bus ? SELECT_DEBUG/*dedicated Debug Bus*/ : 0/*main system bus*/,
        DPACC_SELECT);
    write_irreg(0, IRREGA_APACC, 2);
}

static uint8_t *cortex_reset = DITEM(RESET_TO_IDLE, TMSW, 0x01, 0x00);
static uint8_t *waitreq[2] = {DITEM(RESET_TO_IDLE, TMS_WAIT, TMSW, 0x03, 0x00),
                              DITEM(RESET_TO_IDLE, TMS_WAIT, TMSW, 0x02, 0x00)};
static void cortex_csw(struct ftdi_context *ftdi, int wait, int clear_wait)
{
    uint32_t *cresp[2];
    int i;

    write_irreg(0, IRREGA_ABORT, 2);
    loaddr(0, 1, 0);
    write_irreg(0, IRREGA_DPACC, 2);
    loaddr(0, 0x50000033, DPACC_CTRL);
    // in Debug, 2.3.2: CTRL/STAT, Control/Status register
    //CSYSPWRUPREQ,CDBGPWRUPREQ,STICKYERR,STICKYCMP,STICKYORUN,ORUNDETECT
    if (!clear_wait)
        cresp[0] = (uint32_t[]){2, CORTEX_DEFAULT_STATUS, CORTEX_DEFAULT_STATUS,};
    else {
        write_irreg(0, IRREGA_APACC, 2);
        cortex_pair(0x2000 | DBGDSCRext);
        cresp[0] = (uint32_t[]){5, 0, 0, 0, CORTEX_DEFAULT_STATUS, CORTEX_DEFAULT_STATUS,};
    }
    cresp[1] = (uint32_t[]){3, 0, 0x00800042/*SPIStatus=High*/, CORTEX_DEFAULT_STATUS};
    write_irreg(0, IRREGA_DPACC, 2);
    loaddr(clear_wait?DREAD:0, 0, DPACC_CTRL | DPACC_WRITE);
    for (i = 0; i < 2; i++) {
        if (trace)
            printf("[%s:%d] wait %d i %d\n", __FUNCTION__, __LINE__, wait, i);
        check_read_cortex(__LINE__, ftdi, cresp[i], i);
        write_select(i);
        loaddr(DREAD, 0, AP_CSW | DPACC_WRITE);
        if (wait)
           write_item(waitreq[i]);
    }
    check_read_cortex(__LINE__, ftdi, (uint32_t[]){3, SELECT_DEBUG, DEFAULT_CSW, CORTEX_DEFAULT_STATUS,}, 1);
}

static void tar_read(uint32_t v)
{
    loaddr(DREAD, DEBUG_REGISTER_BASE | v, AP_TAR);
    read_rdbuff();
}
static void tar_write(uint32_t v)
{
    write_irreg(0, IRREGA_APACC, 2);
    loaddr(0, v, AP_TAR);
    read_rdbuff();
}

static void read_csw(struct ftdi_context *ftdi, int wait, uint32_t val3)
{
#define VALC          0x15137030
uint32_t *creturn[] = {(uint32_t[]){10, SELECT_DEBUG, val3,
        VALC, VALC, 1, 1, DEBUGID_VAL1, DEBUGID_VAL1, 0, CORTEX_DEFAULT_STATUS,},
                       (uint32_t[]){12, 0, 0, 0, 0,
        VALC, VALC, 1, 1, DEBUGID_VAL1, DEBUGID_VAL1, 0, CORTEX_DEFAULT_STATUS,}};
int i;
static uint32_t cread[] = {2, 0x80000002};
static uint32_t address_table[] = {ADDRESS_SLCR_ARM_PLL_CTRL, ADDRESS_SLCR_ARM_CLK_CTRL};
uint32_t *cresp[] = {(uint32_t[]){3, 0, DEFAULT_CSW, CORTEX_DEFAULT_STATUS,},
          (uint32_t[]){3, SELECT_DEBUG, DEFAULT_CSW, CORTEX_DEFAULT_STATUS,}};

    for (i = 0; i < 2; i++) {
        write_select(i);
        loaddr(DREAD, cread[i], AP_CSW);
        if (wait)
            write_item(waitreq[i]);
        check_read_cortex(__LINE__, ftdi, cresp[i], 1);
    }
    write_select(0);
#define VAL3          0x1f000200
#define VAL5          0x00028000
    for (i = 0; i < 2; i++) {
        loaddr(DREAD, address_table[i], AP_TAR);
        if (wait)
            write_item(waitreq[0]);
        read_rdbuff();
        if (wait)
            write_item(waitreq[0]);
        else
            write_item(cortex_reset);
    }
    check_read_cortex(__LINE__, ftdi, (uint32_t[]){6, 0, DEFAULT_CSW,
          VAL5, VAL5, VAL3, CORTEX_DEFAULT_STATUS,}, 1);
    if (wait) {
        tar_write(ADDRESS_DEVCFG_MCTRL);
        write_item(cortex_reset);
        check_read_cortex(__LINE__, ftdi, (uint32_t[]){3, VAL3, 0, CORTEX_DEFAULT_STATUS,}, 1);
    }
    write_select(1);
    for (i = 0; i < 2; i++) {
        if (i == 1) {
            write_irreg(0, IRREGA_APACC, 2);
            cortex_pair(DBGDSCRext);
        }
        tar_read((i * 0x2000) | DBGDIDR);
        tar_read((i * 0x2000) | DBGPRSR);
        tar_read((i * 0x2000) | DBGDSCRext);
        tar_read((i * 0x2000) | DBGPCSR);
        check_read_cortex(__LINE__, ftdi, creturn[i], 1);
    }
}

static void cortex_bypass(struct ftdi_context *ftdi, int cortex_nowait)
{
    cortex_csw(ftdi, 1-cortex_nowait, 0);
    if (!cortex_nowait) {
        int count = number_of_devices - 1;
        read_csw(ftdi, 1, 0);
        while (count-- > 0)
            cortex_csw(ftdi, 0, 1);
    }
    read_csw(ftdi, 0, VAL3);
    write_irreg(0, IRREGA_APACC, 2);
    cortex_pair(0x2000 | DBGDSCRext);
    tar_read(DBGPRSR);
    tar_read(DBGDSCRext);
    check_read_cortex(__LINE__, ftdi, (uint32_t[]){8, 0, 0, 0, 0, 1, 1,
            DEBUGID_VAL1, CORTEX_DEFAULT_STATUS,}, 1);
#define VAL6          0xe001b400
    tar_write(DEBUG_REGISTER_BASE | DBGITR);
    check_read_cortex(__LINE__, ftdi, (uint32_t[]){3, DEBUGID_VAL1, VAL6, CORTEX_DEFAULT_STATUS,}, 1);
    tar_write(DEBUG_REGISTER_BASE | 0x2000 | DBGPRSR);
    tar_read(0x2000 | DBGDSCRext);
    check_read_cortex(__LINE__, ftdi, (uint32_t[]){5, VAL6, 1, 1, DEBUGID_VAL1, CORTEX_DEFAULT_STATUS,}, 1);
    tar_write(DEBUG_REGISTER_BASE | 0x2000 | DBGITR);
    check_read_cortex(__LINE__, ftdi, (uint32_t[]){3, DEBUGID_VAL1, VAL6, CORTEX_DEFAULT_STATUS,}, 1);
}

#define REPEAT5(A) INT32((A)), INT32((A)), INT32((A)), INT32((A)), INT32((A))
#define REPEAT10(A) REPEAT5(A), REPEAT5(A)

#define IDCODE_PROBE_PATTERN REPEAT10(0xff), REPEAT5(0xff)
#define PATTERN2 REPEAT10(0xffffffff), REPEAT10(0xffffffff), REPEAT10(0xffffffff), INT32(0xffffffff)

static uint8_t idcode_probe_pattern[] =     {INT32(0xff), IDCODE_PROBE_PATTERN};
static uint8_t idcode_probe_result[] = DITEM(INT32(0xff), IDCODE_PROBE_PATTERN); // filled in with idcode
static uint8_t idcode_validate_pattern[] =     {INT32(0xffffffff),  PATTERN2};
static uint8_t idcode_validate_result[] = DITEM(INT32(0xffffffff), PATTERN2); // filled in with idcode

/*
 * Read/validate IDCODE from device to be programmed
 */
static void check_idcode(int linenumber, struct ftdi_context *ftdi, uint32_t idcode)
{

    write_item(DITEM(TMSW, 0x04, 0x7f/*Reset?????*/, RESET_TO_SHIFT_DR));
    send_data_frame(ftdi, DWRITE | DREAD, DITEM(SHIFT_TO_UPDATE_TO_IDLE(0, 0)),
        idcode_probe_pattern, sizeof(idcode_probe_pattern), 9999);
    uint8_t *rdata = read_data(linenumber, ftdi, idcode_probe_result[0]);
    if (first_time_idcode_read) {    // only setup idcode patterns on first call!
        uint32_t returnedid;
        memcpy(&returnedid, rdata, sizeof(returnedid));
        idcode |= 0xf0000000 & returnedid;
        memcpy(idcode_validate_result+1, rdata, sizeof(returnedid)); // copy returned idcode
        memcpy(idcode_probe_result+1, rdata, sizeof(returnedid));       // copy returned idcode
        if (memcmp(idcode_probe_result+1, rdata, idcode_probe_result[0])) {
            uint32_t anotherid;
            memcpy(&anotherid, rdata+4, sizeof(anotherid));
            printf("[%s] second device idcode found 0x%x\n", __FUNCTION__, anotherid);
            if (anotherid == CORTEX_IDCODE)
                found_cortex = 1;
            memcpy(idcode_probe_result+4+1, rdata+4, sizeof(anotherid));   // copy 2nd idcode
            memcpy(idcode_validate_result+4+1, rdata+4, sizeof(anotherid));   // copy 2nd idcode
            number_of_devices++;
        }
        first_time_idcode_read = 0;
        if (idcode != returnedid) {
            printf("[%s] id %x from file does not match actual id %x\n", __FUNCTION__, idcode, returnedid);
            exit(1);
        }
    }
    if (memcmp(idcode_probe_result+1, rdata, idcode_probe_result[0])) {
        printf("[%s]\n", __FUNCTION__);
        memdump(idcode_probe_result+1, idcode_probe_result[0], "EXPECT");
        memdump(rdata, idcode_probe_result[0], "ACTUAL");
    }
}

static uint32_t fetch_config_word(int linenumber, struct ftdi_context *ftdi, uint32_t irreg, int i)
{
    write_irreg(0, EXTEND_EXTRA | irreg, 1);
    write_item(DITEM(IDLE_TO_SHIFT_DR));
    if (i > 1)
        write_item(DITEM(DATAWBIT, opcode_bits, M(IRREG_JSTART), SHIFT_TO_UPDATE_TO_IDLE(0, 0), IDLE_TO_SHIFT_DR));
    if (i > 0) {
        write_item(DITEM(DATAW(0, 1), 0x69, DATAWBIT, 0x01, 0x00));
        if (found_cortex)
            write_item(DITEM(DATAWBIT, 0x00, 0x00));
    }
    if (found_cortex)
        write_item(DITEM(DATAR(4), SHIFT_TO_UPDATE_TO_IDLE(0, 0)));
    else
        write_item(DITEM(DATAR(3), DATARBIT, 0x06, SHIFT_TO_UPDATE_TO_IDLE(DREAD, 0)));
    return read_data_int(linenumber, ftdi, 4);
}

static void bypass_test(int linenumber, struct ftdi_context *ftdi, int j, int cortex_nowait, int input_shift)
{
    int i;
    uint32_t ret;

    if (trace)
        printf("[%s:%d] start(%d, %d, %d)\n", __FUNCTION__, linenumber, j, cortex_nowait, input_shift);
    if (input_shift)
        write_item(shift_to_exit1);
    else
        write_item(idle_to_reset);
    check_idcode(linenumber, ftdi, 0); // idcode parameter ignored, since this is not the first invocation
    while (j-- > 0) {
        for (i = 0; i < 4; i++) {
            if (trace)
                printf("[%s:%d] j %d i %d\n", __FUNCTION__, linenumber, j, i);
            write_irreg(0, EXTEND_EXTRA | IRREG_BYPASS, 1);
            if ((ret = fetch_config_word(linenumber, ftdi, IRREG_USER2, i)) != 0)
                printf("[%s:%d] nonzero value %x\n", __FUNCTION__, linenumber, ret);
        }
    }
    if (found_cortex)
        cortex_bypass(ftdi, cortex_nowait);
    if (trace)
        printf("[%s:%d] end\n", __FUNCTION__, linenumber);
}

/*
 * Read status register.
 * In ug470_7Series_Config.pdf, see "Accessing Configuration Registers through
 * the JTAG Interface" and Table 6-3.
 */
static void check_status(int linenumber, struct ftdi_context *ftdi, uint32_t expected, int after)
{
    write_item(idle_to_reset);
    if (after)
        write_item(DITEM(IN_RESET_STATE, SHIFT_TO_EXIT1(0, 0)));
    write_item(DITEM(RESET_TO_IDLE));
    write_irreg(0, EXTEND_EXTRA | IRREG_CFG_IN, 1);
    write_item(DITEM(IDLE_TO_SHIFT_DR));
    /*
     * Read Xilinx configuration status register
     * See: ug470_7Series_Config.pdf, Chapter 6
     */
    write_dataw(5 * sizeof(uint32_t) - 1 + found_cortex);
    swap32(CONFIG_DUMMY);
    swap32(CONFIG_SYNC);
    swap32(CONFIG_TYPE2(0));
    swap32(CONFIG_TYPE1(CONFIG_OP_READ, CONFIG_REG_STAT, 1));
    write_one_word(0, found_cortex, 0);
    write_item(DITEM(SHIFT_TO_UPDATE_TO_IDLE(0, 0)));
    uint32_t ret = fetch_config_word(linenumber, ftdi, IRREG_CFG_OUT, 0);
    uint32_t status = ret >> 8;
    if (M(ret) != 0x40 || status != expected)
        printf("[%s:%d] expect %x mismatch %x\n", __FUNCTION__, linenumber, expected, ret);
    printf("STATUS %08x done %x release_done %x eos %x startup_state %x\n", status,
        status & 0x4000, status & 0x2000, status & 0x10, (status >> 18) & 7);
    write_item(idle_to_reset);
}

/*
 * Configuration Register Read Procedure (JTAG), ug470_7Series_Config.pdf,
 * Table 6-4.
 */
static uint32_t read_config_reg(struct ftdi_context *ftdi, uint32_t data)
{
    write_irreg(0, IRREG_CFG_IN, 0);
    write_item(DITEM(IDLE_TO_SHIFT_DR));
    write_dswap32(CONFIG_DUMMY);
    write_eight_bytes();
    write_dswap32(CONFIG_SYNC);
    write_dswap32(CONFIG_TYPE1(CONFIG_OP_NOP, 0,0));
    write_dswap32(CONFIG_TYPE1(CONFIG_OP_READ, data, 1));
    write_dswap32(CONFIG_TYPE1(CONFIG_OP_NOP, 0,0));
    write_dswap32(CONFIG_TYPE1(CONFIG_OP_NOP, 0,0));
    write_dswap32(CONFIG_TYPE1(CONFIG_OP_WRITE, CONFIG_REG_CMD, CONFIG_CMD_WCFG));
    write_dswap32(CONFIG_CMD_DESYNC);
    write_dswap32(CONFIG_TYPE1(CONFIG_OP_NOP, 0,0));
    write_item(DITEM(DATAW(0, sizeof(uint32_t) - 1 +found_cortex)));
    write_one_word(0, found_cortex, 4);
    write_item(DITEM(SHIFT_TO_EXIT1(0, found_cortex ? 0x80 : 0)));
    exit1_to_idle();
    write_irreg(0, IRREG_CFG_OUT, 0);
    write_item(DITEM(IDLE_TO_SHIFT_DR, DATAW(DREAD, sizeof(uint32_t) - 1 )));
    write_one_word(DREAD, 0, 0);
    if (found_cortex)
        write_item(DITEM(SHIFT_TO_PAUSE(DREAD, 0)));
    else
        write_item(DITEM(SHIFT_TO_EXIT1(DREAD, 0)));
    uint64_t ret = read_data_int(__LINE__, ftdi, 4);
    if (found_cortex) {
        exit1_to_idle();
        write_item(DITEM(SHIFT_TO_EXIT1(0, 0x80)));
    }
    exit1_to_idle();
    return ret;
}
static void read_config_memory(struct ftdi_context *ftdi, uint32_t data)
{
    write_irreg(0, IRREG_CFG_IN, 0);
    write_item(DITEM(IDLE_TO_SHIFT_DR));
    write_dswap32(CONFIG_DUMMY);
    write_dswap32(CONFIG_SYNC);
    write_dswap32(CONFIG_TYPE1(CONFIG_OP_NOP, 0,0));
    write_dswap32(CONFIG_TYPE1(CONFIG_OP_WRITE, CONFIG_REG_CMD, 1));
    write_dswap32(CONFIG_CMD_RCRC);
    write_dswap32(CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0));
    write_dswap32(CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0));
    write_item(DITEM(SHIFT_TO_EXIT1(0, 0)));
    exit1_to_idle();
    write_irreg(0, IRREG_JSHUTDOWN, 0);
    write_irreg(0, IRREG_CFG_IN, 0);

    write_item(DITEM(IDLE_TO_SHIFT_DR));
    write_dswap32(CONFIG_DUMMY);
    write_dswap32(CONFIG_SYNC);
    write_dswap32(CONFIG_TYPE1(CONFIG_OP_WRITE,CONFIG_REG_CMD,1));
    write_dswap32(CONFIG_CMD_RCFG);
    write_dswap32(CONFIG_TYPE1(CONFIG_OP_WRITE,CONFIG_REG_FAR,1));
    write_dswap32(0);
    write_dswap32(CONFIG_TYPE1(CONFIG_OP_READ,CONFIG_REG_FDRO,0)); 
    write_dswap32(CONFIG_TYPE2(0x00024090));
    write_dswap32(CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0));
    write_dswap32(CONFIG_TYPE1(CONFIG_OP_NOP, 0, 0));
    write_item(DITEM(SHIFT_TO_EXIT1(0, 0)));
    write_irreg(0, IRREG_CFG_OUT, 0);
    read_data_int(__LINE__, ftdi, 4);
    exit1_to_idle();
}

int main(int argc, char **argv)
{
    logfile = stdout;
    struct ftdi_context *ftdi;
    uint32_t idcode, ret;
    int i = 1, j;
    int inputfd = 0;   /* default input for '-' is stdin */
    const char *serialno = NULL;

    if (argc < 2) {
        printf("%s: [ -s <serialno> ] <filename>\n", argv[0]);
        exit(1);
    }
    if (!strcmp(argv[i], "-s")) {
        serialno = argv[++i];
        i++;
    }
    if (strcmp(argv[i], "-")) {
        inputfd = open(argv[i], O_RDONLY);
        if (inputfd == -1) {
            printf("Unable to open file '%s'\n", argv[i]);
            exit(-1);
        }
    }

    /*
     * Initialize USB, FTDI
     */
    for (i = 0; i < sizeof(bitswap); i++)
        bitswap[i] = BSWAP(i);
    init_usb(serialno);             /*** Initialize USB interface ***/
    ftdi = init_ftdi();             /*** Generic initialization of FTDI chip ***/

    /*
     * Read device id from file to be programmed
     */
    lseek(inputfd, 0x80, SEEK_SET);
    read(inputfd, &idcode, sizeof(idcode));
    idcode = (M(idcode) << 24) | (M(idcode >> 8) << 16) | (M(idcode >> 16) << 8) | M(idcode >> 24);
    lseek(inputfd, 0, SEEK_SET);
    device_type = DEVICE_OTHER;
    if (idcode == 0x03636093)         // ac701
        device_type = DEVICE_AC701;
    if (idcode == 0x03731093)         // zc706
        device_type = DEVICE_ZC706;
    if (idcode == 0x03727093) {       // zc702 and zedboard
        if (usb_bcddevice == 0x700)
            device_type = DEVICE_ZC702;
        else
            device_type = DEVICE_ZEDBOARD;
    }
    printf("[%s] idcode %x device_type %d.\n", __FUNCTION__, idcode, device_type);

    /*
     * Set JTAG clock speed and GPIO pins for our i/f
     */
    write_item(DITEM(LOOPBACK_END, DIS_DIV_5, SET_CLOCK_DIVISOR,
                     SET_BITS_LOW, 0xe8, 0xeb, SET_BITS_HIGH, 0x20, 0x30));
    if (usb_bcddevice == 0x700) /* not a zedboard */
        write_item(DITEM(SET_BITS_HIGH, 0x30, 0x00, SET_BITS_HIGH, 0x00, 0x00));
    flush_write(ftdi, DITEM(FORCE_RETURN_TO_RESET)); /*** Force TAP controller to Reset state ***/
    write_item(shift_to_exit1);

    /*
     * Step 5: Check Device ID
     */
    check_idcode(__LINE__, ftdi, idcode);     /*** Check to see if idcode matches file and detect Zynq ***/
    /*** Depending on the idcode read, change some default actions ***/
    if (found_cortex) {
        opcode_bits = 5;
        irreg_extrabit = EXTRA_BIT_MASK;
    }

    j = 3;
    if (device_type == DEVICE_AC701 || device_type == DEVICE_ZC706 || found_cortex)
        j = 4;
    bypass_test(__LINE__, ftdi, j, 0, 0);
    if (device_type == DEVICE_ZC702) {
        write_item(DITEM(IDLE_TO_RESET));
        write_item(DITEM(IN_RESET_STATE));
        flush_write(ftdi, DITEM(SET_CLOCK_DIVISOR));
    }
    bypass_test(__LINE__, ftdi, 3, 1, device_type == DEVICE_ZC702);
    if (trace)
        printf("[%s:%d]\n", __FUNCTION__, __LINE__);
    flush_write(ftdi, DITEM(IDLE_TO_RESET, IN_RESET_STATE));
    if (device_type != DEVICE_ZC702)
        flush_write(ftdi, DITEM(SET_CLOCK_DIVISOR));
    /*
     * Use a pattern of 0xffffffff to validate that we actually understand all the
     * devices in the JTAG chain.  (this list was set up in check_idcode()
     * on the first call
     */
    write_item(DITEM(SHIFT_TO_EXIT1(0, 0)));
    if (device_type != DEVICE_ZC702) {
        write_item(DITEM(IN_RESET_STATE));
        write_item(DITEM(SHIFT_TO_EXIT1(0, 0)));
    }
    write_item(DITEM(IN_RESET_STATE, RESET_TO_IDLE, IDLE_TO_SHIFT_DR));
    send_data_frame(ftdi, DWRITE | DREAD, DITEM(PAUSE_TO_SHIFT),
        idcode_validate_pattern, sizeof(idcode_validate_pattern), 9999);
    check_read_data(__LINE__, ftdi, idcode_validate_result);
    write_item(DITEM(FORCE_RETURN_TO_RESET, IN_RESET_STATE, RESET_TO_IDLE));
    if (found_cortex)
        write_bypass(ftdi);
    if ((ret = fetch_config_word(__LINE__, ftdi, IRREG_USERCODE, 0)) != 0xffffffff)
        printf("[%s:%d] mismatch %x\n", __FUNCTION__, __LINE__, ret);
    for (i = 0; i < 3; i++) {
        ret = write_bypass(ftdi);
        if (ret == FIRST_TIME)
            printf("fpgajtag: bypass first time %x\n", ret);
        else if (ret == PROGRAMMED)
            printf("fpgajtag: bypass already programmed %x\n", ret);
        else
            printf("fpgajtag: bypass mismatch %x\n", ret);
    }
    check_status(__LINE__, ftdi, 0x301900, 0);
    j = 4;
    if (device_type == DEVICE_AC701)
        j = 3;
    if (device_type == DEVICE_ZC702)
        j = 1;
    for (i = 0; i < j; i++)
        bypass_test(__LINE__, ftdi, 3, 1, (i == 0));
    write_item(DITEM(IDLE_TO_RESET, IN_RESET_STATE, RESET_TO_IDLE));

    /*
     * Step 2: Initialization
     */
    write_irreg(0, IRREG_JPROGRAM, 0);
    write_irreg(0, IRREG_ISC_NOOP, 0);
    pulse_gpio(ftdi, CLOCK_FREQUENCY/80/* 12.5 msec */);
    write_combo_irreg(__LINE__, ftdi, DREAD, IRREG_ISC_NOOP & ~EXTRA_BIT_MASK, INPROGRAMMING);

    /*
     * Step 6: Load Configuration Data Frames
     */
    write_combo_irreg(__LINE__, ftdi, DREAD, IRREG_CFG_IN & ~EXTRA_BIT_MASK, INPROGRAMMING);
    send_data_file(ftdi, inputfd);

    /*
     * Step 8: Startup
     */
    pulse_gpio(ftdi, CLOCK_FREQUENCY/800/*1.25 msec*/);
    if ((ret = read_config_reg(ftdi, CONFIG_REG_BOOTSTS)) != (found_cortex ? 0x03000000 : 0x01000000))
        printf("[%s:%d] mismatch %x\n", __FUNCTION__, __LINE__, ret);
    write_irreg(0, IRREG_BYPASS, 0);
    write_irreg(0, IRREG_JSTART, 0);
    write_item(DITEM(TMSW_DELAY));
    write_combo_irreg(__LINE__, ftdi, DREAD, IRREG_BYPASS, FINISHED);
    if ((ret = read_config_reg(ftdi, CONFIG_REG_STAT)) != (found_cortex ? 0xf87f1046 : 0xfc791040))
        printf("[%s:%d] mismatch %x\n", __FUNCTION__, __LINE__, ret);
    if (found_cortex)
        write_irreg(0, IRREG_BYPASS, 0);
    else {
        write_combo_irreg(__LINE__, ftdi, 0, IRREG_BYPASS, 0);
        flush_write(ftdi, NULL);
    }
    write_item(DITEM(IDLE_TO_RESET, IN_RESET_STATE, RESET_TO_IDLE));
    if ((ret = write_bypass(ftdi)) != PROGRAMMED)
        printf("[%s:%d] mismatch %x\n", __FUNCTION__, __LINE__, ret);
    if (device_type != DEVICE_ZEDBOARD)
        bypass_test(__LINE__, ftdi, 3, 1, 0);
    check_status(__LINE__, ftdi, 0xf07910, 1);
    if (device_type == DEVICE_AC701 || device_type == DEVICE_ZC706 || found_cortex)
        bypass_test(__LINE__, ftdi, 3, 1, 1);
    if (device_type == DEVICE_ZEDBOARD)
        bypass_test(__LINE__, ftdi, 3, 1, 0);

    /*
     * Cleanup and free USB device
     */
    close_usb(ftdi);
    execlp("/usr/local/bin/pciescanportal", "arg", (char *)NULL); /* rescan pci bus to discover device */
    return 0;
}
