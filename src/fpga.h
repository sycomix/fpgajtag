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

#define DITEM(...) ((uint8_t[]){sizeof((uint8_t[]){ __VA_ARGS__ }), __VA_ARGS__})
#define M(A)               ((A) & 0xff)
#define INT16(A)           M(A), M((A) >> 8)
#define INT32(A)           INT16(A), INT16((A) >> 16)
#define BSWAP(A) ((((A) & 1) << 7) | (((A) & 2) << 5) | (((A) & 4) << 3) | (((A) & 8) << 1) \
         | (((A) & 0x10) >> 1) | (((A) & 0x20) >> 3) | (((A) & 0x40) >> 5) | (((A) & 0x80) >> 7))

/*
 * FTDI constants
 */
#define MWRITE    (MPSSE_LSB|MPSSE_WRITE_NEG)
#define DWRITE    (MPSSE_DO_WRITE | MWRITE)
#define DREAD     (MPSSE_DO_READ  | MPSSE_LSB|MPSSE_READ_NEG)

#define TMSW      (MPSSE_WRITE_TMS      |MWRITE|MPSSE_BITMODE)//4b
#define TMSRW     (MPSSE_WRITE_TMS|DREAD|MWRITE|MPSSE_BITMODE)//6f

#define DATAWBIT  (DWRITE|MPSSE_BITMODE)       //1b
#define DATARBIT  (DREAD |MPSSE_BITMODE)       //2e
#define DATARWBIT (DREAD |DWRITE|MPSSE_BITMODE)//3f
#define DATAW(READA, A)    (DWRITE|(READA)), INT16((A)-1) //(0)->19 (DREAD)->3d
#define DATAR(A)           DREAD, INT16((A)-1) //2c

#define IDLE_TO_SHIFT_IR   TMSW, 0x03, 0x03  /* Idle -> Shift-IR */
#define IDLE_TO_SHIFT_DR   TMSW, 0x02, 0x01  /* Idle -> Shift-DR */
#define EXIT1_TO_IDLE      TMSW, 0x01, 0x01  /* Exit1/Exit2 -> Idle */
#define IDLE_TO_RESET      TMSW, 0x02, 0x07  /* Idle -> Reset */
#define RESET_TO_IDLE      TMSW, 0x00, 0x00  /* Reset -> Idle */
#define IN_RESET_STATE     TMSW, 0x00, 0x7f  /* Marker for Reset */
#define PAUSE_TO_SHIFT     TMSW, 0x01, 0x01 /* Pause-DR -> Shift-DR */
#define FORCE_RETURN_TO_RESET TMSW, 0x04, 0x1f /* go back to TMS reset state */
#define RESET_TO_SHIFT_DR     TMSW, 0x03, 0x02  /* Reset -> Shift-DR */

#define TMS_WAIT           TMSW, 0x06, 0x00, TMSW, 0x06, 0x00, TMSW, 0x06, 0x00
#define TMSW_DELAY                                             \
         RESET_TO_IDLE,  /* Hang out in Idle for a while */ \
         TMS_WAIT, TMS_WAIT, TMS_WAIT, TMS_WAIT, \
         TMSW, 0x06, 0x00, TMSW, 0x06, 0x00, TMSW, 0x01, 0x00

#define SHIFT_TO_EXIT1(READA, A) \
     TMSW | (READA), 0x00, ((A) | 0x01)             /* Shift-IR -> Exit1-IR */
#define SHIFT_TO_PAUSE(READA, A) \
     TMSW | (READA), 0x01, ((A) | 0x01)             /* Shift-IR -> Pause-IR */
#define SHIFT_TO_UPDATE(READA, A) \
     TMSW | (READA), 0x01, ((A) | 0x03)             /* Shift-DR -> Update-DR */
#define SHIFT_TO_UPDATE_TO_IDLE(READA, A) \
     TMSW | (READA), 0x02, ((A) | 0x03)    /* Shift-DR -> Update-DR -> Idle */

#define EXTEND_EXTRA            0xc0
#define EXTRA_BIT_SHIFT         12
#define EXTRA_IRREG_BIT_SHIFT    8
#define EXTRA_BIT_MASK          (1<<EXTRA_BIT_SHIFT)
#define EXTRA_BIT_ADDITION(A)   (((A) >> (EXTRA_BIT_SHIFT - 7)) & 0x80)

/*
 * Xilinx constants
 */
//#define CLOCK_FREQUENCY      15000000
#define CLOCK_FREQUENCY      30000000
#define SET_CLOCK_DIVISOR    TCK_DIVISOR, INT16(30000000/CLOCK_FREQUENCY - 1)

