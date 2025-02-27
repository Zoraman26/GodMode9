/*
 Written by Wolfvak, specially sublicensed under the GPLv2
 Read LICENSE for more details
*/

#include "common.h"
#include "fsinit.h"
#include "fsutil.h"
#include "qrcodegen.h"
#include "power.h"
#include "rtc.h"
#include "hid.h"
#include "ui.h"
#include "memmap.h"

#include <arm.h>

#define PC_DUMPRAD (0x10)
#define SP_DUMPLEN (0x80)
extern u32 __text_s, __text_e;

static bool sp_dumpable(u32 sp, u32 *sp_lower, u32 *sp_upper)
{
    if ((sp >= __STACK_TOP) || (sp < (__STACK_TOP - __STACK_LEN)))
        return false;
    *sp_lower = sp;
    *sp_upper = min(sp + SP_DUMPLEN, __STACK_TOP);
    return true;
}

static bool pc_dumpable(u32 pc, u32 *pc_lower, u32 *pc_upper)
{
    u32 code_start = (u32)(&__text_s), code_end = (u32)(&__text_e);

    if ((pc >= code_end) || (pc < code_start))
        return false;

    *pc_lower = max(pc - PC_DUMPRAD, code_start);
    *pc_upper = min(pc + PC_DUMPRAD, code_end);
    return true;
}

#define XRQ_DUMPDATAFUNC(type, size) \
static unsigned XRQ_DumpData_##type(char *b, u32 s, u32 e) \
{ \
    char *c = b; \
    while(s<e) { \
        b+=sprintf(b, "%08lX: ",s); \
        type *dl = (type*)s; \
        for (u32 i = 0; i < (16 / sizeof(type)); i++) { \
            b+=sprintf(b, "%0" #size "lX ", (u32)dl[i]); \
        } \
        b+=sprintf(b, "\n"); \
        s+=16; \
    } \
    return (unsigned)(b-c); \
}
XRQ_DUMPDATAFUNC(u8,  2)
XRQ_DUMPDATAFUNC(u16, 4)
XRQ_DUMPDATAFUNC(u32, 8)


static const char *XRQ_Name[] = {
    "Reset", "Undefined", "SWI", "Prefetch Abort",
    "Data Abort", "Reserved", "IRQ", "FIQ"
};


void XRQ_DumpRegisters(u32 xrq, u32 *regs)
{
    u32 sp, sp_lower, sp_upper, pc, pc_lower, pc_upper;
    char dumpstr[2048], *wstr = dumpstr;

    DsTime dstime;
    get_dstime(&dstime);

    /* Dump registers */
    wstr += sprintf(wstr, "Exception: %s (%lu)\n", XRQ_Name[xrq&7], xrq);
    wstr += sprintf(wstr, FLAVOR " " VERSION "\n");
    wstr += sprintf(wstr, "20%02lX-%02lX-%02lX %02lX:%02lX:%02lX\n \n",
        (u32) dstime.bcd_Y, (u32) dstime.bcd_M, (u32) dstime.bcd_D,
        (u32) dstime.bcd_h, (u32) dstime.bcd_m, (u32) dstime.bcd_s);
    for (int i = 0; i < 16; i += 2) {
        wstr += sprintf(wstr,
        "R%02d: %08lX | R%02d: %08lX\n", i, regs[i], i+1, regs[i+1]);
    }
    wstr += sprintf(wstr, "CPSR: %08lX\n\n", regs[16]);

    /* Output registers to main screen */
    u32 draw_width = GetDrawStringWidth(dumpstr);
    u32 draw_height = GetDrawStringHeight(dumpstr);
    u32 draw_x = (SCREEN_WIDTH_MAIN - draw_width) / 2;
    u32 draw_y = (SCREEN_HEIGHT - draw_height) / 2;
    u32 draw_y_upd = draw_y + draw_height - 10;

    ClearScreen(MAIN_SCREEN, COLOR_STD_BG);
    DrawStringF(MAIN_SCREEN, draw_x, draw_y, COLOR_STD_FONT, COLOR_STD_BG, "%s", dumpstr);

    /* Dump STACK */
    sp = regs[13] & ~0xF;
    if (sp_dumpable(sp, &sp_lower, &sp_upper)) {
        wstr += sprintf(wstr, "Stack:\n");
        wstr += XRQ_DumpData_u8(wstr, sp_lower, sp_upper);
        wstr += sprintf(wstr, "\n");
    }

    /* Dump CODE */
    pc = regs[15] & ~0xF;
    if (pc_dumpable(pc, &pc_lower, &pc_upper)) {
        wstr += sprintf(wstr, "Code:\n");
        if (regs[16] & SR_THUMB) { // need to take Thumb code into account
            wstr += XRQ_DumpData_u16(wstr, pc_lower, pc_upper);
        } else {
            wstr += XRQ_DumpData_u32(wstr, pc_lower, pc_upper);
        }
    }

    /* Draw QR Code */
    u8 qrcode[qrcodegen_BUFFER_LEN_MAX];
    u8 temp[qrcodegen_BUFFER_LEN_MAX];
    DrawStringF(MAIN_SCREEN, draw_x, draw_y_upd, COLOR_STD_FONT, COLOR_STD_BG,
        "%-29.29s", "Generating QR code...");
    if (qrcodegen_encodeText(dumpstr, temp, qrcode, qrcodegen_Ecc_LOW,
        qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
        DrawQrCode(ALT_SCREEN, qrcode);
    }

    /* Reinitialize SD */
    DrawStringF(MAIN_SCREEN, draw_x, draw_y_upd, COLOR_STD_FONT, COLOR_STD_BG,
        "%-29.29s", "Reinitializing SD card...");
    while (!InitSDCardFS()) {
        if (InputWait(1) & BUTTON_POWER) PowerOff();
        DeinitSDCardFS();
    }


    /* Dump to SD */
    char path[64];
    snprintf(path, 64, "%s/exception_dump_%02lX%02lX%02lX%02lX%02lX%02lX.txt", OUTPUT_PATH,
        (u32) dstime.bcd_Y, (u32) dstime.bcd_M, (u32) dstime.bcd_D,
        (u32) dstime.bcd_h, (u32) dstime.bcd_m, (u32) dstime.bcd_s);
    DrawStringF(MAIN_SCREEN, draw_x, draw_y_upd, COLOR_STD_FONT, COLOR_STD_BG,
        "%-29.29s", "Dumping state to SD card...");
    FileSetData(path, dumpstr, wstr - dumpstr, 0, true);

    /* Deinit SD */
    DeinitSDCardFS();

    /* Done, wait for user power off */
    DrawStringF(MAIN_SCREEN, draw_x, draw_y_upd, COLOR_STD_FONT, COLOR_STD_BG,
        "%-29.29s", "Press POWER to turn off");
    while (!(InputWait(0) & BUTTON_POWER));
    PowerOff();

    /* We will not return */
    return;
}