#define COMBINE_IR_REG(FPGAREG, CORTEXREG) \
     (((CORTEXREG) << EXTRA_IRREG_BIT_SHIFT) | (irreg_extrabit | (FPGAREG)))

/* FPGA JTAG registers */
#define IRREG_USER2          COMBINE_IR_REG(0x03, 0xf)
#define IRREG_CFG_OUT        COMBINE_IR_REG(0x04, 0xf)
#define IRREG_CFG_IN         COMBINE_IR_REG(0x05, 0xf)
#define IRREG_USERCODE       COMBINE_IR_REG(0x08, 0xf)
#define IRREG_JPROGRAM       COMBINE_IR_REG(0x0b, 0xf)
#define IRREG_JSTART         COMBINE_IR_REG(0x0c, 0xf)
#define IRREG_ISC_NOOP       COMBINE_IR_REG(0x14, 0xf)
#define IRREG_BYPASS         COMBINE_IR_REG((EXTRA_BIT_MASK | 0x3f), 0xf) // even on PCIE, this has an extra bit

/* Status values */
#define FIRST_TIME    (found_cortex ? 0x8a : 0x20)
#define INPROGRAMMING (found_cortex ? 0x10 : 0x88)
#define PROGRAMMED    (found_cortex ? 0xae : 0xbc)
#define FINISHED      (found_cortex ? 0x5c : 0xac)

/*
 * Xilinx Configuration Packets
 *
 * In ug470_7Series_Config.pdf, this is described on pages 89ff.
 */

// Type 1 Packet, Table 5-17
#define CONFIG_TYPE1_TAG   0x20000000
#define CONFIG_TYPE1(OPCODE,REG,COUNT) \
    (CONFIG_TYPE1_TAG | ((OPCODE) << 27) | ((REG) << 13) | (COUNT))

// Type 1 OPCODE Format, Table 5-18
#define CONFIG_OP_NOP         0
#define CONFIG_OP_READ        1
#define CONFIG_OP_WRITE       2

// Type 1 Packet Registers, Table 5-20
#define CONFIG_REG_CRC     0x00
#define CONFIG_REG_FAR     0x01
#define CONFIG_REG_FDRI    0x02
#define CONFIG_REG_FDRO    0x03
#define CONFIG_REG_CMD     0x04  // CMD register, Table 5-22
#define     CONFIG_CMD_NULL     0x00
#define     CONFIG_CMD_WCFG     0x01
#define     CONFIG_CMD_MFW      0x02
#define     CONFIG_CMD_DGHIGH   0x03
#define     CONFIG_CMD_RCFG     0x04
#define     CONFIG_CMD_START    0x05
#define     CONFIG_CMD_RCAP     0x06
#define     CONFIG_CMD_RCRC     0x07
#define     CONFIG_CMD_AGHIGH   0x08
#define     CONFIG_CMD_SWITCH   0x09
#define     CONFIG_CMD_GRESTORE 0x0a
#define     CONFIG_CMD_SHUTDOWN 0x0b
#define     CONFIG_CMD_GCAPTURE 0x0c
#define     CONFIG_CMD_DESYNC   0x0d  // end of configuration procedure
#define     CONFIG_CMD_IPROG    0x0f
#define     CONFIG_CMD_CRCC     0x10
#define     CONFIG_CMD_LTIMER   0x11
#define CONFIG_REG_CTL0    0x05
#define CONFIG_REG_MASK    0x06
#define CONFIG_REG_STAT    0x07  // STAT register, Table 5-25
#define CONFIG_REG_LOUT    0x08
#define CONFIG_REG_COR0    0x09
#define CONFIG_REG_MFWR    0x0a
#define CONFIG_REG_CBC     0x0b
#define CONFIG_REG_IDCODE  0x0c
#define CONFIG_REG_AXSS    0x0d
#define CONFIG_REG_COR1    0x0e
#define CONFIG_REG_WBSTAR  0x10
#define CONFIG_REG_TIMER   0x11
#define CONFIG_REG_BOOTSTS 0x16  // BOOTSTS register, Table 5-35
#define CONFIG_REG_CTL1    0x18

// Type 2 Packet (must follow a Type 1 packet and is used for long blocks)
//
#define CONFIG_TYPE2_TAG   0x40000000
#define CONFIG_TYPE2(LEN) (CONFIG_TYPE2_TAG | (LEN))

// Constants used in accessing Configuration Registers
#define CONFIG_DUMMY           0xffffffff
#define CONFIG_SYNC            0xaa995566

/*
 * ARM Cortex constants
 * IHI0031C_debug_interface_as.pdf, Figure 7-1
 */
#define CORTEX_IDCODE 0x4ba00477

/* ARM JTAG-DP registers */
#define IRREGA_ABORT         COMBINE_IR_REG(0xff, 0x8)   /* 35 bit register */
#define IRREGA_DPACC         COMBINE_IR_REG(0xff, 0xa)   /* Debug Port access, 35 bit register */
#define IRREGA_APACC         COMBINE_IR_REG(0xff, 0xb)   /* Access Port access, 35 bit register */
    #define AP_CSW           0                           /* MEM-AP registers */
    #define AP_TAR           2
    #define AP_DRW           6
#define IRREGA_IDCODE        COMBINE_IR_REG(0xff, 0xe)   /* 32 bit register */
#define IRREGA_BYPASS        COMBINE_IR_REG(0xff, 0xf)

/* Cortex request extra 3 bit field */
/* 2 bits of register selector */
#define DPACC_CTRL     (1 << 1)
    // Coresight: Figure 2-14
    #define CORTEX_DEFAULT_STATUS 0xf0000001
    // CSYSPWRUPACK,CSYSPWRUPREQ,CDBGPWRUPACK,CDBGPWRUPREQ,ORUNDETECT
#define DPACC_SELECT   (2 << 1)
#define DPACC_RDBUFF   (3 << 1)
/* 1 bit of Write/nRead */
#define DPACC_WRITE        0x1

/* Cortex response extra 3 bit field */
#define DPACC_RESPONSE_OK 0x2

#define DEFAULT_CSW   0xe0000042
                      // Coresight: Table 2-20
                      // DbgStatus=1 -> AHB transfers permitted
                      // Size=2      -> 32 bits
#define SELECT_DEBUG  0x01000000

/* MEM-AP accessable registers */
// From DDI0388I_cortex_a9_r4p1_trm.pdf, Table 10-1
// Detailed description of each register is also in:
//   DDI0406B_arm_architecture_reference_manual_errata_markup_10_0.pdf (but no table!!)
#define DEBUG_REGISTER_BASE 0x80090000

#define DBGDIDR    0x000  /* ARMARM, C10.2.1: Debug ID */
#define DBGPRSR    0x314  /* ARMARM, C10.3.5: Device Power-down and Reset Status */
#define DBGITR     0x084  /* ARMARM, C10.4.3: Instruction Transfer */
#define DBGDSCRext 0x088  /* ARMARM, C10.3.1: Debug Status and Control */
#define DBGPCSR    0x028  /* ARMARM, C10.3.6: Program Counter Sampling */

/*
 * Zynq constants
 * (Addresses from ug585-Zynq-7000-TRM.pdf)
 */
#define ADDRESS_DEVCFG_MCTRL      0xf8007080
#define ADDRESS_SLCR_ARM_PLL_CTRL 0xf8000100
#define ADDRESS_SLCR_ARM_CLK_CTRL 0xf8000120
enum {DEVICE_OTHER, DEVICE_AC701, DEVICE_ZC706, DEVICE_ZC702, DEVICE_ZEDBOARD};

#if 0
DUMMY    : ffffffff
DUMMY    : ffffffff
DUMMY    : ffffffff
DUMMY    : ffffffff
DUMMY    : ffffffff
DUMMY    : ffffffff
DUMMY    : ffffffff
DUMMY    : ffffffff
WIDTHSYNC: 000000bb
WIDTH    : 11220044
DUMMY    : ffffffff
DUMMY    : ffffffff
SYNC     : aa995566
TYPE1    : opcode write  TIMER   : 00000000
TYPE1    : opcode write  WBSTAR  : 00000000
TYPE1    : opcode write  CMD NULL
TYPE1    : opcode write  CMD RCRC
TYPE1    : opcode write  UNK_13  : 00000000
TYPE1    : opcode write  COR0    : 02003fe5
TYPE1    : opcode write  COR1    : 00000000
TYPE1    : opcode write  IDCODE  : 03727093
TYPE1    : opcode write  CMD SWITCH
TYPE1    : opcode write  MASK    : 00000401
TYPE1    : opcode write  CTL0    : 00000501
TYPE1    : opcode write  MASK    : 00000000
TYPE1    : opcode write  CTL1    : 00000000
TYPE1    : opcode write  FAR     type 0 top 0 row 0 col 0 minor 0
TYPE1    : opcode write  CMD WCFG
TYPE1    : opcode write  FDRI   
TYPE2    : wordcnt 000f6c78
#endif