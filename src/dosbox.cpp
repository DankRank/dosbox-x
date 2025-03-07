/*
 *  Copyright (C) 2002-2019  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA.
 */

/* NTS: Valgrind hunting shows memory leak from C++ new operator somewhere
 *      with the JACK library indirectly invoked by SDL audio. Can we resolve
 *      that too eventually? */

/* NTS: Valgrind hunting also shows one of the section INIT functions (I can't
 *      yet tell which one because the stack trace doesn't show it) is allocating
 *      something and is not freeing it. */

/* NTS: Valgrind hunting has a moderate to high signal-to-noise ratio because
 *      of memory leaks (lazy memory allocation) from other libraries in the
 *      system, including:
 *
 *         ncurses
 *         libSDL
 *         libX11 and libXCB
 *         libasound (ALSA sound library)
 *         PulseAudio library calls
 *         JACK library calls
 *         libdl (the dlopen/dlclose functions allocate something and never free it)
 *         and a whole bunch of unidentified malloc calls without a matching free.
 *
 *      On my dev system, a reported leak of 450KB (77KB possibly lost + 384KB still reachable
 *      according to Valgrind) is normal.
 *
 *      Now you ask: why do I care so much about Valgrind, memory leaks, and cleaning
 *      up the code? The less spurious memory leaks, the easier it is to identify
 *      actual leaks among the noise and to patch them up. Thus, "valgrind hunting" --J.C. */

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ctime>
#include <unistd.h>
#include "dosbox.h"
#include "debug.h"
#include "cpu.h"
#include "video.h"
#include "pic.h"
#include "cpu.h"
#include "ide.h"
#include "callback.h"
#include "inout.h"
#include "mixer.h"
#include "timer.h"
#include "dos_inc.h"
#include "setup.h"
#include "control.h"
#include "cross.h"
#include "programs.h"
#include "support.h"
#include "mapper.h"
#include "ints/int10.h"
#include "menu.h"
#include "render.h"
#include "pci_bus.h"
#include "parport.h"
#include "clockdomain.h"

#if C_EMSCRIPTEN
# include <emscripten.h>
#endif

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <list>

/*===================================TODO: Move to it's own file==============================*/
#if defined(__SSE__) && !defined(_M_AMD64)
bool sse2_available = false;

# ifdef __GNUC__
#  define cpuid(func,ax,bx,cx,dx)\
    __asm__ __volatile__ ("cpuid":\
    "=a" (ax), "=b" (bx), "=c" (cx), "=d" (dx) : "a" (func));
# endif /* __GNUC__ */

# if defined(_MSC_VER)
#  define cpuid(func,a,b,c,d)\
    __asm mov eax, func\
    __asm cpuid\
    __asm mov a, eax\
    __asm mov b, ebx\
    __asm mov c, ecx\
    __asm mov d, edx
# endif /* _MSC_VER */

void CheckSSESupport()
{
#if (defined (__GNUC__) || (_MSC_VER)) && !defined(EMSCRIPTEN)
    Bitu a, b, c, d;
    cpuid(1, a, b, c, d);
    sse2_available = ((d >> 26) & 1)?true:false;
#endif
}
#endif
/*=============================================================================*/

extern void         GFX_SetTitle(Bit32s cycles,Bits frameskip,Bits timing,bool paused);

extern Bitu         frames;
extern Bitu         cycle_count;
extern bool         sse2_available;
extern bool         dynamic_dos_kernel_alloc;
extern Bitu         DOS_PRIVATE_SEGMENT_Size;
extern bool         VGA_BIOS_dont_duplicate_CGA_first_half;
extern bool         VIDEO_BIOS_always_carry_14_high_font;
extern bool         VIDEO_BIOS_always_carry_16_high_font;
extern bool         VIDEO_BIOS_enable_CGA_8x8_second_half;
extern bool         allow_more_than_640kb;

Bit32u              guest_msdos_LoL = 0;
Bit16u              guest_msdos_mcb_chain = 0;
int                 boothax = BOOTHAX_NONE;

bool                want_fm_towns = false;

bool                dos_con_use_int16_to_detect_input = true;

bool                dbg_zero_on_dos_allocmem = true;
bool                dbg_zero_on_xms_allocmem = true;
bool                dbg_zero_on_ems_allocmem = true;

/* the exact frequency of the NTSC color subcarrier ~3.579545454...MHz or 315/88 */
/* see: http://en.wikipedia.org/wiki/Colorburst */
#define             NTSC_COLOR_SUBCARRIER_NUM       (315000000ULL)
#define             NTSC_COLOR_SUBCARRIER_DEN       (88ULL)

/* PCI bus clock
 * Usual setting: 100MHz / 3 = 33.333MHz
 *                 90MHz / 3 = 30.000MHz */
ClockDomain         clockdom_PCI_BCLK(100000000,3);     /* MASTER 100MHz / 3 = 33.33333MHz */

/* ISA bus OSC clock (14.31818MHz), using a crystal that is 4x the NTSC subcarrier frequency 3.5795454..MHz */
ClockDomain         clockdom_ISA_OSC(NTSC_COLOR_SUBCARRIER_NUM*4,NTSC_COLOR_SUBCARRIER_DEN);

/* ISA bus clock (varies between 4.77MHz to 8.333MHz)
 * PC/XT: ISA oscillator clock (14.31818MHz / 3) = 4.77MHz
 * Some systems keep CPU synchronous to bus clock: 4.77MHz, 6MHz, 8MHz, 8.333MHz
 * Later systems: 25MHz / 3 = 8.333MHz
 *                33MHz / 4 = 8.333MHz
 * PCI bus systems: PCI bus clock 33MHz / 4 = 8.333MHz (especially Intel chipsets according to PIIX datasheets) */
ClockDomain         clockdom_ISA_BCLK(25000000,3);      /* MASTER 25000000Hz / 3 = 8.333333MHz */

Config*             control;
MachineType         machine;
bool                PS1AudioCard;       // Perhaps have PS1 as a machine type...?
SVGACards           svgaCard;
bool                SDLNetInited;
Bit32s              ticksDone;
Bit32u              ticksScheduled;
bool                ticksLocked;
bool                mono_cga=false;
bool                ignore_opcode_63 = true;
int             dynamic_core_cache_block_size = 32;
Bitu                VGA_BIOS_Size_override = 0;
Bitu                VGA_BIOS_SEG = 0xC000;
Bitu                VGA_BIOS_SEG_END = 0xC800;
Bitu                VGA_BIOS_Size = 0x8000;

Bit32u                  emulator_speed = 100;

static Bit32u           ticksRemain;
static Bit32u           ticksRemainSpeedFrac;
static Bit32u           ticksLast;
static Bit32u           ticksLastFramecounter;
static Bit32u           ticksLastRTcounter;
static double           ticksLastRTtime;
static Bit32u           ticksAdded;
static Bit32u           Ticks = 0;
extern double           rtdelta;
static LoopHandler*     loop;

void increaseticks();

/* The whole load of startups for all the subfunctions */
void                MEM_Init(Section *);
void                ISAPNP_Cfg_Init(Section *);
void                ROMBIOS_Init(Section *);
void                CALLBACK_Init(Section*);
void                PROGRAMS_Init(Section*);
void                RENDER_Init(Section*);
void                VGA_VsyncInit(Section*);
void                VGA_Init(Section*);
void                DOS_Init(Section*);
void                CPU_Init(Section*);
#if C_FPU
void                FPU_Init(Section*);
#endif
void                DMA_Init(Section*);
void                MIXER_Init(Section*);
void                MIDI_Init(Section*);
void                HARDWARE_Init(Section*);
void                PCIBUS_Init(Section*);
void                PCI_Init(Section*);
void                VOODOO_Init(Section*);

void                IDE_Primary_Init(Section*);
void                IDE_Secondary_Init(Section*);
void                IDE_Tertiary_Init(Section*);
void                IDE_Quaternary_Init(Section*);
void                IDE_Quinternary_Init(Section*);
void                IDE_Sexternary_Init(Section*);
void                IDE_Septernary_Init(Section*);
void                IDE_Octernary_Init(Section*);

void                FDC_Primary_Init(Section*);

void                KEYBOARD_Init(Section*);    //TODO This should setup INT 16 too but ok ;)
void                JOYSTICK_Init(Section*);
void                MOUSE_Init(Section*);
void                SBLASTER_Init(Section*);
void                GUS_Init(Section*);
void                MPU401_Init(Section*);
void                PCSPEAKER_Init(Section*);
void                TANDYSOUND_Init(Section*);
void                DISNEY_Init(Section*);
void                PS1SOUND_Init(Section*);
void                INNOVA_Init(Section*);
void                SERIAL_Init(Section*); 
void                DONGLE_Init(Section*);
#if C_IPX
void                IPX_Init(Section*);
#endif
void                PIC_Init(Section*);
void                TIMER_Init(Section*);
void                BIOS_Init(Section*);
void                DEBUG_Init(Section*);
void                CMOS_Init(Section*);
void                MSCDEX_Init(Section*);
void                DRIVES_Init(Section*);
void                CDROM_Image_Init(Section*);
void                EMS_Init(Section*);
void                XMS_Init(Section*);
void                DOS_KeyboardLayout_Init(Section*);
void                AUTOEXEC_Init(Section*);
void                INT10_Init(Section*);
#if C_PRINTER
void                PRINTER_Init(Section*);
#endif

signed long long time_to_clockdom(ClockDomain &src,double t) {
    signed long long lt = (signed long long)t;

    lt *= (signed long long)src.freq;
    lt /= (signed long long)src.freq_div;
    return lt;
}

unsigned long long update_clockdom_from_now(ClockDomain &dst) {
    signed long long s;

    /* PIC_Ticks (if I read the code correctly) is millisecond ticks, units of 1/1000 seconds.
     * PIC_TickIndexND() units of submillisecond time in units of 1/CPU_CycleMax. */
    s  = (signed long long)((unsigned long long)PIC_Ticks * (unsigned long long)dst.freq);
    s += (signed long long)(((unsigned long long)PIC_TickIndexND() * (unsigned long long)dst.freq) / (unsigned long long)CPU_CycleMax);
    /* convert down to frequency counts, not freq x 1000 */
    s /= (signed long long)(1000ULL * (unsigned long long)dst.freq_div);

    /* guard against time going backwards slightly (as PIC_TickIndexND() will do sometimes by tiny amounts) */
    if (dst.counter < (unsigned long long)s) dst.counter = (unsigned long long)s;

    return dst.counter;
}

#include "paging.h"

extern bool rom_bios_vptable_enable;
extern bool rom_bios_8x8_cga_font;
extern bool allow_port_92_reset;
extern bool allow_keyb_reset;

extern bool DOSBox_Paused();

//#define DEBUG_CYCLE_OVERRUN_CALLBACK

//For trying other delays
#define wrap_delay(a) SDL_Delay(a)

static Bitu Normal_Loop(void) {
    bool saved_allow = dosbox_allow_nonrecursive_page_fault;
    Bits ret;

    if (!menu.hidecycles || menu.showrt) { /* sdlmain.cpp/render.cpp doesn't even maintain the frames count when hiding cycles! */
        Bit32u ticksNew = GetTicks();
        if (ticksNew >= Ticks) {
            Bit32u interval = ticksNew - ticksLastFramecounter;
            double rtnow = PIC_FullIndex();

            if (interval == 0) interval = 1; // avoid divide by zero

            rtdelta = rtnow - ticksLastRTtime;
            rtdelta = (rtdelta * 1000) / interval;

            ticksLastRTtime = rtnow;
            ticksLastFramecounter = Ticks;
            Ticks = ticksNew + 500;     // next update in 500ms
            frames = (frames * 1000) / interval; // compensate for interval, be more exact (FIXME: so can we adjust for fractional frame rates)
            GFX_SetTitle((Bit32s)CPU_CycleMax,-1,-1,false);
            frames = 0;
        }
    }

    try {
        while (1) {
            if (PIC_RunQueue()) {
                /* now is the time to check for the NMI (Non-maskable interrupt) */
                CPU_Check_NMI();

                saved_allow = dosbox_allow_nonrecursive_page_fault;
                dosbox_allow_nonrecursive_page_fault = true;
                ret = (*cpudecoder)();
                dosbox_allow_nonrecursive_page_fault = saved_allow;

                if (GCC_UNLIKELY(ret<0))
                    return 1;

                if (ret>0) {
                    if (GCC_UNLIKELY((unsigned int)ret >= CB_MAX))
                        return 0;

                    extern unsigned int last_callback;
                    unsigned int p_last_callback = last_callback;
                    last_callback = (unsigned int)ret;

                    dosbox_allow_nonrecursive_page_fault = false;
                    Bitu blah = (*CallBack_Handlers[ret])();
                    dosbox_allow_nonrecursive_page_fault = saved_allow;

                    last_callback = p_last_callback;

#ifdef DEBUG_CYCLE_OVERRUN_CALLBACK
                    {
                        extern char* CallBack_Description[CB_MAX];

                        /* I/O delay can cause negative CPU_Cycles and PIC event / audio rendering issues */
                        cpu_cycles_count_t overrun = -std::min(CPU_Cycles,(cpu_cycles_count_t)0);

                        if (overrun > (CPU_CycleMax/100))
                            LOG_MSG("Normal loop: CPU cycles count overrun by %ld (%.3fms) after callback '%s'\n",(signed long)overrun,(double)overrun / CPU_CycleMax,CallBack_Description[ret]);
                    }
#endif

                    if (GCC_UNLIKELY(blah > 0U))
                        return blah;
                }
#if C_DEBUG
                if (DEBUG_ExitLoop())
                    return 0;
#endif
            } else {
                GFX_Events();
                if (DOSBox_Paused() == false && ticksRemain > 0) {
                    TIMER_AddTick();
                    ticksRemain--;
                } else {
                    increaseticks();
                    return 0;
                }
            }
        }
    }
    catch (GuestPageFaultException& pf) {
        Bitu FillFlags(void);

        ret = 0;
        FillFlags();
        dosbox_allow_nonrecursive_page_fault = false;
        CPU_Exception(EXCEPTION_PF, pf.faultcode);
        dosbox_allow_nonrecursive_page_fault = saved_allow;
    }
    catch (int x) {
        dosbox_allow_nonrecursive_page_fault = saved_allow;
        if (x == 4/*CMOS shutdown*/) {
            ret = 0;
//          LOG_MSG("CMOS shutdown reset acknowledged");
        }
        else {
            throw;
        }
    }
    return 0;
}

void increaseticks() { //Make it return ticksRemain and set it in the function above to remove the global variable.
    static Bit32s lastsleepDone = -1;
    static Bitu sleep1count = 0;
    if (GCC_UNLIKELY(ticksLocked)) { // For Fast Forward Mode
        ticksRemainSpeedFrac = 0;
        ticksRemain = 5;
        /* Reset any auto cycle guessing for this frame */
        ticksLast = GetTicks();
        ticksAdded = 0;
        ticksDone = 0;
        ticksScheduled = 0;
        return;
    }
    Bit32u ticksNew = GetTicks();
    ticksScheduled += ticksAdded;

    if (ticksNew <= ticksLast) { //lower should not be possible, only equal.
        ticksAdded = 0;

        if (!CPU_CycleAutoAdjust || CPU_SkipCycleAutoAdjust || sleep1count < 3) {
            wrap_delay(1);
        }
        else {
            /* Certain configurations always give an exact sleepingtime of 1, this causes problems due to the fact that
               dosbox keeps track of full blocks.
               This code introduces some randomness to the time slept, which improves stability on those configurations
             */
            static const Bit32u sleeppattern[] = { 2, 2, 3, 2, 2, 4, 2 };
            static Bit32u sleepindex = 0;
            if (ticksDone != lastsleepDone) sleepindex = 0;
            wrap_delay(sleeppattern[sleepindex++]);
            sleepindex %= sizeof(sleeppattern) / sizeof(sleeppattern[0]);
        }
        Bit32s timeslept = (Bit32s)(GetTicks() - ticksNew);
        // Count how many times in the current block (of 250 ms) the time slept was 1 ms
        if (CPU_CycleAutoAdjust && !CPU_SkipCycleAutoAdjust && timeslept == 1) sleep1count++;
        lastsleepDone = ticksDone;

        // Update ticksDone with the time spent sleeping
        ticksDone -= timeslept;

        if (ticksDone < 0)
            ticksDone = 0;
        return;
    }

    //ticksNew > ticksLast
    ticksRemain = ticksNew - ticksLast;

    if (emulator_speed != 100u) {
        ticksRemain *= emulator_speed;
        ticksRemain += ticksRemainSpeedFrac;
        ticksRemainSpeedFrac = ticksRemain % 100u;
        ticksRemain /= 100u;
    }
    else {
        ticksRemainSpeedFrac = 0;
    }

    ticksLast = ticksNew;
    ticksDone += (Bit32s)ticksRemain;
    if (ticksRemain > 20) {
        ticksRemain = 20;
    }
    ticksAdded = ticksRemain;

    // Is the system in auto cycle mode guessing? If not just exit. (It can be temporarily disabled)
    if (!CPU_CycleAutoAdjust || CPU_SkipCycleAutoAdjust)
        return;

    if (ticksScheduled >= 250 || ticksDone >= 250 || (ticksAdded > 15 && ticksScheduled >= 5)) {
        if (ticksDone < 1) ticksDone = 1; // Protect against div by zero
        /* ratio we are aiming for is around 90% usage*/
        Bit32s ratio = (Bit32s)((ticksScheduled * (CPU_CyclePercUsed * 90 * 1024 / 100 / 100)) / ticksDone);
        Bit32s new_cmax = (Bit32s)CPU_CycleMax;
        Bit64s cproc = (Bit64s)CPU_CycleMax * (Bit64s)ticksScheduled;
        if (cproc > 0) {
            /* ignore the cycles added due to the IO delay code in order
               to have smoother auto cycle adjustments */
            double ratioremoved = (double)CPU_IODelayRemoved / (double)cproc;
            if (ratioremoved < 1.0) {
                double ratio_not_removed = 1 - ratioremoved;
                ratio = (Bit32s)((double)ratio * ratio_not_removed);
                /* Don't allow very high ratio which can cause us to lock as we don't scale down
                 * for very low ratios. High ratio might result because of timing resolution */
                if (ticksScheduled >= 250 && ticksDone < 10 && ratio > 16384)
                    ratio = 16384;

                // Limit the ratio even more when the cycles are already way above the realmode default.
                if (ticksScheduled >= 250 && ticksDone < 10 && ratio > 5120 && CPU_CycleMax > 50000)
                    ratio = 5120;

                // When downscaling multiple times in a row, ensure a minimum amount of downscaling
                if (ticksAdded > 15 && ticksScheduled >= 5 && ticksScheduled <= 20 && ratio > 800)
                    ratio = 800;

                if (ratio <= 1024) {
                    // ratio_not_removed = 1.0; //enabling this restores the old formula
                    double r = (1.0 + ratio_not_removed) / (ratio_not_removed + 1024.0 / (static_cast<double>(ratio)));
                    new_cmax = 1 + static_cast<Bit32s>(CPU_CycleMax * r);
                }
                else {
                    Bit64s ratio_with_removed = (Bit64s)((((double)ratio - 1024.0) * ratio_not_removed) + 1024.0);
                    Bit64s cmax_scaled = (Bit64s)CPU_CycleMax * ratio_with_removed;
                    new_cmax = (Bit32s)(1 + (CPU_CycleMax >> 1) + cmax_scaled / (Bit64s)2048);
                }
            }
        }

        if (new_cmax < CPU_CYCLES_LOWER_LIMIT)
            new_cmax = CPU_CYCLES_LOWER_LIMIT;

        /*
           LOG_MSG("cyclelog: current %6d   cmax %6d   ratio  %5d  done %3d   sched %3d",
           CPU_CycleMax,
           new_cmax,
           ratio,
           ticksDone,
           ticksScheduled);
           */
           /* ratios below 1% are considered to be dropouts due to
              temporary load imbalance, the cycles adjusting is skipped */
        if (ratio > 10) {
            /* ratios below 12% along with a large time since the last update
               has taken place are most likely caused by heavy load through a
               different application, the cycles adjusting is skipped as well */
            if ((ratio > 120) || (ticksDone < 700)) {
                CPU_CycleMax = new_cmax;
                if (CPU_CycleLimit > 0) {
                    if (CPU_CycleMax > CPU_CycleLimit) CPU_CycleMax = CPU_CycleLimit;
                }
                else if (CPU_CycleMax > 2000000) CPU_CycleMax = 2000000; //Hardcoded limit, if no limit was specified.
            }
        }

        //Reset cycleguessing parameters.
        CPU_IODelayRemoved = 0;
        ticksDone = 0;
        ticksScheduled = 0;
        lastsleepDone = -1;
        sleep1count = 0;
    }
    else if (ticksAdded > 15) {
        /* ticksAdded > 15 but ticksScheduled < 5, lower the cycles
           but do not reset the scheduled/done ticks to take them into
           account during the next auto cycle adjustment */
        CPU_CycleMax /= 3;
        if (CPU_CycleMax < CPU_CYCLES_LOWER_LIMIT)
            CPU_CycleMax = CPU_CYCLES_LOWER_LIMIT;
    }
}

LoopHandler *DOSBOX_GetLoop(void) {
    return loop;
}

void DOSBOX_SetLoop(LoopHandler * handler) {
    loop=handler;
}

void DOSBOX_SetNormalLoop() {
    loop=Normal_Loop;
}

//#define DEBUG_RECURSION

#ifdef DEBUG_RECURSION
volatile int runmachine_recursion = 0;
#endif

void DOSBOX_RunMachine(void){
    Bitu ret;

    extern unsigned int last_callback;
    unsigned int p_last_callback = last_callback;
    last_callback = 0;

#ifdef DEBUG_RECURSION
    if (runmachine_recursion++ != 0)
        LOG_MSG("RunMachine recursion");
#endif

    do {
        ret=(*loop)();
    } while (!ret);

#ifdef DEBUG_RECURSION
    if (--runmachine_recursion < 0)
        LOG_MSG("RunMachine recursion leave error");
#endif

    last_callback = p_last_callback;
}

static void DOSBOX_UnlockSpeed( bool pressed ) {
    static bool autoadjust = false;
    if (pressed) {
        LOG_MSG("Fast Forward ON");
        ticksLocked = true;
        if (CPU_CycleAutoAdjust) {
            autoadjust = true;
            CPU_CycleAutoAdjust = false;
            CPU_CycleMax /= 3;
            if (CPU_CycleMax<1000) CPU_CycleMax=1000;
        }
    } else {
        LOG_MSG("Fast Forward OFF");
        ticksLocked = false;
        if (autoadjust) {
            autoadjust = false;
            CPU_CycleAutoAdjust = true;
        }
    }
    GFX_SetTitle(-1,-1,-1,false);
}

void DOSBOX_UnlockSpeed2( bool pressed ) {
    if (pressed) {
        ticksLocked = !ticksLocked;
        DOSBOX_UnlockSpeed(ticksLocked?true:false);

        /* make sure the menu item keeps up with our state */
        mainMenu.get_item("mapper_speedlock2").check(ticksLocked).refresh_item(mainMenu);
    }
}

void DOSBOX_NormalSpeed( bool pressed ) {
    if (pressed) {
        /* should also cancel turbo mode */
        if (ticksLocked)
            DOSBOX_UnlockSpeed2(true);

        LOG_MSG("Emulation speed restored to normal (100%%)");

        emulator_speed = 100;
        ticksRemainSpeedFrac = 0;
    }
}

void DOSBOX_SpeedUp( bool pressed ) {
    if (pressed) {
        ticksRemainSpeedFrac = 0;
        if (emulator_speed >= 5)
            emulator_speed += 5;
        else
            emulator_speed = 5;

        LOG_MSG("Emulation speed increased to (%u%%)",(unsigned int)emulator_speed);
    }
}

void DOSBOX_SlowDown( bool pressed ) {
    if (pressed) {
        ticksRemainSpeedFrac = 0;
        if (emulator_speed  > 5)
            emulator_speed -= 5;
        else
            emulator_speed = 1;

        LOG_MSG("Emulation speed decreased to (%u%%)",(unsigned int)emulator_speed);
    }
}

void notifyError(const std::string& message)
{
#ifdef WIN32
    ::MessageBox(0, message.c_str(), "Error", 0);
#endif
    LOG_MSG("%s",message.c_str());
}

/* TODO: move to utility header */
#ifdef _MSC_VER /* Microsoft C++ does not have strtoull */
# if _MSC_VER < 1800 /* But Visual Studio 2013 apparently does (http://www.vogons.org/viewtopic.php?f=41&t=31881&sid=49ff69ebc0459ed6523f5a250daa4d8c&start=400#p355770) */
unsigned long long strtoull(const char *s,char **endptr,int base) {
    return _strtoui64(s,endptr,base); /* pfff... whatever Microsoft */
}
# endif
#endif

/* utility function. rename as appropriate and move to utility collection */
void parse_busclk_setting_str(ClockDomain *cd,const char *s) {
    const char *d;

    /* we're expecting an integer, a float, or an integer ratio */
    d = strchr(s,'/');
    if (d != NULL) { /* it has a slash therefore an integer ratio */
        unsigned long long num,den;

        while (*d == ' ' || *d == '/') d++;
        num = strtoull(s,NULL,0);
        den = strtoull(d,NULL,0);
        if (num >= 1ULL && den >= 1ULL) cd->set_frequency(num,den);
    }
    else {
        d = strchr(s,'.');
        if (d != NULL) { /* it has a dot, floating point */
            double f = atof(s);
            unsigned long long fi = (unsigned long long)floor((f*1000000)+0.5);
            unsigned long long den = 1000000;

            while (den > 1ULL) {
                if ((fi%10ULL) == 0) {
                    den /= 10ULL;
                    fi /= 10ULL;
                }
                else {
                    break;
                }
            }

            if (fi >= 1ULL) cd->set_frequency(fi,den);
        }
        else {
            unsigned long long f = strtoull(s,NULL,10);
            if (f >= 1ULL) cd->set_frequency(f,1);
        }
    }
}

unsigned int dosbox_shell_env_size = 0;

void Null_Init(Section *sec) {
	(void)sec;
}

extern Bit8u cga_comp;
extern bool new_cga;

bool dpi_aware_enable = true;

std::string dosbox_title;

void DOSBOX_InitTickLoop() {
    LOG(LOG_MISC,LOG_DEBUG)("Initializing tick loop management");

    ticksRemain = 0;
    ticksLocked = false;
    ticksLastRTtime = 0;
    ticksLast = GetTicks();
    ticksLastRTcounter = GetTicks();
    ticksLastFramecounter = GetTicks();
    DOSBOX_SetLoop(&Normal_Loop);
}

void Init_VGABIOS() {
    Section_prop *section = static_cast<Section_prop *>(control->GetSection("dosbox"));
    assert(section != NULL);

    if (IS_PC98_ARCH) {
        // There IS no VGA BIOS, this is PC-98 mode!
        VGA_BIOS_SEG = 0xC000;
        VGA_BIOS_SEG_END = 0xC000; // Important: DOS kernel uses this to determine where to place the private area!
        VGA_BIOS_Size = 0;
        return;
    }

    // log
    LOG(LOG_MISC,LOG_DEBUG)("Init_VGABIOS: Initializing VGA BIOS and parsing it's settings");

    // mem init must have already happened.
    // We can remove this once the device callout system is in place.
    assert(MemBase != NULL);

    VGA_BIOS_Size_override = (Bitu)section->Get_int("vga bios size override");
    if (VGA_BIOS_Size_override > 0) VGA_BIOS_Size_override = (VGA_BIOS_Size_override+0x7FFU)&(~0xFFFU);

    VGA_BIOS_dont_duplicate_CGA_first_half = section->Get_bool("video bios dont duplicate cga first half rom font");
    VIDEO_BIOS_always_carry_14_high_font = section->Get_bool("video bios always offer 14-pixel high rom font");
    VIDEO_BIOS_always_carry_16_high_font = section->Get_bool("video bios always offer 16-pixel high rom font");
    VIDEO_BIOS_enable_CGA_8x8_second_half = section->Get_bool("video bios enable cga second half rom font");
    /* NTS: mainline compatible mapping demands the 8x8 CGA font */
    rom_bios_8x8_cga_font = section->Get_bool("rom bios 8x8 CGA font");
    rom_bios_vptable_enable = section->Get_bool("rom bios video parameter table");

    /* sanity check */
    if (VGA_BIOS_dont_duplicate_CGA_first_half && !rom_bios_8x8_cga_font) /* can't point at the BIOS copy if it's not there */
        VGA_BIOS_dont_duplicate_CGA_first_half = false;

    if (VGA_BIOS_Size_override >= 512 && VGA_BIOS_Size_override <= 65536)
        VGA_BIOS_Size = (VGA_BIOS_Size_override + 0x7FFU) & (~0xFFFU);
    else if (IS_VGA_ARCH) {
        if (svgaCard == SVGA_S3Trio)
            VGA_BIOS_Size = 0x4000;
        else
            VGA_BIOS_Size = 0x4000; // FIXME: Why does 0x3800 cause Windows 3.0 386 enhanced mode to hang?
    }
    else if (machine == MCH_EGA) {
        if (VIDEO_BIOS_always_carry_16_high_font)
            VGA_BIOS_Size = 0x3000;
        else
            VGA_BIOS_Size = 0x2000;
    }
    else {
        if (VIDEO_BIOS_always_carry_16_high_font && VIDEO_BIOS_always_carry_14_high_font)
            VGA_BIOS_Size = 0x3000;
        else if (VIDEO_BIOS_always_carry_16_high_font || VIDEO_BIOS_always_carry_14_high_font)
            VGA_BIOS_Size = 0x2000;
        else
            VGA_BIOS_Size = 0;
    }
    VGA_BIOS_SEG = 0xC000;
    VGA_BIOS_SEG_END = (VGA_BIOS_SEG + (VGA_BIOS_Size >> 4));

    /* clear for VGA BIOS (FIXME: Why does Project Angel like our BIOS when we memset() here, but don't like it if we memset() in the INT 10 ROM setup routine?) */
    if (VGA_BIOS_Size != 0)
        memset((char*)MemBase+0xC0000,0x00,VGA_BIOS_Size);
}

void DOSBOX_RealInit() {
    DOSBoxMenu::item *item;

    LOG(LOG_MISC,LOG_DEBUG)("DOSBOX_RealInit: loading settings and initializing");

    MAPPER_AddHandler(DOSBOX_UnlockSpeed, MK_rightarrow, MMODHOST,"speedlock","Speedlock");
    {
        MAPPER_AddHandler(DOSBOX_UnlockSpeed2, MK_nothing, 0, "speedlock2", "Speedlock2", &item);
        item->set_description("Toggle emulation speed, to allow running faster than realtime (fast forward)");
        item->set_text("Turbo (Fast Forward)");
    }
    {
        MAPPER_AddHandler(DOSBOX_NormalSpeed, MK_leftarrow, MMODHOST, "speednorm","SpeedNrm", &item);
        item->set_description("Restore normal emulation speed");
        item->set_text("Normal speed");
    }
    {
        MAPPER_AddHandler(DOSBOX_SpeedUp, MK_rbracket, MMODHOST, "speedup","SpeedUp", &item);
        item->set_text("Speed up");
    }
    {
        MAPPER_AddHandler(DOSBOX_SlowDown, MK_lbracket, MMODHOST,"slowdown","SlowDn", &item);
        item->set_text("Slow down");
    }

    Section_prop *section = static_cast<Section_prop *>(control->GetSection("dosbox"));
    assert(section != NULL);

    // TODO: allow change at any time. in fact if it were possible for DOSBox-X configuration
    //       schema code to attach event callbacks when a setting changes, we would set one
    //       on the title= setting now to auto-update the titlebar when this changes.
    dosbox_title = section->Get_string("title");

    // TODO: these should be parsed by DOS kernel at startup
    dosbox_shell_env_size = (unsigned int)section->Get_int("shell environment size");

    // TODO: a bit of a challenge: if we put it in the ROM area as mainline DOSBox does then the init
    //       needs to read this from the BIOS where it can map the memory appropriately. if the allocation
    //       is dynamic and the private area is down at the base of memory like real DOS, then the BIOS
    //       should ignore it and the DOS kernel should parse it. If we're going to put it into upper
    //       areas as well, then we should also consider making it look like adapter ROM at startup
    //       so it can be enumerated properly by DOS programs scanning the ROM area.
    /* private area size param in bytes. round up to nearest paragraph */
    DOS_PRIVATE_SEGMENT_Size = (Bitu)((section->Get_int("private area size") + 8) / 16);

    // TODO: these should be parsed by BIOS startup
    allow_more_than_640kb = section->Get_bool("allow more than 640kb base memory");

    // TODO: should be parsed by motherboard emulation
    allow_port_92_reset = section->Get_bool("allow port 92 reset");

    // CGA/EGA/VGA-specific
    extern unsigned char vga_p3da_undefined_bits;
    vga_p3da_undefined_bits = (unsigned char)section->Get_hex("vga 3da undefined bits");

    // TODO: should be parsed by motherboard emulation or lower level equiv..?
    std::string cmd_machine;
    if (control->cmdline->FindString("-machine",cmd_machine,true)){
        //update value in config (else no matching against suggested values
        section->HandleInputline(std::string("machine=") + cmd_machine);
    }

    // TODO: should be parsed by...? perhaps at some point we support machine= for backwards compat
    //       but translate it into two separate params that specify what machine vs what video hardware.
    //       or better yet as envisioned, a possible dosbox.conf schema that allows a machine with no
    //       base video of it's own, and then to specify an ISA or PCI card attached to the bus that
    //       provides video.
    std::string mtype(section->Get_string("machine"));
    svgaCard = SVGA_None;
    machine = MCH_VGA;
    int10.vesa_nolfb = false;
    int10.vesa_oldvbe = false;
    if      (mtype == "cga")           { machine = MCH_CGA; mono_cga = false; }
    else if (mtype == "cga_mono")      { machine = MCH_CGA; mono_cga = true; }
    else if (mtype == "cga_rgb")       { machine = MCH_CGA; mono_cga = false; cga_comp = 2; }
    else if (mtype == "cga_composite") { machine = MCH_CGA; mono_cga = false; cga_comp = 1; new_cga = false; }
    else if (mtype == "cga_composite2"){ machine = MCH_CGA; mono_cga = false; cga_comp = 1; new_cga = true; }
    else if (mtype == "mcga")          { machine = MCH_MCGA; }
    else if (mtype == "tandy")         { machine = MCH_TANDY; }
    else if (mtype == "pcjr")          { machine = MCH_PCJR; }
    else if (mtype == "hercules")      { machine = MCH_HERC; }
    else if (mtype == "mda")           { machine = MCH_MDA; }
    else if (mtype == "ega")           { machine = MCH_EGA; }
    else if (mtype == "svga_s3")       { svgaCard = SVGA_S3Trio; }
    else if (mtype == "vesa_nolfb")    { svgaCard = SVGA_S3Trio; int10.vesa_nolfb = true;}
    else if (mtype == "vesa_oldvbe")   { svgaCard = SVGA_S3Trio; int10.vesa_oldvbe = true;}
    else if (mtype == "svga_et4000")   { svgaCard = SVGA_TsengET4K; }
    else if (mtype == "svga_et3000")   { svgaCard = SVGA_TsengET3K; }
    else if (mtype == "svga_paradise") { svgaCard = SVGA_ParadisePVGA1A; }
    else if (mtype == "vgaonly")       { svgaCard = SVGA_None; }
    else if (mtype == "amstrad")       { machine = MCH_AMSTRAD; }
    else if (mtype == "pc98")          { machine = MCH_PC98; }
    else if (mtype == "pc9801")        { machine = MCH_PC98; } /* Future differentiation */
    else if (mtype == "pc9821")        { machine = MCH_PC98; } /* Future differentiation */

    else if (mtype == "fm_towns")      { machine = MCH_VGA; want_fm_towns = true; /*machine = MCH_FM_TOWNS;*/ }

    else E_Exit("DOSBOX:Unknown machine type %s",mtype.c_str());

    // TODO: should be parsed by motherboard emulation
    // FIXME: This re-uses the existing ISA bus delay code for C-BUS in PC-98 mode
    std::string isabclk;

    if (IS_PC98_ARCH)
        isabclk = section->Get_string("cbus bus clock");
    else
        isabclk = section->Get_string("isa bus clock");

    if (isabclk == "std10")
        clockdom_ISA_BCLK.set_frequency(PIT_TICK_RATE_PC98_10MHZ * 4ul,1);          /* 10Mhz (PC-98) */
    else if (isabclk == "std8.3")
        clockdom_ISA_BCLK.set_frequency(25000000,3);    /* 25MHz / 3 = 8.333MHz, early 386 systems did this, became an industry standard "norm" afterwards */
    else if (isabclk == "std8") {
        if (IS_PC98_ARCH)
            clockdom_ISA_BCLK.set_frequency(PIT_TICK_RATE_PC98_8MHZ * 4ul,1);       /* 8Mhz (PC-98) */
        else
            clockdom_ISA_BCLK.set_frequency(8000000,1);                             /* 8Mhz (IBM PC) */
    }
    else if (isabclk == "std6")
        clockdom_ISA_BCLK.set_frequency(6000000,1);     /* 6MHz */
    else if (isabclk == "std5")
        clockdom_ISA_BCLK.set_frequency(PIT_TICK_RATE_PC98_10MHZ * 2ul,1);          /* 5Mhz (PC-98) */
    else if (isabclk == "std4.77")
        clockdom_ISA_BCLK.set_frequency(clockdom_ISA_OSC.freq,clockdom_ISA_OSC.freq_div*3LL); /* 14.31818MHz / 3 = 4.77MHz */
    else if (isabclk == "oc10")
        clockdom_ISA_BCLK.set_frequency(10000000,1);    /* 10MHz */
    else if (isabclk == "oc12")
        clockdom_ISA_BCLK.set_frequency(12000000,1);    /* 12MHz */
    else if (isabclk == "oc15")
        clockdom_ISA_BCLK.set_frequency(15000000,1);    /* 15MHz */
    else if (isabclk == "oc16")
        clockdom_ISA_BCLK.set_frequency(16000000,1);    /* 16MHz */
    else
        parse_busclk_setting_str(&clockdom_ISA_BCLK,isabclk.c_str());

    std::string pcibclk = section->Get_string("pci bus clock");
    if (pcibclk == "std33.3")
        clockdom_PCI_BCLK.set_frequency(100000000,3);   /* 100MHz / 3 = 33.333MHz, VERY common PCI speed */
    else if (pcibclk == "std30")
        clockdom_PCI_BCLK.set_frequency(30000000,1);    /* 30Mhz */
    else if (pcibclk == "std25")
        clockdom_PCI_BCLK.set_frequency(25000000,1);    /* 25MHz */
    else
        parse_busclk_setting_str(&clockdom_PCI_BCLK,pcibclk.c_str());

    LOG_MSG("%s BCLK: %.3fHz (%llu/%llu)",
        IS_PC98_ARCH ? "C-BUS" : "ISA",
        (double)clockdom_ISA_BCLK.freq / clockdom_ISA_BCLK.freq_div,
        (unsigned long long)clockdom_ISA_BCLK.freq,
        (unsigned long long)clockdom_ISA_BCLK.freq_div);

    clockdom_ISA_OSC.set_name("ISA OSC");
    clockdom_ISA_BCLK.set_name("ISA BCLK");
    clockdom_PCI_BCLK.set_name("PCI BCLK");

    // FM TOWNS is stub so far. According to sources like Wikipedia though,
    // it boots from DOS in ROM that then loads bootcode from CD-ROM. So
    // for now, allow booting into FM TOWNS mode with a warning. The
    // switch to FM Towns will begin in the BOOT command with a flag to
    // indicate the ISO is intended for FM TOwns.
    if (IS_FM_TOWNS || want_fm_towns) LOG_MSG("FM Towns emulation not yet implemented. It's currently just a stub for future development.");
}

void DOSBOX_SetupConfigSections(void) {
    Prop_int* Pint;
    Prop_hex* Phex;
    Prop_bool* Pbool;
    Prop_string* Pstring;
    Prop_double* Pdouble;
    Prop_multival* Pmulti;
    Section_prop * secprop;
    Prop_multival_remain* Pmulti_remain;

    // Some frequently used option sets
    const char* vsyncrate[] = { "%u", 0 };
    const char* force[] = { "", "forced", 0 };
    const char* cyclest[] = { "auto","fixed","max","%u",0 };
    const char* mputypes[] = { "intelligent", "uart", "none", 0 };
    const char* vsyncmode[] = { "off", "on" ,"force", "host", 0 };
    const char* captureformats[] = { "default", "avi-zmbv", "mpegts-h264", 0 };
    const char* blocksizes[] = {"1024", "2048", "4096", "8192", "512", "256", 0};
    const char* capturechromaformats[] = { "auto", "4:4:4", "4:2:2", "4:2:0", 0};
    const char* controllertypes[] = { "auto", "at", "xt", "pcjr", "pc98", 0}; // Future work: Tandy(?) and USB
    const char* auxdevices[] = {"none","2button","3button","intellimouse","intellimouse45",0};
    const char* cputype_values[] = {"auto", "8086", "8086_prefetch", "80186", "80186_prefetch", "286", "286_prefetch", "386", "386_prefetch", "486old", "486old_prefetch", "486", "486_prefetch", "pentium", "pentium_mmx", "ppro_slow", 0};
    const char* rates[] = {  "44100", "48000", "32000","22050", "16000", "11025", "8000", "49716", 0 };
    const char* oplrates[] = {   "44100", "49716", "48000", "32000","22050", "16000", "11025", "8000", 0 };
    const char* devices[] = { "default", "win32", "alsa", "oss", "coreaudio", "coremidi", "mt32", "synth", "timidity", "none", 0}; // FIXME: add some way to offer the actually available choices.
    const char* apmbiosversions[] = { "auto", "1.0", "1.1", "1.2", 0 };
    const char *mt32log[] = {"off", "on",0};
    const char *mt32thread[] = {"off", "on",0};
    const char *mt32ReverseStereo[] = {"off", "on",0};
    const char *mt32DACModes[] = {"0", "1", "2", "3", "auto",0};
    const char *mt32reverbModes[] = {"0", "1", "2", "3", "auto",0};
    const char *mt32reverbTimes[] = {"0", "1", "2", "3", "4", "5", "6", "7",0};
    const char *mt32reverbLevels[] = {"0", "1", "2", "3", "4", "5", "6", "7",0};
    const char* gustypes[] = { "classic", "classic37", "max", "interwave", 0 };
    const char* sbtypes[] = { "sb1", "sb2", "sbpro1", "sbpro2", "sb16", "sb16vibra", "gb", "ess688", "reveal_sc400", "none", 0 };
    const char* oplmodes[]={ "auto", "cms", "opl2", "dualopl2", "opl3", "opl3gold", "none", "hardware", "hardwaregb", 0};
    const char* serials[] = { "dummy", "disabled", "modem", "nullmodem", "serialmouse", "directserial", "log", "file", 0 };
    const char* acpi_rsd_ptr_settings[] = { "auto", "bios", "ebda", 0 };
    const char* cpm_compat_modes[] = { "auto", "off", "msdos2", "msdos5", "direct", 0 };
    const char* dosv_settings[] = { "off", "japanese", "chinese", "korean", 0 };
    const char* acpisettings[] = { "off", "1.0", "1.0b", "2.0", "2.0a", "2.0b", "2.0c", "3.0", "3.0a", "3.0b", "4.0", "4.0a", "5.0", "5.0a", "6.0", 0 };
    const char* guspantables[] = { "old", "accurate", "default", 0 };
    const char *sidbaseno[] = { "240", "220", "260", "280", "2a0", "2c0", "2e0", "300", 0 };
    const char* joytypes[] = { "auto", "2axis", "4axis", "4axis_2", "fcs", "ch", "none",0};
//    const char* joydeadzone[] = { "0.26", 0 };
//    const char* joyresponse[] = { "1.0", 0 };
    const char* iosgus[] = { "240", "220", "260", "280", "2a0", "2c0", "2e0", "300", "210", "230", "250", 0 };
    const char* mpubases[] = {
        "0",                                                                                    /* Auto */
        "300", "310", "320", "330", "332", "334", "336", "340", "360",                          /* IBM PC */
        "c0d0","c8d0","d0d0","d8d0","e0d0","e8d0","f0d0","f8d0",                                /* NEC PC-98 MPU98 */
        "80d2","80d4","80d6","80d8","80da","80dc","80de",                                       /* NEC PC-98 SB16 */
        0 };
    const char* ios[] = {
        "220", "240", "260", "280", "2a0", "2c0", "2e0",            /* IBM PC      (base+port i.e. 220h base, 22Ch is DSP) */
        "d2",  "d4",  "d6",  "d8",  "da",  "dc",  "de",             /* NEC PC-98   (base+(port << 8) i.e. 00D2h base, 2CD2h is DSP) */
        0 };
    const char* ems_settings[] = { "true", "emsboard", "emm386", "false", 0};
    const char* irqsgus[] = { "5", "3", "7", "9", "10", "11", "12", 0 };
    const char* irqssb[] = { "7", "5", "3", "9", "10", "11", "12", 0 };
    const char* dmasgus[] = { "3", "0", "1", "5", "6", "7", 0 };
    const char* dmassb[] = { "1", "5", "0", "3", "6", "7", 0 };
    const char* oplemus[] = { "default", "compat", "fast", "nuked", "mame", 0 };
    const char *qualityno[] = { "0", "1", "2", "3", 0 };
    const char* tandys[] = { "auto", "on", "off", 0};
    const char* ps1opt[] = { "on", "off", 0};
    const char* truefalseautoopt[] = { "true", "false", "1", "0", "auto", 0};
    const char* pc98fmboards[] = { "auto", "off", "false", "board14", "board26k", "board86", "board86c", 0};
    const char* pc98videomodeopt[] = { "", "24khz", "31khz", "15khz", 0};
    const char* aspectmodes[] = { "false", "true", "0", "1", "yes", "no", "nearest", "bilinear", 0};
    const char *vga_ac_mapping_settings[] = { "", "auto", "4x4", "4low", "first16", 0 };

    const char* irqhandler[] = {
        "", "simple", "cooperative_2nd", 0 };

    /* Setup all the different modules making up DOSBox */
    const char* machines[] = {
        "hercules", "cga", "cga_mono", "cga_rgb", "cga_composite", "cga_composite2", "tandy", "pcjr", "ega",
        "vgaonly", "svga_s3", "svga_et3000", "svga_et4000",
        "svga_paradise", "vesa_nolfb", "vesa_oldvbe", "amstrad", "pc98", "pc9801", "pc9821",

        "fm_towns", // STUB

        "mcga", "mda",

        0 };

    const char* scalers[] = { 
        "none", "normal2x", "normal3x", "normal4x", "normal5x",
#if RENDER_USE_ADVANCED_SCALERS>2
        "advmame2x", "advmame3x", "advinterp2x", "advinterp3x", "hq2x", "hq3x", "2xsai", "super2xsai", "supereagle",
#endif
#if RENDER_USE_ADVANCED_SCALERS>0
        "tv2x", "tv3x", "rgb2x", "rgb3x", "scan2x", "scan3x", "gray", "gray2x",
#endif
        "hardware_none", "hardware2x", "hardware3x", "hardware4x", "hardware5x",
#if C_XBRZ
        "xbrz", "xbrz_bilinear",
#endif
        0 };

    const char* cores[] = { "auto",
#if (C_DYNAMIC_X86) || (C_DYNREC)
        "dynamic",
#endif
        "normal", "full", "simple", 0 };

    const char* voodoo_settings[] = {
        "false",
        "software",
#if C_OPENGL
        "opengl",
#endif
        "auto",
        0
    };

#if defined(__SSE__) && !defined(_M_AMD64) && !defined(EMSCRIPTEN)
    CheckSSESupport();
#endif
    SDLNetInited = false;

    secprop=control->AddSection_prop("dosbox",&Null_Init);
    Pstring = secprop->Add_path("language",Property::Changeable::Always,"");
    Pstring->Set_help("Select another language file.");

    Pstring = secprop->Add_path("title",Property::Changeable::Always,"");
    Pstring->Set_help("Additional text to place in the title bar of the window");

    Pbool = secprop->Add_bool("enable 8-bit dac",Property::Changeable::OnlyAtStart,true);
    Pbool->Set_help("If set, allow VESA BIOS calls in IBM PC mode to set DAC width. Has no effect in PC-98 mode.");

#if defined(MACOSX)
    /* Let's make DPI aware OFF by default so Mac OS X users with Retina displays don't yell at us about eyestrain.
       They can turn it on in combination with a nice scaler when they want it. */
    Pbool = secprop->Add_bool("dpi aware",Property::Changeable::OnlyAtStart,false);
#else
    Pbool = secprop->Add_bool("dpi aware",Property::Changeable::OnlyAtStart,true);
#endif
    Pbool->Set_help("Set this option (on by default) to indicate to your OS that DOSBox is DPI aware.\n"
            "If it is not set, Windows Vista/7/8/10 and higher may upscale the DOSBox window\n"
            "on higher resolution monitors which is probably not what you want.");

    Pbool = secprop->Add_bool("keyboard hook", Property::Changeable::Always, false);
    Pbool->Set_help("Use keyboard hook (currently only on Windows) to catch special keys and synchronize the keyboard LEDs with the host");

    // STUB OPTION, NOT YET FULLY IMPLEMENTED
    Pbool = secprop->Add_bool("weitek",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If set, emulate the Weitek coprocessor. This option only has effect if cputype=386 or cputype=486.");

    Pbool = secprop->Add_bool("bochs debug port e9",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If set, emulate Bochs debug port E9h. ASCII text written to this I/O port is assumed to be debug output, and logged.");

    Pstring = secprop->Add_string("machine",Property::Changeable::OnlyAtStart,"svga_s3");
    Pstring->Set_values(machines);
    Pstring->Set_help("The type of machine DOSBox tries to emulate.");

    Phex = secprop->Add_hex("svga lfb base", Property::Changeable::OnlyAtStart, 0);
    Phex->Set_help("If nonzero, define the physical memory address of the linear framebuffer.");

    Pbool = secprop->Add_bool("pci vga",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("If set, SVGA is emulated as if a PCI device (when enable pci bus=true)");

    Pint = secprop->Add_int("vmemdelay", Property::Changeable::WhenIdle,0);
    Pint->SetMinMax(-1,100000);
    Pint->Set_help( "VGA Memory I/O delay in nanoseconds. Set to -1 to use default, 0 to disable.\n"
            "Default off. Enable this option (-1 or nonzero) if you are running a game or\n"
            "demo that needs slower VGA memory (like that of older ISA hardware) to work properly.\n"
            "If your game is not sensitive to VGA RAM I/O speed, then turning on this option\n"
            "will do nothing but cause a significant drop in frame rate which is probably not\n"
            "what you want. Recommended values -1, 0 to 2000.");

    Pint = secprop->Add_int("vmemsize", Property::Changeable::WhenIdle,-1);
    Pint->SetMinMax(-1,8);
    Pint->Set_help(
        "Amount of video memory in megabytes.\n"
        "  The maximum resolution and color depth the svga_s3 will be able to display\n"
        "  is determined by this value.\n "
        " -1: auto (vmemsizekb is ignored)\n"
        "  0: 512k (800x600  at 256 colors) if vmemsizekb=0\n"
        "  1: 1024x768  at 256 colors or 800x600  at 64k colors\n"
        "  2: 1600x1200 at 256 colors or 1024x768 at 64k colors or 640x480 at 16M colors\n"
        "  4: 1600x1200 at 64k colors or 1024x768 at 16M colors\n"
        "  8: up to 1600x1200 at 16M colors\n"
        "For build engine games, use more memory than in the list above so it can\n"
        "use triple buffering and thus won't flicker.\n"
        );

    Pint = secprop->Add_int("vmemsizekb", Property::Changeable::WhenIdle,0);
    Pint->SetMinMax(0,1024);
    Pint->Set_help(
        "Amount of video memory in kilobytes, in addition to vmemsize");

    Pstring = secprop->Add_path("captures",Property::Changeable::Always,"capture");
    Pstring->Set_help("Directory where things like wave, midi, screenshot get captured.");

    /* will change to default true unless this causes compatibility issues with other users or their editing software */
    Pbool = secprop->Add_bool("skip encoding unchanged frames",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Unchanged frames will not be sent to the video codec as a possible performance and bandwidth optimization.");

    Pstring = secprop->Add_string("capture chroma format", Property::Changeable::OnlyAtStart,"auto");
    Pstring->Set_values(capturechromaformats);
    Pstring->Set_help("Chroma format to use when capturing to H.264. 'auto' picks the best quality option.\n"
            "4:4:4       Chroma is at full resolution. This provides the best quality, however not widely supported by editing software.\n"
            "4:2:2       Chroma is at half horizontal resolution.\n"
            "4:2:0       Chroma is at quarter resolution, which may cause minor color smearing.\n"
            "            However, this chroma format is most likely to be compatible with video editing software.");

    Pstring = secprop->Add_string("capture format", Property::Changeable::OnlyAtStart,"default");
    Pstring->Set_values(captureformats);
    Pstring->Set_help("Capture format to use when capturing video. The availability of the format depends on how DOSBox-X was compiled.\n"
            "default                     Use compiled-in default (avi-zmbv)\n"
            "avi-zmbv                    Use DOSBox-style AVI + ZMBV codec with PCM audio\n"
            "mpegts-h264                 Use MPEG transport stream + H.264 + AAC audio. Resolution & refresh rate changes can be contained\n"
            "                            within one file with this choice, however not all software can support mid-stream format changes.");

    Pint = secprop->Add_int("shell environment size",Property::Changeable::OnlyAtStart,0);
    Pint->SetMinMax(0,65280);
    Pint->Set_help("Size of the initial DOSBox shell environment block, in bytes. This does not affect the environment block of sub-processes spawned from the shell.\n"
            "This option has no effect unless dynamic kernel allocation is enabled.");

    Pint = secprop->Add_int("private area size",Property::Changeable::OnlyAtStart,32768); // DOSBox mainline compatible 32KB region
    Pint->SetMinMax(16,128*1024);
    Pint->Set_help("Set DOSBox-X private memory area size. This area contains private memory structures used by the DOS kernel.\n"
            "It is discarded when you boot into another OS. Mainline DOSBox uses 32KB. Testing shows that it is possible\n"
            "to run DOSBox with as little as 4KB. If DOSBox-X aborts with error \"not enough memory for internal tables\"\n"
            "then you need to increase this value.");

    // NOTE: This will be revised as I test the DOSLIB code against more VGA/SVGA hardware!
    Pstring = secprop->Add_string("vga attribute controller mapping",Property::Changeable::WhenIdle,"auto");
    Pstring->Set_values(vga_ac_mapping_settings);
    Pstring->Set_help(
            "This affects how the attribute controller maps colors, especially in 256-color mode.\n"
            "Some SVGA cards handle the attribute controller palette differently than most SVGA cards.\n"
            "  auto                         Automatically pick the mapping based on the SVGA chipset.\n"
            "  4x4                          Split into two 4-bit nibbles, map through AC, recombine. This is standard VGA behavior including clone SVGA cards.\n"
            "  4low                         Split into two 4-bit nibbles, remap only the low 4 bits, recombine. This is standard ET4000 behavior.\n"
            "\n"
            "NOTES:\n"
            "  Demoscene executable 'COPPER.EXE' requires the '4low' behavior in order to display line-fading effects\n"
            "  (including scrolling credits) correctly, else those parts of the demo show up as a blank screen.\n"
            "  \n"
            "  4low behavior is default for ET4000 emulation.");

    Pstring = secprop->Add_string("a20",Property::Changeable::WhenIdle,"mask");
    Pstring->Set_help("A20 gate emulation mode.\n"
              "The on/off/on_fake/off_fake options are intended for testing and debugging DOS development,\n"
              "or to emulate obscure hardware, or to work around potential extended memory problems with DOS programs.\n"
              "on_fake/off_fake are intended to test whether a program carries out a memory test to ensure the A20\n"
              "gate is set as intended (as HIMEM.SYS does). If it goes by the gate bit alone, it WILL crash.\n"
              "This parameter is also changeable from the builtin A20GATE command.\n"
              "  fast                         Emulate A20 gating by remapping the first 64KB @ 1MB boundary (fast, mainline DOSBox behavior)\n"
              "  mask                         Emulate A20 gating by masking memory I/O address (accurate)\n"
              "  off                          Lock A20 gate off (Software/OS cannot enable A20)\n"
              "  on                           Lock A20 gate on (Software/OS cannot disable A20)\n"
              "  off_fake                     Lock A20 gate off but allow bit to toggle (hope your DOS game tests the HMA!)\n"
              "  on_fake                      Lock A20 gate on but allow bit to toggle");

    Pbool = secprop->Add_bool("turn off a20 gate on boot",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("If enabled, A20 gate is switched off when booting a guest OS.\n"
                    "Enabled by default. Recommended for MS-DOS when HIMEM.SYS is not installed in the guest OS.\n"
                    "If disabled, and MS-DOS does not load HIMEM.SYS, programs and features that rely on the 1MB wraparound will fail.");

    /* Ref:
     *
     * "Except the first generation, which C-Bus was synchronous with its 5MHz 8086, PC-98s
     *  before the age of SuperIO and PCI use either 10MHz (9.8304MHz) or 8MHz (7.9872MHz)
     *  for its C-Bus.
     * 
     *  It's determined by the CPU clock base (2.4756Mhz or 1.9968MHz). For example, on a
     *  16MHz 386, C-Bus runs at 8MHz and on a 25MHz 386, C-Bus runs at 10MHz.
     *
     *  After NEC brought SuperIO and PCI to PC-98, C-Bus clock no longer ties to the CPU
     *  oscillator and got fixed to 10MHz." -Yksoft1
     *
     * Assuming this is true, the selection is given below */

    Pstring = secprop->Add_string("cbus bus clock",Property::Changeable::WhenIdle,"std10");
    Pstring->Set_help("C-BUS BCLK frequency (PC-98), used to emulate I/O delay.\n"
              "WARNING: In future revisions, PCI/motherboard chipset emulation will allow the guest OS/program to alter this value at runtime.\n"
              "  std10                        10MHz (CPU speed multiple of 5MHz or PCI-based)\n"
              "  std8                         8MHz (CPU speed multiple of 4MHz)\n"
              "  std5                         5MHz (older PC-9801)\n"
              "  <integer or float>           Any integer or floating point value will be used as the clock frequency in Hz\n"
              "  <integer/integer ratio>      If a ratio is given (num/den), the ratio will be used as the clock frequency");

    Pstring = secprop->Add_string("isa bus clock",Property::Changeable::WhenIdle,"std8.3");
    Pstring->Set_help("ISA BCLK frequency, used to emulate I/O delay.\n"
              "WARNING: In future revisions, PCI/motherboard chipset emulation will allow the guest OS/program to alter this value at runtime.\n"
              "  std8.3                       8.333MHz (typical 386-class or higher)\n"
              "  std8                         8MHz\n"
              "  std6                         6MHz\n"
              "  std4.77                      4.77MHz (precisely 1/3 x 14.31818MHz). Bus frequency of older PC/XT systems.\n"
              "  oc10                         10MHz\n"
              "  oc12                         12MHz\n"
              "  oc15                         15MHz\n"
              "  oc16                         16MHz\n"
              "  <integer or float>           Any integer or floating point value will be used as the clock frequency in Hz\n"
              "  <integer/integer ratio>      If a ratio is given (num/den), the ratio will be used as the clock frequency");

    Pstring = secprop->Add_string("pci bus clock",Property::Changeable::WhenIdle,"std33.3");
    Pstring->Set_help("PCI bus frequency, used to emulate I/O delay.\n"
              "WARNING: In future revisions, PCI/motherboard chipset emulation will allow the guest OS/program to alter this value at runtime.\n"
              "  std33.3                      33.333MHz (very common setting on motherboards)\n"
              "  std30                        30MHz (some older mid-1990's Pentium systems)\n"
              "  std25                        25MHz\n"
              "  <integer or float>           Any integer or floating point value will be used as the clock frequency in Hz\n"
              "  <integer/integer ratio>      If a ratio is given (num/den), the ratio will be used as the clock frequency");

    Pstring = secprop->Add_string("call binary on reset",Property::Changeable::WhenIdle,"");
    Pstring->Set_help("If set, this is the path of a binary blob to load into the ROM BIOS area and execute immediately after CPU reset.\n"
                      "It will be executed before the BIOS POST routine, only ONCE. The binary blob is expected either to IRET or to\n"
                      "jump directly to F000:FFF0 to return control to the BIOS.\n"
                      "This can be used for x86 assembly language experiments and automated testing against the CPU emulation.");

    Pstring = secprop->Add_string("unhandled irq handler",Property::Changeable::WhenIdle,"");
    Pstring->Set_values(irqhandler);
    Pstring->Set_help("Determines how unhandled IRQs are handled. This may help some errant DOS applications.\n"
                      "Leave unset for default behavior (simple).\n"
                      "simple               Acknowledge the IRQ, and the master (if slave IRQ)\n"
                      "mask_isr             Acknowledge IRQs in service on master and slave and mask IRQs still in service, to deal with errant handlers (em-dosbox method)");

    Pstring = secprop->Add_string("call binary on boot",Property::Changeable::WhenIdle,"");
    Pstring->Set_help("If set, this is the path of a binary blob to load into the ROM BIOS area and execute immediately before booting the DOS system.\n"
                      "This can be used for x86 assembly language experiments and automated testing against the CPU emulation.");

    Pint = secprop->Add_int("rom bios allocation max",Property::Changeable::OnlyAtStart,0);
    Pint->SetMinMax(0,128);
    Pint->Set_help("Maximum size (top down from 1MB) allowed for ROM BIOS dynamic allocation in KB");

    Pint = secprop->Add_int("rom bios minimum size",Property::Changeable::OnlyAtStart,0);
    Pint->SetMinMax(0,128);
    Pint->Set_help("Once ROM BIOS layout is finalized, trim total region down to a minimum amount in KB");

    Pint = secprop->Add_int("irq delay ns", Property::Changeable::WhenIdle,-1);
    Pint->SetMinMax(-1,100000);
    Pint->Set_help( "IRQ delay in nanoseconds. Set to -1 to use default, 0 to disable.\n"
                    "This is a more precise version of the irqdelay= setting.\n"
                    "There are some old DOS games and demos that have race conditions with IRQs that need a nonzero value here to work properly.");

    Pint = secprop->Add_int("iodelay", Property::Changeable::WhenIdle,-1);
    Pint->SetMinMax(-1,100000);
    Pint->Set_help( "I/O delay in nanoseconds for I/O port access. Set to -1 to use default, 0 to disable.\n"
            "A value of 1000 (1us) is recommended for ISA bus type delays. If your game\n"
            "or demo is not sensitive to I/O port and ISA bus timing, you can turn this option off\n"
            "(set to 0) to increase game performance.");

    Pint = secprop->Add_int("iodelay16", Property::Changeable::WhenIdle,-1);
    Pint->SetMinMax(-1,100000);
    Pint->Set_help( "I/O delay for 16-bit transfers. -1 to use default, 0 to disable.");

    Pint = secprop->Add_int("iodelay32", Property::Changeable::WhenIdle,-1);
    Pint->SetMinMax(-1,100000);
    Pint->Set_help( "I/O delay for 32-bit transfers. -1 to use default, 0 to disable.");

    // STUB OPTION, NOT YET FULLY IMPLEMENTED
    Pstring = secprop->Add_string("acpi", Property::Changeable::OnlyAtStart,"off");
    Pstring->Set_values(acpisettings);
    Pstring->Set_help("ACPI emulation, and what version of the specification to follow.\n"
            "WARNING: This option is very experimental at this time and should not be enabled unless you're willing to accept the consequences.\n"
            "         Intended for use with ACPI-aware OSes including Linux and Windows 98/ME. This option will also slightly reduce available\n"
            "         system memory to make room for the ACPI tables, just as real BIOSes do, and reserve an IRQ for ACPI functions.");

    // STUB OPTION, NOT YET FULLY IMPLEMENTED
    Pstring = secprop->Add_string("acpi rsd ptr location", Property::Changeable::OnlyAtStart,"auto");
    Pstring->Set_values(acpi_rsd_ptr_settings);
    Pstring->Set_help("Where to store the Root System Description Pointer structure. You can have it stored in the ROM BIOS area, or the Extended Bios Data Area.");

    // STUB OPTION, NOT YET FULLY IMPLEMENTED
    Pint = secprop->Add_int("acpi sci irq", Property::Changeable::WhenIdle,-1);
    Pint->Set_help("IRQ to assign as ACPI system control interrupt. set to -1 to automatically assign.");

    // STUB OPTION, NOT YET FULLY IMPLEMENTED
    Phex = secprop->Add_hex("acpi iobase",Property::Changeable::WhenIdle,0);
    Phex->Set_help("I/O port base for the ACPI device Power Management registers. Set to 0 for automatic assignment.");

    // STUB OPTION, NOT YET FULLY IMPLEMENTED
    Pint = secprop->Add_int("acpi reserved size", Property::Changeable::WhenIdle,0);
    Pint->Set_help("Amount of memory at top to reserve for ACPI structures and tables. Set to 0 for automatic assignment.");

#if defined(C_EMSCRIPTEN)
    Pint = secprop->Add_int("memsize", Property::Changeable::WhenIdle,4);
#else
    Pint = secprop->Add_int("memsize", Property::Changeable::WhenIdle,16);
#endif
    Pint->SetMinMax(1,3584); // 3.5GB
    Pint->Set_help(
        "Amount of memory DOSBox has in megabytes.\n"
        "This value is best left at its default to avoid problems with some games,\n"
        "though few games might require a higher value.\n"
        "There is generally no speed advantage when raising this value.n"
        "Programs that use 286 protected mode like Windows 3.0 in Standard Mode may crash with more than 15MB.");

    Pint = secprop->Add_int("memsizekb", Property::Changeable::WhenIdle,0);
    Pint->SetMinMax(0,524288);
    Pint->Set_help(
        "Amount of memory DOSBox has in kilobytes.\n"
        "This value should normally be set to 0.\n"
        "If nonzero, it is added to the memsize parameter.\n"
        "Finer grained control of total memory may be useful in\n"
        "emulating ancient DOS machines with less than 640KB of\n"
        "RAM or early 386 systems with odd extended memory sizes.");

    Pint = secprop->Add_int("dos mem limit", Property::Changeable::WhenIdle,0);
    Pint->SetMinMax(0,1023);
    Pint->Set_help( "Limit DOS conventional memory to this amount. Does not affect extended memory.\n"
            "Setting this option to a value in the range 636-639 can be used to simulate modern BIOSes\n"
            "that maintain an EBDA (Extended BIOS Data Area) at the top of conventional memory.\n"
            "You may also play with this option for diagnostic purposes or to stress test DOS programs in limited memory setups.\n"
            "\n"
            "A few DOS games & demos require this option to be set:\n"
            "     Majic 12 \"Show\": If UMBs are enabled, set this option to 639 to avoid MCB chain corruption error.");

    Pbool = secprop->Add_bool("isa memory hole at 512kb",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If set, emulate an ISA memory hole at the 512KB to 640KB area (0x80000-0x9FFFF).");

    Pint = secprop->Add_int("reboot delay", Property::Changeable::WhenIdle,-1);
    Pint->SetMinMax(-1,10000);
    Pint->Set_help(
        "Reboot delay. How long to pause at BIOS POST after reboot in milliseconds.\n"
        "This option is provided so that it is possible to see what the guest application\n"
        "or OS might have written to the screen before resetting the system. A value of\n"
        "-1 means to use a reasonable default.");

    Pint = secprop->Add_int("memalias", Property::Changeable::WhenIdle,0);
    Pint->SetMinMax(0,32);
    Pint->Set_help(
        "Memory aliasing emulation, in number of valid address bits.\n"
        "Many 386/486 class motherboards and processors prior to 1995\n"
        "suffered from memory aliasing for various technical reasons. If the software you are\n"
        "trying to run assumes aliasing, or otherwise plays cheap tricks with paging,\n"
        "enabling this option can help. Note that enabling this option can cause slight performance degredation. Set to 0 to disable.\n"
        "Recommended values when enabled:\n"
        "    24: 16MB aliasing. Common on 386SX systems (CPU had 24 external address bits)\n"
        "        or 386DX and 486 systems where the CPU communicated directly with the ISA bus (A24-A31 tied off)\n"
        "    26: 64MB aliasing. Some 486s had only 26 external address bits, some motherboards tied off A26-A31");

    Pbool = secprop->Add_bool("pc-98 BIOS copyright string",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If set, the PC-98 BIOS copyright string is placed at E800:0000. Enable this for software that detects PC-98 vs Epson.");

    Pbool = secprop->Add_bool("pc-98 int 1b fdc timer wait",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If set, INT 1Bh floppy access will wait for the timer to count down before returning.\n"
                    "This is needed for Ys II to run without crashing.");

    Pbool = secprop->Add_bool("pc-98 pic init to read isr",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("If set, the programmable interrupt controllers are initialized by default (if PC-98 mode)\n"
                    "so that the in-service interrupt status can be read immediately. There seems to be a common\n"
                    "convention in PC-98 games to program and/or assume this mode for cooperative interrupt handling.\n"
                    "This option is enabled by default for best compatibility with PC-98 games.");

    Pstring = secprop->Add_string("pc-98 fm board",Property::Changeable::Always,"auto");
    Pstring->Set_values(pc98fmboards);
    Pstring->Set_help("In PC-98 mode, selects the FM music board to emulate.");

    Pint = secprop->Add_int("pc-98 fm board irq", Property::Changeable::WhenIdle,0);
    Pint->Set_help("If set, helps to determine the IRQ of the FM board. A setting of zero means to auto-determine the IRQ.");

    Phex = secprop->Add_hex("pc-98 fm board io port", Property::Changeable::WhenIdle,0);
    Phex->Set_help("If set, helps to determine the base I/O port of the FM board. A setting of zero means to auto-determine the port number.");

    Pbool = secprop->Add_bool("pc-98 sound bios",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Set Sound BIOS enabled bit in MEMSW 4 for some games that require it.\n"
                    "TODO: Real emulation of PC-9801-26K/86 Sound BIOS");

    Pbool = secprop->Add_bool("pc-98 load sound bios rom file",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("If set, load SOUND.ROM if available and prsent that to the guest instead of trying to emulate directly.\n"
                    "This is strongly recommended, and is default enabled.\n"
                    "SOUND.ROM is a snapshot of the FM board BIOS taken from real PC-98 hardware.");

    Pbool = secprop->Add_bool("pc-98 buffer page flip",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If set, the game's request to page flip will be delayed to vertical retrace, which can eliminate tearline artifacts.\n"
                    "Note that this is NOT the behavior of actual hardware. This option is provided for the user's preference.");

    Pbool = secprop->Add_bool("pc-98 enable 256-color planar",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Allow 256-color planar graphics mode if set, disable if not set.\n"
                    "This is a form of memory access in 256-color mode that existed for a short\n"
                    "time before later PC-9821 models removed it. This option must be enabled\n"
                    "to use DOSBox-X with Windows 3.1 and it's built-in 256-color driver.");

    Pbool = secprop->Add_bool("pc-98 enable 256-color",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Allow 256-color graphics mode if set, disable if not set");

    Pbool = secprop->Add_bool("pc-98 enable 16-color",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Allow 16-color graphics mode if set, disable if not set");

    Pbool = secprop->Add_bool("pc-98 enable grcg",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Allow GRCG graphics functions if set, disable if not set");

    Pbool = secprop->Add_bool("pc-98 enable egc",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Allow EGC graphics functions if set, disable if not set");

    Pbool = secprop->Add_bool("pc-98 enable 188 user cg",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Allow 188+ user-defined CG cells if set");

    Pbool = secprop->Add_bool("pc-98 start gdc at 5mhz",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Start GDC at 5MHz if set, 2.5MHz if clear. May be required for some games.");

    Pbool = secprop->Add_bool("pc-98 allow scanline effect",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("If set, PC-98 emulation will allow the DOS application to enable the 'scanline effect'\n"
                    "in 200-line graphics modes upconverted to 400-line raster display. When enabled, odd\n"
                    "numbered scanlines are blanked instead of doubled");

    Pbool = secprop->Add_bool("pc-98 bus mouse",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Enable PC-98 bus mouse emulation. Disabling this option does not disable INT 33h emulation.");

    Pstring = secprop->Add_string("pc-98 video mode",Property::Changeable::WhenIdle,"");
    Pstring->Set_values(pc98videomodeopt);
    Pstring->Set_help("Specify the preferred PC-98 video mode.\n"
                      "Valid values are 15, 24, or 31 for each specific horizontal refresh rate on the platform.\n"
                      "24khz is default and best supported at this time.\n"
                      "15khz is not implemented at this time.\n"
                      "31khz is experimental at this time.");

    Pint = secprop->Add_int("pc-98 timer master frequency", Property::Changeable::WhenIdle,0);
    Pint->SetMinMax(0,2457600);
    Pint->Set_help("8254 timer clock frequency (NEC PC-98). Depending on the CPU frequency the clock frequency is one of two common values.\n"
                   "If your setting is neither of the below the closest appropriate value will be chosen.\n"
                   "This setting affects the master clock rate that DOS applications must divide down from to program the timer\n"
                   "at the correct rate, which affects timer interrupt, PC speaker, and the COM1 RS-232C serial port baud rate.\n"
                   "8MHz is treated as an alias for 4MHz and 10MHz is treated as an alias for 5MHz.\n"
                   "    0: Use default (auto)\n"
                   "    4: 1.996MHz (as if 4MHz or multiple thereof CPU clock)\n"
                   "    5: 2.457MHz (as if 5MHz or multiple thereof CPU clock)");

    Pint = secprop->Add_int("pc-98 allow 4 display partition graphics", Property::Changeable::WhenIdle,-1);
    Pint->SetMinMax(-1,1);
    Pint->Set_help("According to NEC graphics controller documentation, graphics mode is supposed to support only\n"
                   "2 display partitions. Some games rely on hardware flaws that allowed 4 partitions.\n"
                   "   -1: Default (choose automatically)\n"
                   "    0: Disable\n"
                   "    1: Enable");

    Pbool = secprop->Add_bool("pc-98 force ibm keyboard layout",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Force to use a default keyboard layout like IBM US-English for PC-98 emulation.\n"
                    "Will only work with apps and games using BIOS for keyboard.");

    Pint = secprop->Add_int("vga bios size override", Property::Changeable::WhenIdle,0);
    Pint->SetMinMax(512,65536);
    Pint->Set_help("VGA BIOS size override. Override the size of the VGA BIOS (normally 32KB in compatible or 12KB in non-compatible).");

    Pbool = secprop->Add_bool("video bios dont duplicate cga first half rom font",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If set, save 4KB of EGA/VGA ROM space by pointing to the copy in the ROM BIOS of the first 128 chars");

    Pbool = secprop->Add_bool("video bios always offer 14-pixel high rom font",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If set, video BIOS will always carry the 14-pixel ROM font. If clear, 14-pixel rom font will not be offered except for EGA/VGA emulation.");

    Pbool = secprop->Add_bool("video bios always offer 16-pixel high rom font",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If set, video BIOS will always carry the 16-pixel ROM font. If clear, 16-pixel rom font will not be offered except for VGA emulation.");

    Pbool = secprop->Add_bool("video bios enable cga second half rom font",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("If set, and emulating CGA/PCjr/Tandy, automatically provide the second half of the 8x8 ROM font.\n"
            "This setting is ignored for EGA/VGA emulation. If not set, you will need a utility like GRAFTABL.COM to load the second half of the ROM font for graphics.\n"
            "NOTE: if you disable the 14 & 16 pixel high font AND the second half when machine=cga, you will disable video bios completely.");

    Pstring = secprop->Add_string("forcerate",Property::Changeable::Always,"");
    Pstring->Set_help("Force the VGA framerate to a specific value(ntsc, pal, or specific hz), no matter what");

    Pbool = secprop->Add_bool("sierra ramdac",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Whether or not to emulate a Sierra or compatible RAMDAC at port 3C6h-3C9h.\n"
            "Some DOS games expect to access port 3C6h to enable highcolor/truecolor SVGA modes on older chipsets.\n"
            "Disable if you wish to emulate SVGA hardware that lacks a RAMDAC or (depending on the chipset) does\n"
            "not emulate a RAMDAC that is accessible through port 3C6h. This option has no effect for non-VGA video hardware.");

    Pbool = secprop->Add_bool("sierra ramdac lock 565",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("When emulating High Sierra highcolor RAMDAC, assume 5:6:5 at all times if set. Else,\n"
            "bit 6 of the DAC command selects between 5:5:5 and 5:6:5. Set this option for demos or\n"
            "games that got the command byte wrong (MFX Transgrassion 2) or any other demo that is\n"
            "not rendering highcolor 16bpp correctly.");

    Pbool = secprop->Add_bool("page flip debug line",Property::Changeable::Always,false);
    Pbool->Set_help("VGA debugging switch. If set, an inverse line will be drawn on the exact scanline that the CRTC display offset registers were written.\n"
            "This can be used to help diagnose whether or not the DOS game is page flipping properly according to vertical retrace if the display on-screen is flickering.");

    Pbool = secprop->Add_bool("vertical retrace poll debug line",Property::Changeable::Always,false);
    Pbool->Set_help("VGA debugging switch. If set, an inverse green dotted line will be drawn on the exact scanline that the CRTC status port (0x3DA) was read.\n"
            "This can be used to help diagnose whether the DOS game is propertly waiting for vertical retrace.");

    Pbool = secprop->Add_bool("cgasnow",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("When machine=cga, determines whether or not to emulate CGA snow in 80x25 text mode");

    /* Default changed to 0x04 for "Blues Brothers" at Allofich's request [https://github.com/joncampbell123/dosbox-x/issues/1273] */
    Phex = secprop->Add_hex("vga 3da undefined bits",Property::Changeable::WhenIdle,0x04);
    Phex->Set_help("VGA status port 3BA/3DAh only defines bits 0 and 3. This setting allows you to assign a bit pattern to the undefined bits.\n"
                   "The purpose of this hack is to deal with demos that read and handle port 3DAh in ways that might crash if all are zero.");

    Pbool = secprop->Add_bool("unmask timer on int 10 setmode",Property::Changeable::OnlyAtStart,false);
    Pbool->Set_help("If set, INT 10h will unmask IRQ 0 (timer) when setting video modes.");

    Pbool = secprop->Add_bool("unmask keyboard on int 16 read",Property::Changeable::OnlyAtStart,true);
    Pbool->Set_help("If set, INT 16h will unmask IRQ 1 (keyboard) when asked to read keyboard input.\n"
                    "It is strongly recommended that you set this option if running Windows 3.11 Windows for Workgroups in DOSBox-X.");

    Pbool = secprop->Add_bool("int16 keyboard polling undocumented cf behavior",Property::Changeable::OnlyAtStart,false);
    Pbool->Set_help("If set, INT 16h function AH=01h will also set/clear the carry flag depending on whether input was available.\n"
                    "There are some old DOS games and demos that rely on this behavior to sense keyboard input, and this behavior\n"
                    "has been verified to occur on some old (early 90s) BIOSes.");

    Pbool = secprop->Add_bool("allow port 92 reset",Property::Changeable::OnlyAtStart,true);
    Pbool->Set_help("If set (default), allow the application to reset the CPU through port 92h");

    Pbool = secprop->Add_bool("enable port 92",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Emulate port 92h (PS/2 system control port A). If you want to emulate a system that pre-dates the PS/2, set to 0.");

    Pbool = secprop->Add_bool("enable 1st dma controller",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Emulate 1st (AT) DMA controller (default). Set to 0 if you wish to emulate a system that lacks DMA (PCjr and some Tandy systems)");

    Pbool = secprop->Add_bool("enable 2nd dma controller",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Emulate 2nd (AT) DMA controller (default). Set to 0 if you wish to emulate a PC/XT system without 16-bit DMA.\n"
            "Note: mainline DOSBox automatically disables 16-bit DMA when machine=cga or machine=hercules, while DOSBox-X does not.");

    Pbool = secprop->Add_bool("allow dma address decrement",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("If set, allow increment & decrement modes as specified in the 8237 datasheet.\n"
            "If clear, always increment the address (as if to emulate clone 8237 implementations that skipped the inc/dec bit).");

    Pstring = secprop->Add_string("enable 128k capable 16-bit dma", Property::Changeable::OnlyAtStart,"auto");
    Pstring->Set_values(truefalseautoopt);
    Pstring->Set_help("If true, DMA controller emulation models ISA hardware that permits 16-bit DMA to span 128KB.\n"
                    "If false, DMA controller emulation models PCI hardware that limits 16-bit DMA to 64KB boundaries.\n"
                    "If auto, the choice is made according to other factors in hardware emulation");

    Pbool = secprop->Add_bool("enable dma extra page registers",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("If set, emulate the extra page registers (I/O ports 0x80, 0x84-0x86, 0x88, 0x8C-0x8E), like actual hardware.\n"
            "Note that mainline DOSBox behavior is to NOT emulate these registers.");

    Pbool = secprop->Add_bool("dma page registers write-only",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Normally (on AT hardware) the DMA page registers are read/write. Set this option if you want to emulate PC/XT hardware where the page registers are write-only.");

    Pbool = secprop->Add_bool("cascade interrupt never in service",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If set, PIC emulation will never mark cascade interrupt as in service. This is OFF by default. It is a hack for troublesome games.");

    // TODO: "Special mode" which apparently triggers this alternate behavior and used by default on PC-98, is configurable
    //       by software through the PIC control words, and should control this setting if this is "auto".
    //       It's time for "auto" default setting to end once and for all the running gag that PC-98 games will not run
    //       properly without having to add "cascade interrupt ignore in service=true" to your dosbox.conf all the time.
    Pstring = secprop->Add_string("cascade interrupt ignore in service",Property::Changeable::WhenIdle,"auto");
    Pstring->Set_values(truefalseautoopt);
    Pstring->Set_help("If true, PIC emulation will allow slave pic interrupts even if the cascade interrupt is still \"in service\" (common PC-98 behavior)\n"
                    "If false, PIC emulation will consider cascade in-service state when deciding which interrupt to signal (common IBM PC behavior)\n"
                    "If auto, setting is chosen based on machine type and other configuration.");

    Pbool = secprop->Add_bool("enable slave pic",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Enable slave PIC (IRQ 8-15). Set this to 0 if you want to emulate a PC/XT type arrangement with IRQ 0-7 and no IRQ 2 cascade.");

    Pbool = secprop->Add_bool("enable pc nmi mask",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Enable PC/XT style NMI mask register (0xA0). Note that this option conflicts with the secondary PIC and will be ignored if the slave PIC is enabled.");

    Pbool = secprop->Add_bool("rom bios 8x8 CGA font",Property::Changeable::Always,true);
    Pbool->Set_help("If set, or mainline compatible bios mapping, a legacy 8x8 CGA font (first 128 characters) is stored at 0xF000:0xFA6E. DOS programs that do not use INT 10h to locate fonts might require that font to be located there.");

    Pbool = secprop->Add_bool("rom bios video parameter table",Property::Changeable::Always,true);
    Pbool->Set_help("If set, or mainline compatible bios mapping, DOSBox will emulate the video parameter table and assign that to INT 1Dh. If clear, table will not be provided.");

    Pbool = secprop->Add_bool("allow more than 640kb base memory",Property::Changeable::Always,false);
    Pbool->Set_help("If set, and space is available, allow conventional memory to extend past 640KB.\n"
            "For example, if machine=cga, conventional memory can extend out to 0xB800 and provide up to 736KB of RAM.\n"
            "This allows you to emulate PC/XT style memory extensions.");

    Pbool = secprop->Add_bool("vesa zero buffer on get information",Property::Changeable::Always,true);
    Pbool->Set_help("This setting affects VESA BIOS function INT 10h AX=4F00h. If set, the VESA BIOS will zero the\n"
                    "256-byte buffer defined by the standard at ES:DI, then fill in the structure. If clear, only\n"
                    "the structure members will be filled in, and memory outside the initial 20-32 bytes will remain\n"
                    "unmodified. This setting is ON by default. Some very early 1990s DOS games that support VESA\n"
                    "BIOS standards may need this setting turned OFF if the programmer did not provide enough space\n"
                    "for the entire 256 byte structure and the game crashes if it detects VESA BIOS extensions.\n"
                    "Needed for:\n"
                    "  GETSADAM.EXE");

    /* should be set to zero unless for very specific demos:
     *  - "Melvindale" by MFX (1996): Set this to 2, the nightmarish visual rendering code appears to draw 2 scanlines
     *    upward from the VESA linear framebuffer base we return, causing DOSBox to emit warnings about illegal read/writes
     *    from 0xBFFFF000-0xBFFFFFFF (just before the base of the framebuffer at 0xC0000000). It also has the effect of
     *    properly centering the picture on the screen. I suppose it's a miracle the demo didn't crash people's computers
     *    writing to undefined areas like that. */
    Pint = secprop->Add_int("vesa lfb base scanline adjust",Property::Changeable::WhenIdle,0);
    Pint->Set_help("If non-zero, the VESA BIOS will report the linear framebuffer offset by this many scanlines.\n"
            "This does not affect the linear framebuffer's location. It only affects the linear framebuffer\n"
            "location reported by the VESA BIOS. Set to nonzero for DOS games with sloppy VESA graphics pointer management.\n"
            "    MFX \"Melvindale\" (1996): Set this option to 2 to center the picture properly.");

    /* If set, all VESA BIOS modes map 128KB of video RAM at A0000-BFFFF even though VESA BIOS emulation
     * reports a 64KB window. Some demos like the 1996 Wired report
     * (ftp.scene.org/pub/parties/1995/wired95/misc/e-w95rep.zip) assume they can write past the window
     * by spilling into B0000 without bank switching. */
    Pbool = secprop->Add_bool("vesa map non-lfb modes to 128kb region",Property::Changeable::Always,false);
    Pbool->Set_help("If set, VESA BIOS SVGA modes will be set to map 128KB of video memory to A0000-BFFFF instead of\n"
                    "64KB at A0000-AFFFF. This does not affect the SVGA window size or granularity.\n"
                    "Some games or demoscene productions assume that they can render into the next SVGA window/bank\n"
                    "by writing to video memory beyond the current SVGA window address and will not appear correctly\n"
                    "without this option.");

    Pbool = secprop->Add_bool("allow hpel effects",Property::Changeable::Always,false);
    Pbool->Set_help("If set, allow the DOS demo or program to change the horizontal pel (panning) register per scanline.\n"
            "Some early DOS demos use this to create waving or sinus effects on the picture. Not very many VGA\n"
            "chipsets allow this, so far, only ATI chipsets are known to support this effect. Disabled by default.");

    Pbool = secprop->Add_bool("allow hretrace effects",Property::Changeable::Always,false);
    Pbool->Set_help("If set, allow the DOS demo or program to make the picture wavy by playing with the 'start horizontal"
            "retrace' register of the CRTC during the active picture. Some early DOS demos (Copper by Surprise!"
            "productions) need this option set for some demo effects to work. Disabled by default.");

    Pdouble = secprop->Add_double("hretrace effect weight",Property::Changeable::Always,4.0);
    Pdouble->Set_help("If emulating hretrace effects, this parameter adds 'weight' to the offset to smooth it out.\n"
            "the larger the number, the more averaging is applied. This is intended to emulate the inertia\n"
            "of the electron beam in a CRT monitor");

    Pint = secprop->Add_int("vesa modelist cap",Property::Changeable::Always,0);
    Pint->Set_help("IF nonzero, the VESA modelist is capped so that it contains no more than the specified number of video modes.\n"
            "Set this option to a value between 8 to 32 if the DOS application has problems with long modelists or a fixed\n"
            "buffer for querying modes. Such programs may crash if given the entire modelist supported by DOSBox-X.\n"
            "  Warcraft II by Blizzard ................ Set to a value between 8 and 16. This game has a fixed buffer that it\n"
            "                                           reads the modelist into. DOSBox-X's normal modelist is too long and\n"
            "                                           the game will overrun the buffer and crash without this setting.");

    Pint = secprop->Add_int("vesa modelist width limit",Property::Changeable::Always,0);
    Pint->Set_help("IF nonzero, VESA modes with horizontal resolution higher than the specified pixel count will not be listed.\n"
            "This is another way the modelist can be capped for DOS applications that have trouble with long modelists.");

    Pint = secprop->Add_int("vesa modelist height limit",Property::Changeable::Always,0);
    Pint->Set_help("IF nonzero, VESA modes with vertical resolution higher than the specified pixel count will not be listed.\n"
            "This is another way the modelist can be capped for DOS applications that have trouble with long modelists.");

    Pbool = secprop->Add_bool("vesa vbe put modelist in vesa information",Property::Changeable::Always,false);
    Pbool->Set_help("If set, the VESA modelist is placed in the VESA information structure itself when the DOS application\n"
                    "queries information on the VESA BIOS. Setting this option may help with some games, though it limits\n"
                    "the mode list reported to the DOS application.");

    Pbool = secprop->Add_bool("vesa vbe 1.2 modes are 32bpp",Property::Changeable::Always,true);
    Pbool->Set_help("If set, truecolor (16M color) VESA BIOS modes in the 0x100-0x11F range are 32bpp. If clear, they are 24bpp.\n"
            "Some DOS games and demos assume one bit depth or the other and do not enumerate VESA BIOS modes, which is why this\n"
            "option exists.");

    Pbool = secprop->Add_bool("allow low resolution vesa modes",Property::Changeable::Always,true);
    Pbool->Set_help("If set, allow low resolution VESA modes (320x200x16/24/32bpp and so on). You could set this to false to simulate\n"
            "SVGA hardware with a BIOS that does not support the lowres modes for testing purposes.");

    Pbool = secprop->Add_bool("allow 32bpp vesa modes",Property::Changeable::Always,true);
    Pbool->Set_help("If the DOS game or demo has problems with 32bpp VESA modes, set to 'false'");

    Pbool = secprop->Add_bool("allow 24bpp vesa modes",Property::Changeable::Always,true);
    Pbool->Set_help("If the DOS game or demo has problems with 24bpp VESA modes, set to 'false'");

    Pbool = secprop->Add_bool("allow 16bpp vesa modes",Property::Changeable::Always,true);
    Pbool->Set_help("If the DOS game or demo has problems with 16bpp VESA modes, set to 'false'");

    Pbool = secprop->Add_bool("allow 15bpp vesa modes",Property::Changeable::Always,true);
    Pbool->Set_help("If the DOS game or demo has problems with 15bpp VESA modes, set to 'false'");

    Pbool = secprop->Add_bool("allow 8bpp vesa modes",Property::Changeable::Always,true);
    Pbool->Set_help("If the DOS game or demo has problems with 8bpp VESA modes, set to 'false'");

    Pbool = secprop->Add_bool("allow 4bpp vesa modes",Property::Changeable::Always,true);
    Pbool->Set_help("If the DOS game or demo has problems with 4bpp VESA modes, set to 'false'");

    Pbool = secprop->Add_bool("allow 4bpp packed vesa modes",Property::Changeable::Always,true);
    Pbool->Set_help("If the DOS game or demo has problems with 4bpp packed VESA modes, set to 'false'");

    Pbool = secprop->Add_bool("allow tty vesa modes",Property::Changeable::Always,true);
    Pbool->Set_help("If the DOS game or demo has problems with text VESA modes, set to 'false'");

    Pbool = secprop->Add_bool("double-buffered line compare",Property::Changeable::Always,false);
    Pbool->Set_help("This setting affects the VGA Line Compare register. Set to false (default value) to emulate most VGA behavior\n"
            "Set to true for the value to latch once at the start of the frame.");

    Pbool = secprop->Add_bool("ignore vblank wraparound",Property::Changeable::Always,false);
    Pbool->Set_help("DOSBox-X can handle active display properly if games or demos reprogram vertical blanking to end in the active picture area.\n"
            "If the wraparound handling prevents the game from displaying properly, set this to false. Out of bounds vblank values will be ignored.\n");

    Pbool = secprop->Add_bool("enable vga resize delay",Property::Changeable::Always,false);
    Pbool->Set_help("If the DOS game you are running relies on certain VGA raster tricks that affect active display area, enable this option.\n"
            "This adds a delay between VGA mode changes and window updates. It also means that if you are capturing a demo or game,\n"
            "that your capture will also show a few garbled frames at any point mode changes occur, which is why this option is disabled\n"
            "by default. If you intend to run certain DOS games and demos like DoWhackaDo, enable this option.");

    Pbool = secprop->Add_bool("resize only on vga active display width increase",Property::Changeable::Always,false);
    Pbool->Set_help("If set, changes to the Display End register of the CRTC do not trigger DOSBox to resize it's window\n"
            "IF the value written is less than the current value. Some demos like DoWhackaDo need this option set\n"
            "because of the way it's raster effects work. If the DOSBox window rapidly changes size during a demo\n"
            "try setting this option. Else, leave it turned off. Changes to other VGA CRTC registers will trigger\n"
            "a DOSBox mode change as normal regardless of this setting.");

    Pbool = secprop->Add_bool("enable pci bus",Property::Changeable::OnlyAtStart,true);
    Pbool->Set_help("Enable PCI bus emulation");

    Pbool = secprop->Add_bool("vga palette update on full load",Property::Changeable::Always,true);
    Pbool->Set_help("If set, all three bytes of the palette entry must be loaded before taking the color,\n"
                    "which is fairly typical SVGA behavior. If not set, partial changes are allowed.");

    Pbool = secprop->Add_bool("ignore odd-even mode in non-cga modes",Property::Changeable::Always,false);
    Pbool->Set_help("Some demoscene productions use VGA Mode X but accidentally enable odd/even mode.\n"
                    "Setting this option can correct for that and render the demo properly.\n"
                    "This option forces VGA emulation to ignore odd/even mode except in text and CGA modes.");

    secprop=control->AddSection_prop("render",&Null_Init,true);
    Pint = secprop->Add_int("frameskip",Property::Changeable::Always,0);
    Pint->SetMinMax(0,10);
    Pint->Set_help("How many frames DOSBox skips before drawing one.");

    Pbool = secprop->Add_bool("alt render",Property::Changeable::Always,false);
    Pbool->Set_help("If set, use a new experimental rendering engine");

    Pstring = secprop->Add_string("aspect", Property::Changeable::Always, "false");
    Pstring->Set_values(aspectmodes);
    Pstring->Set_help(
        "Aspect ratio correction mode. Can be set to the following values:\n"
        "  'false' (default):\n"
        "      'direct3d'/opengl outputs: image is simply scaled to full window/fullscreen size, possibly resulting in disproportional image\n"
        "      'surface' output: it does no aspect ratio correction (default), resulting in disproportional images if VGA mode pixel ratio is not 4:3\n"
        "  'true':\n"
        "      'direct3d'/opengl outputs: uses output driver functions to scale / pad image with black bars, correcting output to proportional 4:3 image\n"
        "          In most cases image degradation should not be noticeable (it all depends on the video adapter and how much the image is upscaled).\n"
        "          Should have none to negligible impact on performance, mostly being done in hardware\n"
        "      'surface' output: inherits old DOSBox aspect ratio correction method (adjusting rendered image line count to correct output to 4:3 ratio)\n"
        "          Due to source image manipulation this mode does not mix well with scalers, i.e. multiline scalers like hq2x/hq3x will work poorly\n"
        "          Slightly degrades visual image quality. Has a tiny impact on performance"
#if C_XBRZ
        "\n"
        "          When using xBRZ scaler with 'surface' output, aspect ratio correction is done by the scaler itself, so none of the above apply"
#endif
#if C_SURFACE_POSTRENDER_ASPECT
        "\n"
        "  'nearest':\n"
        "      'direct3d'/opengl outputs: not available, fallbacks to 'true' mode automatically\n"
        "      'surface' output: scaler friendly aspect ratio correction, works by rescaling rendered image using nearest neighbor scaler\n"
        "          Complex scalers work. Image quality is on par with 'true' mode (and better with scalers). More CPU intensive than 'true' mode\n"
#if C_XBRZ
        "          When using xBRZ scaler with 'surface' output, aspect ratio correction is done by the scaler itself, so it fallbacks to 'true' mode\n"
#endif
        "  'bilinear':\n"
        "      'direct3d'/opengl outputs: not available, fallbacks to 'true' mode automatically\n"
        "      'surface' output: scaler friendly aspect ratio correction, works by rescaling rendered image using bilinear scaler\n"
        "          Complex scalers work. Image quality is much better, should be on par with using 'direct3d' output + 'true' mode\n"
        "          Very CPU intensive, high end CPU may be required"
#if C_XBRZ
        "\n"
        "          When using xBRZ scaler with 'surface' output, aspect ratio correction is done by the scaler itself, so it fallbacks to 'true' mode"
#endif
#endif
    );

    Pbool = secprop->Add_bool("char9",Property::Changeable::Always,true);
    Pbool->Set_help("Allow 9-pixel wide text mode fonts.");

    /* NTS: In the original code borrowed from yhkong, this was named "multiscan". All it really does is disable
     *      the doublescan down-rezzing DOSBox normally does with 320x240 graphics so that you get the full rendition of what a VGA output would emit. */
    Pbool = secprop->Add_bool("doublescan",Property::Changeable::Always,true);
    Pbool->Set_help("If set, doublescanned output emits two scanlines for each source line, in the\n"
            "same manner as the actual VGA output (320x200 is rendered as 640x400 for example).\n"
            "If clear, doublescanned output is rendered at the native source resolution (320x200 as 320x200).\n"
            "This affects the raster PRIOR to the software or hardware scalers. Choose wisely.\n");

    Pmulti = secprop->Add_multi("scaler",Property::Changeable::Always," ");
    Pmulti->SetValue("normal2x",/*init*/true);
    Pmulti->Set_help("Scaler used to enlarge/enhance low resolution modes. If 'forced' is appended,\n"
                     "then the scaler will be used even if the result might not be desired.\n"
                     "To fit a scaler in the resolution used at full screen may require a border or side bars.\n"
                     "To fill the screen entirely, depending on your hardware, a different scaler/fullresolution might work.");
    Pstring = Pmulti->GetSection()->Add_string("type",Property::Changeable::Always,"normal2x");
    Pstring->Set_values(scalers);

    Pstring = Pmulti->GetSection()->Add_string("force",Property::Changeable::Always,"");
    Pstring->Set_values(force);

#if C_XBRZ
    Pint = secprop->Add_int("xbrz slice",Property::Changeable::OnlyAtStart,16);
    Pint->SetMinMax(1,1024);
    Pint->Set_help("Number of screen lines to process in single xBRZ scaler taskset task, affects xBRZ performance, 16 is the default");

    Pint = secprop->Add_int("xbrz fixed scale factor",Property::Changeable::OnlyAtStart, 0);
    Pint->SetMinMax(0,6);
    Pint->Set_help("To use fixed xBRZ scale factor (i.e. to attune performance), set it to 2-6, 0 - use automatic calculation (default)");

    Pint = secprop->Add_int("xbrz max scale factor",Property::Changeable::OnlyAtStart, 0);
    Pint->SetMinMax(0,6);
    Pint->Set_help("To cap maximum xBRZ scale factor used (i.e. to attune performance), set it to 2-6, 0 - use scaler allowed maximum (default)");
#endif

    Pbool = secprop->Add_bool("autofit",Property::Changeable::Always,true);
    Pbool->Set_help(
        "Best fits image to window\n"
        "- Intended for output=direct3d, fullresolution=original, aspect=true");

    Pmulti = secprop->Add_multi("monochrome_pal",Property::Changeable::Always," ");
    Pmulti->SetValue("green",/*init*/true);
    Pmulti->Set_help("Specify the color of monochrome display.\n"
        "Possible values: green, amber, gray, white\n"
        "Append 'bright' for a brighter look.");
    Pstring = Pmulti->GetSection()->Add_string("color",Property::Changeable::Always,"green");
    const char* monochrome_pal_colors[]={
      "green","amber","gray","white",0
    };
    Pstring->Set_values(monochrome_pal_colors);
    Pstring = Pmulti->GetSection()->Add_string("bright",Property::Changeable::Always,"");
    const char* bright[] = { "", "bright", 0 };
    Pstring->Set_values(bright);

    secprop=control->AddSection_prop("vsync",&Null_Init,true);//done

    Pstring = secprop->Add_string("vsyncmode",Property::Changeable::WhenIdle,"off");
    Pstring->Set_values(vsyncmode);
    Pstring->Set_help("Synchronize vsync timing to the host display. Requires calibration within dosbox.");
    Pstring = secprop->Add_string("vsyncrate",Property::Changeable::WhenIdle,"75");
    Pstring->Set_values(vsyncrate);
    Pstring->Set_help("Vsync rate used if vsync is enabled. Ignored if vsyncmode is set to host (win32).");

    secprop=control->AddSection_prop("cpu",&Null_Init,true);//done
    Pstring = secprop->Add_string("core",Property::Changeable::WhenIdle,"auto");
    Pstring->Set_values(cores);
    Pstring->Set_help("CPU Core used in emulation. auto will switch to dynamic if available and appropriate.\n"
            "WARNING: Do not use dynamic or auto setting core with Windows 95 or other preemptive\n"
            "multitasking OSes with protected mode paging, you should use the normal core instead.");

    Pbool = secprop->Add_bool("fpu",Property::Changeable::Always,true);
    Pbool->Set_help("Enable FPU emulation");

    Pbool = secprop->Add_bool("segment limits",Property::Changeable::Always,true);
    Pbool->Set_help("Enforce segment limits");

    Pbool = secprop->Add_bool("double fault",Property::Changeable::Always,true);
    Pbool->Set_help("Emulate double fault exception");

    Pbool = secprop->Add_bool("reset on triple fault",Property::Changeable::Always,true);
    Pbool->Set_help("Reset CPU on triple fault condition (failure to handle double fault)");

    Pbool = secprop->Add_bool("always report double fault",Property::Changeable::Always,false);
    Pbool->Set_help("Always report (to log file) double faults if set. Else, a double fault is reported only once. Set this option for debugging purposes.");

    Pbool = secprop->Add_bool("always report triple fault",Property::Changeable::Always,false);
    Pbool->Set_help("Always report (to log file) triple faults if set. Else, a triple fault is reported only once. Set this option for debugging purposes.");

    Pbool = secprop->Add_bool("enable msr",Property::Changeable::Always,true);
    Pbool->Set_help("Allow RDMSR/WRMSR instructions. This option is only meaningful when cputype=pentium.\n"
            "WARNING: Leaving this option enabled while installing Windows 95/98/ME can cause crashes.");

    Pbool = secprop->Add_bool("enable cmpxchg8b",Property::Changeable::Always,true);
    Pbool->Set_help("Enable Pentium CMPXCHG8B instruction. Enable this explicitly if using software that uses this instruction.\n"
            "You must enable this option to run Windows ME because portions of the kernel rely on this instruction.");

    Pbool = secprop->Add_bool("ignore undefined msr",Property::Changeable::Always,false);
    Pbool->Set_help("Ignore RDMSR/WRMSR on undefined registers. Normally the CPU will fire an Invalid Opcode exception in that case.\n"
            "This option is off by default, enable if using software or drivers that assumes the presence of\n"
            "certain MSR registers without checking. If you are using certain versions of the 3Dfx glide drivers for MS-DOS\n"
            "you will need to set this to TRUE as 3Dfx appears to have coded GLIDE2.OVL to assume the presence\n"
            "of Pentium Pro/Pentium II MTRR registers.\n"
            "WARNING: Leaving this option enabled while installing Windows 95/98/ME can cause crashes.");

    /* NTS: This setting is honored by all cpu cores except dynamic core */
    Pint = secprop->Add_int("interruptible rep string op",Property::Changeable::Always,-1);
    Pint->SetMinMax(-1,65536);
    Pint->Set_help("if nonzero, REP string instructions (LODS/MOVS/STOS/INS/OUTS) are interruptible (by interrupts or other events).\n"
            "if zero, REP string instructions are carried out in full before processing events and interrupts.\n"
            "Set to -1 for a reasonable default setting based on cpu type and other configuration.\n"
            "A setting of 0 can improve emulation speed at the expense of emulation accuracy.\n"
            "A nonzero setting (1-8) may be needed for DOS games and demos that use the IRQ 0 interrupt to play digitized samples\n"
            "while doing VGA palette animation at the same time (use case of REP OUTS), where the non-interruptible version\n"
            "would cause an audible drop in audio pitch.");

    Pint = secprop->Add_int("dynamic core cache block size",Property::Changeable::Always,32);
    Pint->SetMinMax(1,65536);
    Pint->Set_help("dynamic core cache block size. default value is 32. change this value carefully.\n"
            "according to forum discussion, setting this to 1 can aid debugging, however doing so\n"
            "also causes problems with 32-bit protected mode DOS games and reduces the performance\n"
            "of the dynamic core.\n");

    Pstring = secprop->Add_string("cputype",Property::Changeable::Always,"auto");
    Pstring->Set_values(cputype_values);
    Pstring->Set_help("CPU Type used in emulation. auto emulates a 486 which tolerates Pentium instructions.");

    Pmulti_remain = secprop->Add_multiremain("cycles",Property::Changeable::Always," ");
    Pmulti_remain->Set_help(
        "Amount of instructions DOSBox tries to emulate each millisecond.\n"
        "Setting this value too high results in sound dropouts and lags.\n"
        "Cycles can be set in 3 ways:\n"
        "  'auto'          tries to guess what a game needs.\n"
        "                  It usually works, but can fail for certain games.\n"
        "  'fixed #number' will set a fixed amount of cycles. This is what you usually\n"
        "                  need if 'auto' fails (Example: fixed 4000).\n"
        "  'max'           will allocate as much cycles as your computer is able to\n"
        "                  handle.");

    Pstring = Pmulti_remain->GetSection()->Add_string("type",Property::Changeable::Always,"auto");
    Pmulti_remain->SetValue("auto",/*init*/true);
    Pstring->Set_values(cyclest);

    Pmulti_remain->GetSection()->Add_string("parameters",Property::Changeable::Always,"");

    Pint = secprop->Add_int("cycleup",Property::Changeable::Always,10);
    Pint->SetMinMax(1,1000000);
    Pint->Set_help("Amount of cycles to decrease/increase with keycombos.(CTRL-F11/CTRL-F12)");

    Pint = secprop->Add_int("cycledown",Property::Changeable::Always,20);
    Pint->SetMinMax(1,1000000);
    Pint->Set_help("Setting it lower than 100 will be a percentage.");

    Pbool = secprop->Add_bool("use dynamic core with paging on",Property::Changeable::Always,true);
    Pbool->Set_help("Allow dynamic core with 386 paging enabled. This is generally OK for DOS games and Windows 3.1.\n"
                    "If the game becomes unstable, turn off this option.\n"
                    "WARNING: Do NOT use this option with preemptive multitasking OSes including Windows 95 and Windows NT.");
            
    Pbool = secprop->Add_bool("ignore opcode 63",Property::Changeable::Always,true);
    Pbool->Set_help("When debugging, do not report illegal opcode 0x63.\n"
            "Enable this option to ignore spurious errors while debugging from within Windows 3.1/9x/ME");

    Pbool = secprop->Add_bool("apmbios",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Emulate Advanced Power Management BIOS calls");

    Pbool = secprop->Add_bool("apmbios pnp",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If emulating ISA PnP BIOS, announce APM BIOS in PnP enumeration.\n"
            "Warning: this can cause Windows 95 OSR2 and later to enumerate the APM BIOS twice and cause problems.");

    Pstring = secprop->Add_string("apmbios version",Property::Changeable::WhenIdle,"auto");
    Pstring->Set_values(apmbiosversions);
    Pstring->Set_help("What version of the APM BIOS specification to emulate.\n"
            "You will need at least APM BIOS v1.1 for emulation to work with Windows 95/98/ME");

    Pbool = secprop->Add_bool("apmbios allow realmode",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Allow guest OS to connect from real mode.");

    Pbool = secprop->Add_bool("apmbios allow 16-bit protected mode",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Allow guest OS to connect from 16-bit protected mode.");

    Pbool = secprop->Add_bool("apmbios allow 32-bit protected mode",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Allow guest OS to connect from 32-bit protected mode.\n"
            "If you want power management in Windows 95/98/ME (beyond using the APM to shutdown the computer) you MUST enable this option.\n"
            "Windows 95/98/ME does not support the 16-bit real and protected mode APM BIOS entry points.\n"
            "Please note at this time that 32-bit APM is unstable under Windows ME");

    Pbool = secprop->Add_bool("integration device",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Enable DOSBox integration I/O device. This can be used by the guest OS to match mouse pointer position, for example. EXPERIMENTAL!");

    Pbool = secprop->Add_bool("integration device pnp",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("List DOSBox integration I/O device as part of ISA PnP enumeration. This has no purpose yet.");

    Pbool = secprop->Add_bool("isapnpbios",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Emulate ISA Plug & Play BIOS. Enable if using DOSBox to run a PnP aware DOS program or if booting Windows 9x.\n"
            "Do not disable if Windows 9x is configured around PnP devices, you will likely confuse it.");

    Pbool = secprop->Add_bool("realbig16",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Allow the B (big) bit in real mode. If set, allow the DOS program to set the B bit,\n"
        "then jump to realmode with B still set (aka Huge Unreal mode). Needed for Project Angel.");

    secprop=control->AddSection_prop("keyboard",&Null_Init);
    Pbool = secprop->Add_bool("aux",Property::Changeable::OnlyAtStart,true);
    Pbool->Set_help("Enable emulation of the 8042 auxiliary port. PS/2 mouse emulation requires this to be enabled.\n"
            "You should enable this if you will be running Windows ME or any other OS that does not use the BIOS to receive mouse events.");

    Pbool = secprop->Add_bool("allow output port reset",Property::Changeable::OnlyAtStart,true);
    Pbool->Set_help("If set (default), allow the application to reset the CPU through the keyboard controller.\n"
            "This option is required to allow Windows ME to reboot properly, whereas Windows 9x and earlier\n"
            "will reboot without this option using INT 19h");

    Pstring = secprop->Add_string("controllertype",Property::Changeable::OnlyAtStart,"auto");
    Pstring->Set_values(controllertypes);
    Pstring->Set_help("Type of keyboard controller (and keyboard) attached.\n"
                      "auto     Automatically pick according to machine type\n"
                      "at       AT (PS/2) type keyboard\n"
                      "xt       IBM PC/XT type keyboard\n"
                      "pcjr     IBM PCjr type keyboard (only if machine=pcjr)\n"
                      "pc98     PC-98 keyboard emulation (only if machine=pc98)");

    Pstring = secprop->Add_string("auxdevice",Property::Changeable::OnlyAtStart,"intellimouse");
    Pstring->Set_values(auxdevices);
    Pstring->Set_help("Type of PS/2 mouse attached to the AUX port");

    secprop=control->AddSection_prop("pci",&Null_Init,false); //PCI bus

    Pstring = secprop->Add_string("voodoo",Property::Changeable::WhenIdle,"auto");
    Pstring->Set_values(voodoo_settings);
    Pstring->Set_help("Enable VOODOO support.");

    secprop=control->AddSection_prop("mixer",&Null_Init);
    Pbool = secprop->Add_bool("nosound",Property::Changeable::OnlyAtStart,false);
    Pbool->Set_help("Enable silent mode, sound is still emulated though.");

    Pbool = secprop->Add_bool("sample accurate",Property::Changeable::OnlyAtStart,false);
    Pbool->Set_help("Enable sample accurate mixing, at the expense of some emulation performance. Enable this option for DOS games and demos that\n"
            "require such accuracy for correct Tandy/OPL output including digitized speech. This option can also help eliminate minor\n"
            "errors in Gravis Ultrasound emulation that result in random echo/attenuation effects.");

    Pbool = secprop->Add_bool("swapstereo",Property::Changeable::OnlyAtStart,false); 
    Pbool->Set_help("Swaps the left and right stereo channels."); 

    Pint = secprop->Add_int("rate",Property::Changeable::OnlyAtStart,44100);
    Pint->SetMinMax(8000,192000);
    Pint->Set_help("Mixer sample rate, setting any device's rate higher than this will probably lower their sound quality.");

    Pint = secprop->Add_int("blocksize",Property::Changeable::OnlyAtStart,1024);
    Pint->Set_values(blocksizes);
    Pint->Set_help("Mixer block size, larger blocks might help sound stuttering but sound will also be more lagged.");

    Pint = secprop->Add_int("prebuffer",Property::Changeable::OnlyAtStart,25);
    Pint->SetMinMax(0,100);
    Pint->Set_help("How many milliseconds of data to keep on top of the blocksize.");

    secprop=control->AddSection_prop("midi",&Null_Init,true);//done

    Pstring = secprop->Add_string("mpu401",Property::Changeable::WhenIdle,"intelligent");
    Pstring->Set_values(mputypes);
    Pstring->Set_help("Type of MPU-401 to emulate.");

    Phex = secprop->Add_hex("mpubase",Property::Changeable::WhenIdle,0/*default*/);
    Phex->Set_values(mpubases);
    Phex->Set_help("The IO address of the MPU-401.\n"
                   "Set to 0 to use a default I/O address.\n"
                   "300h to 330h are for use with IBM PC mode.\n"
                   "C0D0h to F8D0h (in steps of 800h) are for use with NEC PC-98 mode (MPU98).\n"
                   "80D2h through 80DEh are for use with NEC PC-98 Sound Blaster 16 MPU-401 emulation.\n"
                   "If not assigned (0), 330h is the default for IBM PC and E0D0h is the default for PC-98.");

    Pstring = secprop->Add_string("mididevice",Property::Changeable::WhenIdle,"default");
    Pstring->Set_values(devices);
    Pstring->Set_help("Device that will receive the MIDI data from MPU-401.");

    Pstring = secprop->Add_string("midiconfig",Property::Changeable::WhenIdle,"");
    Pstring->Set_help("Special configuration options for the device driver. This is usually the id or part of the name of the device you want to use (find the id/name with mixer/listmidi).\n"
                      "Or in the case of coreaudio or synth, you can specify a soundfont here.\n"
                      "When using a Roland MT-32 rev. 0 as midi output device, some games may require a delay in order to prevent 'buffer overflow' issues.\n"
                      "In that case, add 'delaysysex', for example: midiconfig=2 delaysysex\n"
                      "See the README/Manual for more details.");

    Pint = secprop->Add_int("samplerate",Property::Changeable::WhenIdle,44100);
    Pint->Set_values(rates);
    Pint->Set_help("Sample rate for MIDI synthesizer, if applicable.");
    
    Pint = secprop->Add_int("mpuirq",Property::Changeable::WhenIdle,-1);
    Pint->SetMinMax(-1,15);
    Pint->Set_help("MPU-401 IRQ. -1 to automatically choose.");

    Pstring = secprop->Add_string("mt32.reverse.stereo",Property::Changeable::WhenIdle,"off");
    Pstring->Set_values(mt32ReverseStereo);
    Pstring->Set_help("Reverse stereo channels for MT-32 output");

    Pstring = secprop->Add_string("mt32.verbose",Property::Changeable::WhenIdle,"off");
    Pstring->Set_values(mt32log);
    Pstring->Set_help("MT-32 debug logging");

    Pstring = secprop->Add_string("mt32.thread",Property::Changeable::WhenIdle,"off");
    Pstring->Set_values(mt32thread);
    Pstring->Set_help("MT-32 rendering in separate thread");

    Pstring = secprop->Add_string("mt32.dac",Property::Changeable::WhenIdle,"auto");
    Pstring->Set_values(mt32DACModes);
    Pstring->Set_help("MT-32 DAC input emulation mode\n"
        "Nice = 0 - default\n"
        "Produces samples at double the volume, without tricks.\n"
        "Higher quality than the real devices\n\n"

        "Pure = 1\n"
        "Produces samples that exactly match the bits output from the emulated LA32.\n"
        "Nicer overdrive characteristics than the DAC hacks (it simply clips samples within range)\n"
        "Much less likely to overdrive than any other mode.\n"
        "Half the volume of any of the other modes, meaning its volume relative to the reverb\n"
        "output when mixed together directly will sound wrong. So, reverb level must be lowered.\n"
        "Perfect for developers while debugging :)\n\n"

        "GENERATION1 = 2\n"
        "Re-orders the LA32 output bits as in early generation MT-32s (according to Wikipedia).\n"
        "Bit order at DAC (where each number represents the original LA32 output bit number, and XX means the bit is always low):\n"
        "15 13 12 11 10 09 08 07 06 05 04 03 02 01 00 XX\n\n"

        "GENERATION2 = 3\n"
        "Re-orders the LA32 output bits as in later geneerations (personally confirmed on my CM-32L - KG).\n"
        "Bit order at DAC (where each number represents the original LA32 output bit number):\n"
        "15 13 12 11 10 09 08 07 06 05 04 03 02 01 00 14\n");

    Pstring = secprop->Add_string("mt32.reverb.mode",Property::Changeable::WhenIdle,"auto");
    Pstring->Set_values(mt32reverbModes);
    Pstring->Set_help("MT-32 reverb mode");

    Pint = secprop->Add_int("mt32.reverb.time",Property::Changeable::WhenIdle,5);
    Pint->Set_values(mt32reverbTimes);
    Pint->Set_help("MT-32 reverb decaying time"); 

    Pint = secprop->Add_int("mt32.reverb.level",Property::Changeable::WhenIdle,3);
    Pint->Set_values(mt32reverbLevels);
    Pint->Set_help("MT-32 reverb level");

    Pint = secprop->Add_int("mt32.partials",Property::Changeable::WhenIdle,32);
    Pint->SetMinMax(0,256);
    Pint->Set_help("MT-32 max partials allowed (0-256)");

    secprop=control->AddSection_prop("sblaster",&Null_Init,true);//done
    
    Pstring = secprop->Add_string("sbtype",Property::Changeable::WhenIdle,"sb16");
    Pstring->Set_values(sbtypes);
    Pstring->Set_help("Type of Soundblaster to emulate. gb is Gameblaster.");

    Phex = secprop->Add_hex("sbbase",Property::Changeable::WhenIdle,0x220);
    Phex->Set_values(ios);
    Phex->Set_help("The IO address of the soundblaster.\n"
                   "220h to 2E0h are for use with IBM PC Sound Blaster emulation.\n"
                   "D2h to DEh are for use with NEC PC-98 Sound Blaster 16 emulation.");

    Pint = secprop->Add_int("irq",Property::Changeable::WhenIdle,7);
    Pint->Set_values(irqssb);
    Pint->Set_help("The IRQ number of the soundblaster. Set to -1 to start DOSBox with the IRQ unassigned");

    Pint = secprop->Add_int("mindma",Property::Changeable::OnlyAtStart,-1);
    Pint->Set_help( "Minimum DMA transfer left to increase attention across DSP blocks, in milliseconds. Set to -1 for default.\n"
            "There are some DOS games/demos that use single-cycle DSP playback in their music tracker and they micromanage\n"
            "the DMA transfer per block poorly in a way that causes popping and artifacts. Setting this option to 0 for\n"
            "such DOS applications may reduce audible popping and artifacts.");

    /* Sound Blaster IRQ hacks.
     *
     * These hacks reduce emulation accuracy but can be set to work around bugs or mistakes in some old
     * games and demos related to handling the Sound Blaster IRQ.
     *
     * - Saga by Dust (1993):
     *     Sound Blaster support has a fatal flaw in that the Sound Blaster interrupt handler it installs assumes
     *     DS == CS. It uses the DS register to read local variables needed to manage the Sound Blaster card but
     *     it makes no attempt to push DS and then load the DS segment value it needs. While the demo may seem to
     *     run normally at first, eventually the interrupt is fired at just the right time to catch the demo in
     *     the middle of it's graphics routines (DS=A000). Since the ISR uses DS to load the Sound Blaster DSP
     *     I/O port, it reads some random value from *video RAM* and then hangs in a loop waiting for that I/O
     *     port to clear bit 7! Setting 'cs_equ_ds' works around that bug by instructing PIC emulation not to
     *     fire the interrupt unless segment registers CS and DS match. */
    Pstring = secprop->Add_string("irq hack",Property::Changeable::WhenIdle,"none");
    Pstring->Set_help("Specify a hack related to the Sound Blaster IRQ to avoid crashes in a handful of games and demos.\n"
            "    none                   Emulate IRQs normally\n"
            "    cs_equ_ds              Do not fire IRQ unless two CPU segment registers match: CS == DS. Read Dosbox-X Wiki or source code for details.");

    Pint = secprop->Add_int("dma",Property::Changeable::WhenIdle,1);
    Pint->Set_values(dmassb);
    Pint->Set_help("The DMA number of the soundblaster. Set to -1 to start DOSBox with the IRQ unassigned");

    Pint = secprop->Add_int("hdma",Property::Changeable::WhenIdle,5);
    Pint->Set_values(dmassb);
    Pint->Set_help("The High DMA number of the soundblaster. Set to -1 to start DOSBox with the IRQ unassigned");

    Pbool = secprop->Add_bool("pic unmask irq",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Start the DOS virtual machine with the sound blaster IRQ already unmasked at the PIC.\n"
            "Some early DOS games/demos that support Sound Blaster expect the IRQ to fire but make\n"
            "no attempt to unmask the IRQ. If audio cuts out no matter what IRQ you try, then try\n"
            "setting this option.\n"
            "Option is needed for:\n"
            "   Public NMI \"jump\" demo (1992)");

    Pbool = secprop->Add_bool("enable speaker",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Start the DOS virtual machine with the sound blaster speaker enabled.\n"
                    "Sound Blaster Pro and older cards have a speaker disable/enable command.\n"
                    "Normally the card boots up with the speaker disabled. If a DOS game or demo\n"
                    "attempts to play without enabling the speaker, set this option to true to\n"
                    "compensate. This setting has no meaning if emulating a Sound Blaster 16 card.");

    Pbool = secprop->Add_bool("enable asp",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If set, emulate the presence of the Sound Blaster 16 Advanced Sound Processor/Creative Sound Processor chip.\n"
            "NOTE: This only emulates it's presence and the basic DSP commands to communicate with it. Actual ASP/CSP functions are not yet implemented.");

    Pbool = secprop->Add_bool("disable filtering",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("By default DOSBox-X filters Sound Blaster output to emulate lowpass filters and analog output limitations.\n"
            "Set this option to true to disable filtering. Note that doing so disables emulation of the Sound Blaster Pro\n"
            "output filter and ESS AudioDrive lowpass filter.");

    Pbool = secprop->Add_bool("dsp write buffer status must return 0x7f or 0xff",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If set, force port 22Ch (DSP write buffer status) to return 0x7F or 0xFF. If not set, the port\n"
            "may return 0x7F or 0xFF depending on what type of Sound Blaster is being emulated.\n"
            "Set this option for some early DOS demos that make that assumption about port 22Ch.\n"
            "Option is needed for:\n"
            "   Overload by Hysteria (1992) - Audio will crackle/saturate (8-bit overflow) except when sbtype=sb16");

    Pbool = secprop->Add_bool("pre-set sbpro stereo",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Start the DOS virtual machine with the Sound Blaster Pro stereo bit set (in the mixer).\n"
            "A few demos support Sound Blaster Pro but forget to set this bit.\n"
            "Option is needed for:\n"
            "   Inconexia by Iguana (1993)");

    Pbool = secprop->Add_bool("sbmixer",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Allow the soundblaster mixer to modify the DOSBox mixer.");

    Pstring = secprop->Add_string("oplmode",Property::Changeable::WhenIdle,"auto");
    Pstring->Set_values(oplmodes);
    Pstring->Set_help("Type of OPL emulation. On 'auto' the mode is determined by sblaster type.\n"
        "To emulate Adlib, set sbtype=none and oplmode=opl2. To emulate a Game Blaster, set\n"
        "sbtype=none and oplmode=cms");

    Pbool = secprop->Add_bool("adlib force timer overflow on detect",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If set, Adlib/OPL emulation will signal 'overflow' on timers after 50 I/O reads.\n"
            "This is a temporary hack to work around timing bugs noted in DOSBox-X. Certain\n"
            "games (Wolfenstein 3D) poll the Adlib status port a fixed number of times assuming\n"
            "that the poll loop takes long enough for the Adlib timer to run out. If the game\n"
            "cannot reliably detect Adlib at higher cycles counts, but can reliably detect at\n"
            "lower cycles counts, set this option.\n"
            "NOTE: Technically this decreases emulation accuracy, however it also reflects the\n"
            "      fact that DOSBox-X's I/O timing code needs some work to better match the\n"
            "      slowness of the ISA bus per I/O read in consideration of DOS games. So this\n"
            "      option is ON by default.");
    /* NTS: The reason I mention Wolfenstein 3D is that it seems coded not to probe for Sound Blaster unless it
     *      first detects the Adlib at port 0x388. No Adlib, no Sound Blaster. */
    /* ^ NTS: To see what I mean, download Wolf3d source code, look at ID_SD.C line 1585 (Adlib detection routine).
     *        Note it sets Timer 1, then reads port 388h 100 times before reading status to detect whether the
     *        timer "overflowed" (fairly typical Adlib detection code).
     *        Some quick math: 8333333Hz ISA BCLK / 6 cycles per read (3 wait states) = 1388888 reads/second possible
     *                         100 I/O reads * (1 / 1388888) = 72us */ 

    Pstring = secprop->Add_string("oplemu",Property::Changeable::WhenIdle,"default");
    Pstring->Set_values(oplemus);
    Pstring->Set_help("Provider for the OPL emulation. compat might provide better quality (see oplrate as well).");

    Pint = secprop->Add_int("oplrate",Property::Changeable::WhenIdle,44100);
    Pint->Set_values(oplrates);
    Pint->Set_help("Sample rate of OPL music emulation. Use 49716 for highest quality (set the mixer rate accordingly).");

    Phex = secprop->Add_hex("hardwarebase",Property::Changeable::WhenIdle,0x220);
    Phex->Set_help("base address of the real hardware soundblaster:\n"\
        "210,220,230,240,250,260,280");

    Pbool = secprop->Add_bool("force dsp auto-init",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Treat all single-cycle DSP commands as auto-init to keep playback going.\n"
            "This option is a workaround for DOS games or demos that use single-cycle DSP playback commands and\n"
            "have problems with missing the Sound Blaster IRQ under load. Do not enable unless you need this workaround.\n"
            "Needed for:\n"
            "  - Extreme \"lunatic\" demo (1993)");

    Pbool = secprop->Add_bool("force goldplay",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Always render Sound Blaster output sample-at-a-time. Testing option. You probably don't want to enable this.");

    Pbool = secprop->Add_bool("goldplay",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Enable goldplay emulation.");

    Pbool = secprop->Add_bool("goldplay stereo",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Enable workaround for goldplay stereo playback. Many DOS demos using this technique\n"
            "don't seem to know they need to double the frequency when programming the DSP time constant for Pro stereo output.\n"
            "If stereo playback seems to have artifacts consider enabling this option. For accurate emulation of Sound Blaster\n"
            "hardware, disable this option.");

    Pstring = secprop->Add_string("dsp require interrupt acknowledge",Property::Changeable::WhenIdle,"auto");
    Pstring->Set_help("If set, the DSP will halt DMA playback until IRQ acknowledgement occurs even in auto-init mode (SB16 behavior).\n"
            "If clear, IRQ acknowledgement will have no effect on auto-init playback (SB Pro and earlier & clone behavior)\n"
            "If set to 'auto' then behavior is determined by sbtype= setting.\n"
            "This is a setting for hardware accuracy in emulation. If audio briefly plays then stops then your DOS game\n"
            "and it's not using IRQ (but using DMA), try setting this option to 'false'");

    Pint = secprop->Add_int("dsp write busy delay",Property::Changeable::WhenIdle,-1);
    Pint->Set_help("Amount of time in nanoseconds the DSP chip signals 'busy' after writing to the DSP (port 2xCh). Set to -1 to use card-specific defaults.\n"
            "WARNING: Setting the value too high (above 20000ns) may have detrimental effects to DOS games that use IRQ 0 and DSP command 0x10 to play audio.\n"
            "         Setting the value way too high (above 1000000ns) can cause significant lag in DOS games.");

    Pbool = secprop->Add_bool("blaster environment variable",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Whether or not to set the BLASTER environment variable automatically at startup");

    Pbool = secprop->Add_bool("sample rate limits",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("If set (default), limit DSP sample rate to what real hardware is limited to");

    /* recommended for:
     *   1992 demo "overload" (if set, Sound Blaster support can run at 24KHz without causing demo to hang in the IRQ 0 timer)
     *   1993 demo "xmas 93" (superiority complex) because the demo's Sound Blaster mode writes at the timer interrupt rate without polling the DSP to check busy state */
    Pbool = secprop->Add_bool("instant direct dac",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If set, direct DAC output commands are instantaneous. This option is intended as a quick fix for\n"
            "games or demos that play direct DAC music/sound from the IRQ 0 timer who a) write the DSP command\n"
            "and data without polling the DSP to ensure it's ready or b) can get locked into the IRQ 0 handler\n"
            "waiting for DSP status when instructed to play at or beyond the DSP's maximum direct DAC sample rate.\n"
            "This fix allows broken Sound Blaster code to work and should not be enabled unless necessary.");

    /* accuracy emulation: SB16 does not honor SBPro stereo bit in the mixer */
    Pbool = secprop->Add_bool("stereo control with sbpro only",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Default on. If set, Sound Blaster Pro stereo is not available when emulating sb16 or sb16vibra.\n"
            "If clear, sb16 emulation will honor the sbpro stereo bit. Note that Creative SB16 cards do not\n"
            "honor the stereo bit, and this option allows DOSBox emulate that fact. Accuracy setting.");

    /* NTS: It turns out (SB16 at least) the DSP will periodically set bit 7 (busy) by itself at some
     *      clock rate even if it's idle. Casual testing on an old Pentium system with a ViBRA shows
     *      it's possible to see both 0x7F and 0xFF come back if you repeatedly type "i 22c" in DOS
     *      DEBUG.EXE.  FIXME: At what clock rate and duty cycle does this happen? */
    Pint = secprop->Add_int("dsp busy cycle rate",Property::Changeable::WhenIdle,-1/*default*/);
    Pint->Set_help("Sound Blaster 16 DSP chips appear to go busy periodically at some high clock rate\n"
            "whether the DSP is actually doing anything for the system or not. This is an accuracy\n"
            "option for Sound Blaster emulation. If this option is nonzero, it will be interpreted\n"
            "as the busy cycle rate in Hz. If zero, busy cycle will not be emulated. If -1, sound\n"
            "blaster emulation will automatically choose a setting based on the sbtype= setting");

    Pint = secprop->Add_int("dsp busy cycle always",Property::Changeable::WhenIdle,-1/*default*/);
    Pint->Set_help("If set, the DSP busy cycle always happens. If clear, DSP busy cycle only happens when\n"
            "audio playback is running. Default setting is to pick according to the sound card.");

    Pint = secprop->Add_int("dsp busy cycle duty",Property::Changeable::WhenIdle,-1/*default*/);
    Pint->Set_help("If emulating SB16 busy cycle, this value (0 to 100) controls the duty cycle of the busy cycle.\n"
            "If this option is set to -1, Sound Blaster emulation will choose a value automatically according\n"
            "to sbtype=. If 0, busy cycle emulation is disabled.");

    /* NTS: Confirmed: My Sound Blaster 2.0 (at least) mirrors the DSP on port 22Ch and 22Dh. This option
     *      will only take effect with sbtype sb1 and sb2, so make it enabled by default. Accuracy setting. */
    Pbool = secprop->Add_bool("io port aliasing",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("If set, Sound Blaster ports alias by not decoding the LSB of the I/O port.\n"
            "This option only applies when sbtype is set to sb1 or sb2 (not SBPro or SB16).\n"
            "This is a hack for the Electromotive Force 'Internal Damage' demo which apparently\n"
            "relies on this behavior for Sound Blaster output and should be enabled for accuracy in emulation.");

    secprop=control->AddSection_prop("gus",&Null_Init,true); //done
    Pbool = secprop->Add_bool("gus",Property::Changeable::WhenIdle,false);  
    Pbool->Set_help("Enable the Gravis Ultrasound emulation.");

    Pbool = secprop->Add_bool("autoamp",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If set, GF1 output will reduce in volume automatically if the sum of all channels exceeds full volume.\n"
                    "If not set, then loud music will clip to full volume just as it would on real hardware.\n"
                    "Enable this option for loud music if you want a more pleasing rendition without saturation and distortion.");

    Pbool = secprop->Add_bool("unmask dma",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Start the DOS virtual machine with the DMA channel already unmasked at the controller.\n"
            "Use this for DOS applications that expect to operate the GUS but forget to unmask the DMA channel.");

    Pbool = secprop->Add_bool("ignore channel count while active",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Ignore writes to the active channel count register when the DAC is enabled (bit 1 of GUS reset)\n"
                    "This is a HACK for demoscene prod 'Ice Fever' without which the music sounds wrong.\n"
                    "According to current testing real hardware does not behave this way.");

    Pbool = secprop->Add_bool("pic unmask irq",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Start the DOS virtual machine with the GUS IRQ already unmasked at the PIC.");

    Pbool = secprop->Add_bool("startup initialized",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If set, start the GF1 in a fully initialized state (as if ULTRINIT had been run).\n"
                    "If clear, leave the card in an uninitialized state (as if cold boot).\n"
                    "Some DOS games or demoscene productions will hang or fail to use the Ultrasound hardware\n"
                    "because they assume the card is initialized and their hardware detect does not fully initialize the card.");

    Pbool = secprop->Add_bool("dma enable on dma control polling",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If set, automatically enable GUS DMA transfer bit in specific cases when the DMA control register is being polled.\n"
                    "THIS IS A HACK. Some games and demoscene productions need this hack to avoid hanging while uploading sample data\n"
                    "to the Gravis Ultrasound due to bugs in their implementation.");

    Pbool = secprop->Add_bool("clear dma tc irq if excess polling",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If the DOS application is seen polling the IRQ status register rapidly, automatically clear the DMA TC IRQ status.\n"
            "This is a hack that should only be used with DOS applications that need it to avoid bugs in their GUS support code.\n"
            "Needed for:\n"
            "  Warcraft II by Blizzard ............. if using GUS for music and sound, set this option to prevent the game from\n"
            "                                        hanging when you click on the buttons in the main menu.");

    /* some DOS demos, especially where the programmers wrote their own tracker, forget to set "master IRQ enable" on the GUS,
     * and then wonder why music isn't playing. prior to some GUS bugfixes they happend to work anyway because DOSBox also
     * ignored master IRQ enable. you can restore that buggy behavior here.
     *
     * DOS games & demos that need this:
     *   - "Juice" by Psychic Link (writes 0x300 to GUS reset which only enables DAC and takes card out of reset, does not enable IRQ) */
    Pbool = secprop->Add_bool("force master irq enable",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Set this option if a DOS game or demo initializes the GUS but is unable to play any music.\n"
            "Usually the cause is buggy GUS support that resets the GUS but fails to set the Master IRQ enable bit.");

    Pstring = secprop->Add_string("gus panning table",Property::Changeable::WhenIdle,"default");
    Pstring->Set_values(guspantables);
    Pstring->Set_help("Controls which table or equation is used for the Gravis Ultrasound panning emulation.\n"
            "accurate emulation attempts to better reflect how the actual hardware handles panning,\n"
            "while the old emulation uses a simpler idealistic mapping.");

    Pint = secprop->Add_int("gusrate",Property::Changeable::WhenIdle,44100);
    Pint->Set_values(rates);
    Pint->Set_help("Sample rate of Ultrasound emulation.");

    Pbool = secprop->Add_bool("gus fixed render rate",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If set, Gravis Ultrasound audio output is rendered at a fixed sample rate specified by 'gusrate'. This can provide better quality than real hardware,\n"
            "if desired. Else, Gravis Ultrasound emulation will change the sample rate of it's output according to the number of active channels, just like real hardware.\n"
            "Note: DOSBox-X defaults to 'false', while mainline DOSBox SVN is currently hardcoded to render as if this setting is 'true'.");

    Pint = secprop->Add_int("gusmemsize",Property::Changeable::WhenIdle,-1);
    Pint->SetMinMax(-1,1024);
    Pint->Set_help("Amount of RAM on the Gravis Ultrasound in KB. Set to -1 for default.");

    Pdouble = secprop->Add_double("gus master volume",Property::Changeable::WhenIdle,0);
    Pdouble->SetMinMax(-120.0,6.0);
    Pdouble->Set_help("Master Gravis Ultrasound GF1 volume, in decibels. Reducing the master volume can help with games or demoscene productions where the music is too loud and clipping");

    Phex = secprop->Add_hex("gusbase",Property::Changeable::WhenIdle,0x240);
    Phex->Set_values(iosgus);
    Phex->Set_help("The IO base address of the Gravis Ultrasound.");

    Pint = secprop->Add_int("gusirq",Property::Changeable::WhenIdle,5);
    Pint->Set_values(irqsgus);
    Pint->Set_help("The IRQ number of the Gravis Ultrasound.");

    Pint = secprop->Add_int("gusdma",Property::Changeable::WhenIdle,3);
    Pint->Set_values(dmasgus);
    Pint->Set_help("The DMA channel of the Gravis Ultrasound.");
 
    Pstring = secprop->Add_string("irq hack",Property::Changeable::WhenIdle,"none");
    Pstring->Set_help("Specify a hack related to the Gravis Ultrasound IRQ to avoid crashes in a handful of games and demos.\n"
            "    none                   Emulate IRQs normally\n"
            "    cs_equ_ds              Do not fire IRQ unless two CPU segment registers match: CS == DS. Read Dosbox-X Wiki or source code for details.");

    Pstring = secprop->Add_string("gustype",Property::Changeable::WhenIdle,"classic");
    Pstring->Set_values(gustypes);
    Pstring->Set_help(  "Type of Gravis Ultrasound to emulate.\n"
                "classic             Original Gravis Ultrasound chipset\n"
                "classic37           Original Gravis Ultrasound with ICS Mixer (rev 3.7)\n"
                "max                 Gravis Ultrasound MAX emulation (with CS4231 codec)\n"
                "interwave           Gravis Ultrasound Plug & Play (interwave)");

    Pstring = secprop->Add_string("ultradir",Property::Changeable::WhenIdle,"C:\\ULTRASND");
    Pstring->Set_help(
        "Path to Ultrasound directory. In this directory\n"
        "there should be a MIDI directory that contains\n"
        "the patch files for GUS playback. Patch sets used\n"
        "with Timidity should work fine.");

    secprop = control->AddSection_prop("innova",&Null_Init,true);//done
    Pbool = secprop->Add_bool("innova",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Enable the Innovation SSI-2001 emulation.");
    Pint = secprop->Add_int("samplerate",Property::Changeable::WhenIdle,22050);
    Pint->Set_values(rates);
    Pint->Set_help("Sample rate of Innovation SSI-2001 emulation");
    Phex = secprop->Add_hex("sidbase",Property::Changeable::WhenIdle,0x280);
    Phex->Set_values(sidbaseno);
    Phex->Set_help("SID base port (typically 280h).");
    Pint = secprop->Add_int("quality",Property::Changeable::WhenIdle,0);
    Pint->Set_values(qualityno);
    Pint->Set_help("Set SID emulation quality level (0 to 3).");

    secprop = control->AddSection_prop("speaker",&Null_Init,true);//done
    Pbool = secprop->Add_bool("pcspeaker",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Enable PC-Speaker emulation.");

    /* added for "baoxiao-sanguozhi" which for some reason uses both port 61h bit 4 (DRAM refresh) and PIT timer 2 (PC speaker)
     * for game timing IN ADDITION TO the BIOS timer counter in the BIOS data area. Game does not set bit 0 itself, so if the
     * bit wasn't set, the game will hang when asking for a password. Setting this option to "true" tells the BIOS to start the
     * system with that bit set so games like that can run. [https://github.com/joncampbell123/dosbox-x/issues/1274].
     *
     * Note that setting clock gate enable will not make audible sound through the PC speaker unless bit 1 (output gate enable)
     * is also set. Setting bits [1:0] = to 01 is a way to cycle PIT timer 2 without making audible noise. */
    Pbool = secprop->Add_bool("pcspeaker clock gate enable at startup",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Start system with the clock gate (bit 0 of port 61h) on. Needed for some games that use the PC speaker for timing on IBM compatible systems.\n"
                    "This option has no effect in PC-98 mode.");

    Pint = secprop->Add_int("initial frequency",Property::Changeable::WhenIdle,-1);
    Pint->Set_help("PC speaker PIT timer is programmed to this frequency on startup. If the DOS game\n"
            "or demo causes a long audible beep at startup (leaving the gate open) try setting\n"
            "this option to 0 to silence the PC speaker until reprogrammed by the demo.\n"
            "Set to 0 for some early Abaddon demos including \"Torso\" and \"Cycling\".");

    Pint = secprop->Add_int("pcrate",Property::Changeable::WhenIdle,44100);
    Pint->Set_values(rates);
    Pint->Set_help("Sample rate of the PC-Speaker sound generation.");

    Pstring = secprop->Add_string("tandy",Property::Changeable::WhenIdle,"auto");
    Pstring->Set_values(tandys);
    Pstring->Set_help("Enable Tandy Sound System emulation. For 'auto', emulation is present only if machine is set to 'tandy'.");

    Pint = secprop->Add_int("tandyrate",Property::Changeable::WhenIdle,44100);
    Pint->Set_values(rates);
    Pint->Set_help("Sample rate of the Tandy 3-Voice generation.");

    Pbool = secprop->Add_bool("disney",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Enable Disney Sound Source emulation. (Covox Voice Master and Speech Thing compatible).");
    Pstring = secprop->Add_string("ps1audio",Property::Changeable::WhenIdle,"off");
    Pstring->Set_values(ps1opt);
    Pstring->Set_help("Enable PS1 audio emulation.");
    Pint = secprop->Add_int("ps1audiorate",Property::Changeable::OnlyAtStart,22050);
    Pint->Set_values(rates);
    Pint->Set_help("Sample rate of the PS1 audio emulation.");

    secprop=control->AddSection_prop("joystick",&Null_Init,false);//done
    Pstring = secprop->Add_string("joysticktype",Property::Changeable::WhenIdle,"auto");
    Pstring->Set_values(joytypes);
    Pstring->Set_help(
        "Type of joystick to emulate: auto (default), none,\n"
        "2axis (supports two joysticks),\n"
        "4axis (supports one joystick, first joystick used),\n"
        "4axis_2 (supports one joystick, second joystick used),\n"
        "fcs (Thrustmaster), ch (CH Flightstick).\n"
        "none disables joystick emulation.\n"
        "auto chooses emulation depending on real joystick(s).\n"
        "(Remember to reset dosbox's mapperfile if you saved it earlier)");

    Pbool = secprop->Add_bool("timed",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("enable timed intervals for axis. Experiment with this option, if your joystick drifts (away).");

    Pbool = secprop->Add_bool("autofire",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("continuously fires as long as you keep the button pressed.");

    Pbool = secprop->Add_bool("swap34",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("swap the 3rd and the 4th axis. can be useful for certain joysticks.");

    Pbool = secprop->Add_bool("buttonwrap",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("enable button wrapping at the number of emulated buttons.");

	/*improved joystick
	 * each axis has its own deadzone and response
	 * each axis index can be remapped, e.g. fix poor driver mappings
	 */

	/* logical axes settings*/
	std::vector<int> sticks = { 2, 1 };
	for (auto i = 0u; i < sticks.size(); i++)
	{
		const auto count = sticks[i];
		for (auto j = 0; j < count; j++)
		{
			const auto joy = std::to_string(i + 1);
			const auto stick = std::to_string(j + 1);
			const auto name = "joy" + joy + "deadzone" + stick;
			const auto help = "deadzone for joystick " + joy + " thumbstick " + stick + ".";
			Pdouble = secprop->Add_double(name, Property::Changeable::WhenIdle, 0.25);
			Pdouble->SetMinMax(0.0, 1.0);
			Pdouble->Set_help(help);
		}
	}
	for (auto i = 0u; i < sticks.size(); i++)
	{
		const auto count = sticks[i];
		for (auto j = 0; j < count; j++)
		{
			const auto joy = std::to_string(i + 1);
			const auto stick = std::to_string(j + 1);
			const auto name = "joy" + joy + "response" + stick;
			const auto help = "response for joystick " + joy + " thumbstick " + stick + ".";
			Pdouble = secprop->Add_double(name, Property::Changeable::WhenIdle, 1.0);
			Pdouble->SetMinMax(-5.0, 5.0);
			Pdouble->Set_help(help);
		}
	}

	const auto joysticks = 2;
	const auto axes = 8;
	for (auto i = 0; i < joysticks; i++)
	{
		for (auto j = 0; j < axes; j++)
		{
			const auto joy = std::to_string(i + 1);
			const auto axis = std::to_string(j);
			const auto propname = "joy" + joy + "axis" + axis;
			Pint = secprop->Add_int(propname, Property::Changeable::WhenIdle, j);
			Pint->SetMinMax(0, axes - 1);
			const auto help = "axis for joystick " + joy + " axis " + axis + ".";
			Pint->Set_help(help);
		}
	}
	/*physical axes settings*/
	secprop = control->AddSection_prop("mapper", &Null_Init, true);

	const auto directions = 2;
	for (auto i = 0; i < joysticks; i++)
	{
		for (auto j = 0; j < axes; j++)
		{
			for (auto k = 0; k < directions; k++)
			{
				const auto joy = std::to_string(i + 1);
				const auto axis = std::to_string(j);
				const auto dir = k == 0 ? "-" : "+";
				const auto name = "joy" + joy + "deadzone" + axis + dir;
				Pdouble = secprop->Add_double(name, Property::Changeable::WhenIdle, 0.6);
				Pdouble->SetMinMax(0.0, 1.0);
				const auto help = "deadzone for joystick " + joy + " axis " + axis + dir;
				Pdouble->Set_help(help);
			}
		}
	}


    secprop=control->AddSection_prop("serial",&Null_Init,true);

    Pmulti_remain = secprop->Add_multiremain("serial1",Property::Changeable::WhenIdle," ");
    Pstring = Pmulti_remain->GetSection()->Add_string("type",Property::Changeable::WhenIdle,"dummy");
    Pmulti_remain->SetValue("dummy",/*init*/true);
    Pstring->Set_values(serials);
    Pmulti_remain->GetSection()->Add_string("parameters",Property::Changeable::WhenIdle,"");
    Pmulti_remain->Set_help(
        "set type of device connected to com port.\n"
        "Can be disabled, dummy, modem, nullmodem, directserial.\n"
        "Additional parameters must be in the same line in the form of\n"
        "parameter:value. Parameter for all types is irq (optional).\n"
        "for directserial: realport (required), rxdelay (optional).\n"
        "                 (realport:COM1 realport:ttyS0).\n"
        "for modem: listenport (optional).\n"
        "for nullmodem: server, rxdelay, txdelay, telnet, usedtr,\n"
        "               transparent, port, inhsocket, nonlocal (all optional).\n"
        "               connections are limited to localhost unless you specify nonlocal:1\n"
        "Example: serial1=modem listenport:5000");

    Pmulti_remain = secprop->Add_multiremain("serial2",Property::Changeable::WhenIdle," ");
    Pstring = Pmulti_remain->GetSection()->Add_string("type",Property::Changeable::WhenIdle,"dummy");
    Pmulti_remain->SetValue("dummy",/*init*/true);
    Pstring->Set_values(serials);
    Pmulti_remain->GetSection()->Add_string("parameters",Property::Changeable::WhenIdle,"");
    Pmulti_remain->Set_help("see serial1");

    Pmulti_remain = secprop->Add_multiremain("serial3",Property::Changeable::WhenIdle," ");
    Pstring = Pmulti_remain->GetSection()->Add_string("type",Property::Changeable::WhenIdle,"disabled");
    Pmulti_remain->SetValue("disabled",/*init*/true);
    Pstring->Set_values(serials);
    Pmulti_remain->GetSection()->Add_string("parameters",Property::Changeable::WhenIdle,"");
    Pmulti_remain->Set_help("see serial1");

    Pmulti_remain = secprop->Add_multiremain("serial4",Property::Changeable::WhenIdle," ");
    Pstring = Pmulti_remain->GetSection()->Add_string("type",Property::Changeable::WhenIdle,"disabled");
    Pmulti_remain->SetValue("disabled",/*init*/true);
    Pstring->Set_values(serials);
    Pmulti_remain->GetSection()->Add_string("parameters",Property::Changeable::WhenIdle,"");
    Pmulti_remain->Set_help("see serial1");

#if C_PRINTER
    // printer redirection parameters
    secprop = control->AddSection_prop("printer", &Null_Init);
    Pbool = secprop->Add_bool("printer", Property::Changeable::WhenIdle, true);
    Pbool->Set_help("Enable printer emulation.");
    //secprop->Add_string("fontpath","%%windir%%\\fonts");
    Pint = secprop->Add_int("dpi", Property::Changeable::WhenIdle, 360);
    Pint->Set_help("Resolution of printer (default 360).");
    Pint = secprop->Add_int("width", Property::Changeable::WhenIdle, 85);
    Pint->Set_help("Width of paper in 1/10 inch (default 85 = 8.5'').");
    Pint = secprop->Add_int("height", Property::Changeable::WhenIdle, 110);
    Pint->Set_help("Height of paper in 1/10 inch (default 110 = 11.0'').");
#ifdef C_LIBPNG
    Pstring = secprop->Add_string("printoutput", Property::Changeable::WhenIdle, "png");
#else
    Pstring = secprop->Add_string("printoutput", Property::Changeable::WhenIdle, "ps");
#endif
    Pstring->Set_help("Output method for finished pages: \n"
#ifdef C_LIBPNG
        "  png     : Creates PNG images (default)\n"
#endif
        "  ps      : Creates Postscript\n"
        "  bmp     : Creates BMP images (very huge files, not recommend)\n"
#if defined (WIN32)
        "  printer : Send to an actual printer (Print dialog will appear)"
#endif
    );

    Pbool = secprop->Add_bool("multipage", Property::Changeable::WhenIdle, false);
    Pbool->Set_help("Adds all pages to one Postscript file or printer job until CTRL-F2 is pressed.");

    Pstring = secprop->Add_string("docpath", Property::Changeable::WhenIdle, ".");
    Pstring->Set_help("The path where the output files are stored.");

    Pint = secprop->Add_int("timeout", Property::Changeable::WhenIdle, 0);
    Pint->Set_help("(in milliseconds) if nonzero: the time the page will\n"
        "be ejected automatically after when no more data\n"
        "arrives at the printer.");
#endif

    // parallel ports
    secprop=control->AddSection_prop("parallel",&Null_Init,true);
    Pstring = secprop->Add_string("parallel1",Property::Changeable::WhenIdle,"disabled");
    Pstring->Set_help(
            "parallel1-3 -- set type of device connected to lpt port.\n"
            "Can be:\n"
            "   reallpt (direct parallel port passthrough),\n"
            "   file (records data to a file or passes it to a device),\n"
            "   printer (virtual dot-matrix printer, see [printer] section)\n"
            "       disney (attach Disney Sound Source emulation to this port)\n"
            "Additional parameters must be in the same line in the form of\n"
            "parameter:value.\n"
            "  for reallpt:\n"
            "  Windows:\n"
            "    realbase (the base address of your real parallel port).\n"
            "      Default: 378\n"
            "    ecpbase (base address of the ECP registers, optional).\n"
            "  Linux: realport (the parallel port device i.e. /dev/parport0).\n"
            "  for file: \n"
            "    dev:<devname> (i.e. dev:lpt1) to forward data to a device,\n"
            "    or append:<file> appends data to the specified file.\n"
            "    Without the above parameters data is written to files in the capture dir.\n"
            "    Additional parameters: timeout:<milliseconds> = how long to wait before\n"
            "    closing the file on inactivity (default:500), addFF to add a formfeed when\n"
            "    closing, addLF to add a linefeed if the app doesn't, cp:<codepage number>\n"
            "    to perform codepage translation, i.e. cp:437\n"
            "  for printer:\n"
            "    printer still has it's own configuration section above."
    );
    Pstring = secprop->Add_string("parallel2",Property::Changeable::WhenIdle,"disabled");
    Pstring->Set_help("see parallel1");
    Pstring = secprop->Add_string("parallel3",Property::Changeable::WhenIdle,"disabled");
    Pstring->Set_help("see parallel1");

    Pbool = secprop->Add_bool("dongle",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Enable dongle");

    /* All the DOS Related stuff, which will eventually start up in the shell */
    secprop=control->AddSection_prop("dos",&Null_Init,false);//done
    Pbool = secprop->Add_bool("xms",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Enable XMS support.");

    Pint = secprop->Add_int("xms handles",Property::Changeable::WhenIdle,0);
    Pint->Set_help("Number of XMS handles available for the DOS environment, or 0 to use a reasonable default");

    Pbool = secprop->Add_bool("shell configuration as commands",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("Allow entering dosbox.conf configuration parameters as shell commands to get and set settings.\n"
                    "This is disabled by default to avoid conflicts between commands and executables.\n"
                    "It is recommended to get and set dosbox.conf settings using the CONFIG command instead.\n"
                    "Compatibility with DOSBox SVN can be improved by enabling this option.");

    Pbool = secprop->Add_bool("hma",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Report through XMS that HMA exists (not necessarily available)");

    Pbool = secprop->Add_bool("hma allow reservation",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Allow TSR and application (anything other than the DOS kernel) to request control of the HMA.\n"
            "They will not be able to request control however if the DOS kernel is configured to occupy the HMA (DOS=HIGH)");

    Pint = secprop->Add_int("hard drive data rate limit",Property::Changeable::WhenIdle,-1);
    Pint->Set_help("Slow down (limit) hard disk throughput. This setting controls the limit in bytes/second.\n"
                   "Set to 0 to disable the limit, or -1 to use a reasonable default.");

    Pint = secprop->Add_int("hma minimum allocation",Property::Changeable::WhenIdle,0);
    Pint->Set_help("Minimum allocation size for HMA in bytes (equivalent to /HMAMIN= parameter).");

    Pbool = secprop->Add_bool("log console",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If set, log DOS CON output to the log file.");

    Pbool = secprop->Add_bool("dos in hma",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Report that DOS occupies HMA (equiv. DOS=HIGH)");

    Pint = secprop->Add_int("dos sda size",Property::Changeable::WhenIdle,0);
    Pint->Set_help("SDA (swappable data area) size, in bytes. Set to 0 to use a reasonable default.");

    Pint = secprop->Add_int("hma free space",Property::Changeable::WhenIdle,34*1024); /* default 34KB (TODO: How much does MS-DOS 5.0 usually occupy?) */
    Pint->Set_help("Controls the amount of free space available in HMA. This setting is not meaningful unless the\n"
            "DOS kernel occupies HMA and the emulated DOS version is at least 5.0.");

    Pstring = secprop->Add_string("cpm compatibility mode",Property::Changeable::WhenIdle,"auto");
    Pstring->Set_values(cpm_compat_modes);
    Pstring->Set_help(
            "This controls how the DOS kernel sets up the CP/M compatibility code in the PSP segment.\n"
            "Several options are provided to emulate one of several undocumented behaviors related to the CP/M entry point.\n"
            "If set to auto, DOSBox-X will pick the best option to allow it to work properly.\n"
            "Unless set to 'off', this option will require the DOS kernel to occupy the first 256 bytes of the HMA memory area\n"
            "to prevent crashes when the A20 gate is switched on.\n"
            "   auto      Pick the best option\n"
            "   off       Turn off the CP/M entry point (program will abort if called)\n"
            "   msdos2    MS-DOS 2.x behavior, offset field also doubles as data segment size\n"
            "   msdos5    MS-DOS 5.x behavior, entry point becomes one of two fixed addresses\n"
            "   direct    Non-standard behavior, encode the CALL FAR directly to the entry point rather than indirectly");

    Pbool = secprop->Add_bool("share",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Report SHARE.EXE as resident. Does not actually emulate SHARE functions.");

    Phex = secprop->Add_hex("minimum dos initial private segment", Property::Changeable::WhenIdle,0);
    Phex->Set_help("In non-mainline mapping mode, where DOS structures are allocated from base memory, this sets the\n"
            "minimum segment value. Recommended value is 0x70. You may reduce the value down to 0x50 if freeing\n"
            "up more memory is important. Set to 0 for default.");

    Phex = secprop->Add_hex("minimum mcb segment", Property::Changeable::WhenIdle,0);
    Phex->Set_help("Minimum segment value to begin memory allocation from, in hexadecimal. Set to 0 for default.\n"
            "You can increase available DOS memory by reducing this value down to as low as 0x51, however\n"
            "setting it to low can cause some DOS programs to crash or run erratically, and some DOS games\n"
            "and demos to cause intermittent static noises when using Sound Blaster output. DOS programs\n"
            "compressed with Microsoft EXEPACK will not run if the minimum MCB segment is below 64KB.");

    Phex = secprop->Add_hex("minimum mcb free", Property::Changeable::WhenIdle,0);
    Phex->Set_help("Minimum free segment value to leave free. At startup, the DOS kernel will allocate memory\n"
                   "up to this point. This can be used to deal with EXEPACK issues or DOS programs that cannot\n"
                   "be loaded too low in memory. This differs from 'minimum mcb segment' in that this affects\n"
                   "the lowest free block instead of the starting point of the mcb chain.");

    // TODO: Enable by default WHEN the 'SD' signature becomes valid, and a valid device list within
    //       is emulated properly.
    Pbool = secprop->Add_bool("enable dummy device mcb",Property::Changeable::OnlyAtStart,false);
    Pbool->Set_help("If set (default), allocate a fake device MCB at the base of conventional memory.\n"
            "Clearing this option can reclaim a small amount of conventional memory at the expense of\n"
            "some minor DOS compatibility.");

    Pint = secprop->Add_int("maximum environment block size on exec", Property::Changeable::WhenIdle,-1);
    Pint->SetMinMax(-1,65535);
    Pint->Set_help("Maximum environment block size to copy for child processes. Set to -1 for default.");

    Pint = secprop->Add_int("additional environment block size on exec", Property::Changeable::WhenIdle,-1);
    Pint->SetMinMax(-1,65535);
    Pint->Set_help("When executing a program, compute the size of the parent block then add this amount to allow for a few additional variables.\n"
            "If the subprocesses will never add/modify the environment block, you can free up a few additional bytes by setting this to 0.\n"
            "Set to -1 for default setting.");

    // DEPRECATED, REMOVE
    Pbool = secprop->Add_bool("enable a20 on windows init",Property::Changeable::OnlyAtStart,false);
    Pbool->Set_help("If set, DOSBox will enable the A20 gate when Windows 3.1/9x broadcasts the INIT message\n"
            "at startup. Windows 3.1 appears to make assumptions at some key points on startup about\n"
            "A20 that don't quite hold up and cause Windows 3.1 to crash when you set A20 emulation\n"
            "to a20=mask as opposed to a20=fast. This option is enabled by default.");

    Pbool = secprop->Add_bool("zero memory on xms memory allocation",Property::Changeable::OnlyAtStart,false);
    Pbool->Set_help("If set, memory returned by XMS allocation call is zeroed first. This is NOT what\n"
            "DOS actually does, but if set, can help certain DOS games and demos cope with problems\n"
            "related to uninitialized variables in extended memory. When enabled this option may\n"
            "incur a slight to moderate performance penalty.");

    Pstring = secprop->Add_string("dosv",Property::Changeable::WhenIdle,"off");
    Pstring->Set_values(dosv_settings);
    Pstring->Set_help("Enable DOS/V emulation and specify which version to emulate. This option is intended for\n"
            "use with games or software originating from Asia that use the double byte character set\n"
            "encodings and the DOS/V extensions to display Japanese, Chinese, or Korean text.\n"
            "Note that enabling DOS/V replaces 80x25 text mode (INT 10h mode 3) with a EGA/VGA graphics\n"
            "mode that emulates text mode to display the characters and may be incompatible with non-Asian\n"
            "software that assumes direct access to the text mode via segment 0xB800.\n"
            "WARNING: This option is very experimental at this time.");

    Pstring = secprop->Add_string("ems",Property::Changeable::WhenIdle,"true");
    Pstring->Set_values(ems_settings);
    Pstring->Set_help("Enable EMS support. The default (=true) provides the best\n"
        "compatibility but certain applications may run better with\n"
        "other choices, or require EMS support to be disabled (=false)\n"
        "to work at all.");

    Pbool = secprop->Add_bool("vcpi",Property::Changeable::OnlyAtStart,true);
    Pbool->Set_help("If set and expanded memory is enabled, also emulate VCPI.");

    Pbool = secprop->Add_bool("unmask timer on disk io",Property::Changeable::OnlyAtStart,false);
    Pbool->Set_help("If set, INT 21h emulation will unmask IRQ 0 (timer interrupt) when the application opens/closes/reads/writes files.");

    Pbool = secprop->Add_bool("zero int 67h if no ems",Property::Changeable::OnlyAtStart,true);
    Pbool->Set_help("If ems=false, leave interrupt vector 67h zeroed out (default true).\n"
            "This is a workaround for games or demos that try to detect EMS by whether or not INT 67h is 0000:0000 rather than a proper test.\n"
            "This option also affects whether INT 67h is zeroed when booting a guest OS");

    Pbool = secprop->Add_bool("zero unused int 68h",Property::Changeable::OnlyAtStart,false);
    Pbool->Set_help("Leave INT 68h zero at startup.\n"
            "Set this to true for certain games that use INT 68h in unusual ways that require a zero value.\n"
            "Note that the vector is left at zero anyway when machine=cga.\n"
            "This is needed to properly run 1988 game 'PopCorn'.");

    /* FIXME: The vm86 monitor in src/ints/ems.cpp is not very stable! Option is default OFF until stabilized! */
    Pbool = secprop->Add_bool("emm386 startup active",Property::Changeable::OnlyAtStart,false);
    Pbool->Set_help("If set and expanded memory is set to emulate emm386, start the DOS machine with EMM386.EXE active\n"
            "(running the 16-bit DOS environment from within Virtual 8086 mode). If you will be running anything\n"
            "that involves a DOS extender you will also need to enable the VCPI interface as well.");

    Pbool = secprop->Add_bool("zero memory on ems memory allocation",Property::Changeable::OnlyAtStart,false);
    Pbool->Set_help("If set, memory returned by EMS allocation call is zeroed first. This is NOT what\n"
            "DOS actually does, but if set, can help certain DOS games and demos cope with problems\n"
            "related to uninitialized variables in expanded memory. When enabled this option may\n"
            "incur a slight to moderate performance penalty.");

    Pint = secprop->Add_int("ems system handle memory size",Property::Changeable::WhenIdle,384);
    Pint->Set_help("Amount of memory associated with system handle, in KB");

    Pbool = secprop->Add_bool("ems system handle on even megabyte",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If set, try to allocate the EMM system handle on an even megabyte.\n"
            "If the DOS game or demo fiddles with the A20 gate while using EMM386.EXE emulation in virtual 8086 mode, setting this option may help prevent crashes.\n"
            "However, forcing allocation on an even megabyte will also cause some extended memory fragmentation and reduce the\n"
            "overall amount of extended memory available to the DOS game depending on whether it expects large contiguous chunks\n"
            "of extended memory.");

    Pbool = secprop->Add_bool("umb",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Enable UMB support.");

    Phex = secprop->Add_hex("umb start",Property::Changeable::OnlyAtStart,0); /* <- (0=auto) 0xD000 is mainline DOSBox compatible behavior */
    Phex->Set_help("UMB region starting segment");

    Phex = secprop->Add_hex("umb end",Property::Changeable::OnlyAtStart,0); /* <- (0=auto) 0xEFFF is mainline DOSBox compatible (where base=0xD000 and size=0x2000) */
    Phex->Set_help("UMB region last segment");

    Pbool = secprop->Add_bool("kernel allocation in umb",Property::Changeable::OnlyAtStart,false);
    Pbool->Set_help("If set, dynamic kernel allocation=1, and private area in umb=1, all kernel structures will be allocated from the private area in UMB.\n"
            "If you intend to run Windows 3.1 in DOSBox, you must set this option to false else Windows 3.1 will not start.");

    Pbool = secprop->Add_bool("keep umb on boot",Property::Changeable::OnlyAtStart,false);
    Pbool->Set_help("If emulating UMBs, keep the UMB around after boot (Mainline DOSBox behavior). If clear, UMB is unmapped when you boot an operating system.");

    Pbool = secprop->Add_bool("keep private area on boot",Property::Changeable::OnlyAtStart,false);
    Pbool->Set_help("If set, keep the DOSBox private area around after boot (Mainline DOSBox behavior). If clear, unmap and discard the private area when you boot an operating system.");

    Pbool = secprop->Add_bool("private area in umb",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("If set, keep private DOS segment in upper memory block, usually segment 0xC800 (Mainline DOSBox behavior)\n"
            "If clear, place private DOS segment at the base of system memory (just below the MCB)");

    Pstring = secprop->Add_string("ver",Property::Changeable::WhenIdle,"");
    Pstring->Set_help("Set DOS version. Specify as major.minor format. A single number is treated as the major version (LFN patch compat). Common settings are:\n"
            "auto (or unset)                  Pick a DOS kernel version automatically\n"
            "3.3                              MS-DOS 3.3 emulation (not tested!)\n"
            "5.0                              MS-DOS 5.0 emulation (recommended for DOS gaming)\n"
            "6.22                             MS-DOS 6.22 emulation\n"
            "7.0                              Windows 95 (pure DOS mode) emulation\n");

    Pbool = secprop->Add_bool("automount",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Enable automatic mount.");

    Pbool = secprop->Add_bool("int33",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Enable INT 33H (mouse) support.");

    Pbool = secprop->Add_bool("int33 hide host cursor if interrupt subroutine",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("If set, the cursor on the host will be hidden if the DOS application provides it's own\n"
                    "interrupt subroutine for the mouse driver to call, which is usually an indication that\n"
                    "the DOS game wishes to draw the cursor with it's own support routines (DeluxePaint II).");

    Pbool = secprop->Add_bool("int33 hide host cursor when polling",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If set, the cursor on the host will be hidden even if the DOS application has also\n"
                    "hidden the cursor in the guest, as long as the DOS application is polling position\n"
                    "and button status. This can be useful for DOS programs that draw the cursor on their\n"
                    "own instead of using the mouse driver, including most games and DeluxePaint II.");

    Pbool = secprop->Add_bool("int33 disable cell granularity",Property::Changeable::WhenIdle,false);
    Pbool->Set_help("If set, the mouse pointer position is reported at full precision (as if 640x200 coordinates) in all modes.\n"
                    "If not set, the mouse pointer position is rounded to the top-left corner of a character cell in text modes.\n"
                    "This option is OFF by default.");

    Pbool = secprop->Add_bool("int 13 extensions",Property::Changeable::WhenIdle,true);
    Pbool->Set_help("Enable INT 13h extensions (functions 0x40-0x48). You will need this enabled if the virtual hard drive image is 8.4GB or larger.");

    Pbool = secprop->Add_bool("biosps2",Property::Changeable::OnlyAtStart,true);
    Pbool->Set_help("Emulate BIOS INT 15h PS/2 mouse services\n"
        "Note that some OS's like Microsoft Windows neither use INT 33h nor\n"
        "probe the AUX port directly and depend on this BIOS interface exclusively\n"
        "for PS/2 mouse support. In other cases there is no harm in leaving this enabled");

    /* bugfix for Yodel "mayday" demo */
    /* TODO: Set this option to default to "true" if it turns out most BIOSes unmask the IRQ during INT 15h AH=86 WAIT */
    Pbool = secprop->Add_bool("int15 wait force unmask irq",Property::Changeable::OnlyAtStart,true);
    Pbool->Set_help("Some demos or games mistakingly use INT 15h AH=0x86 (WAIT) while leaving the IRQs needed for it masked.\n"
            "If this option is set (by default), the necessary IRQs will be unmasked when INT 15 AH=0x86 is used so that the game or demo does not hang.");

    Pbool = secprop->Add_bool("int15 mouse callback does not preserve registers",Property::Changeable::OnlyAtStart,false);
    Pbool->Set_help("Set to true if the guest OS or DOS program assigns an INT 15h mouse callback,\n"
            "but does not properly preserve CPU registers. Diagnostic function only (default off).");

    Pstring = secprop->Add_string("keyboardlayout",Property::Changeable::WhenIdle, "auto");
    Pstring->Set_help("Language code of the keyboard layout (or none).");

    Pbool = secprop->Add_bool("dbcs",Property::Changeable::OnlyAtStart,true);
    Pbool->Set_help("Enable DBCS table.\n"
            "CAUTION: Some software will crash without the DBCS table, including the Open Watcom installer.\n");

    Pbool = secprop->Add_bool("filenamechar",Property::Changeable::OnlyAtStart,true);
    Pbool->Set_help("Enable filename char table");

    Pbool = secprop->Add_bool("collating and uppercase",Property::Changeable::OnlyAtStart,true);
    Pbool->Set_help("Enable collating and uppercase table");

    Pint = secprop->Add_int("files",Property::Changeable::OnlyAtStart,127);
    Pint->Set_help("Number of file handles available to DOS programs. (equivalent to \"files=\" in config.sys)");

    // DEPRECATED, REMOVE
    Pbool = secprop->Add_bool("con device use int 16h to detect keyboard input",Property::Changeable::OnlyAtStart,true);
    Pbool->Set_help("If set, use INT 16h to detect keyboard input (MS-DOS 6.22 behavior). If clear, detect keyboard input by\n"
            "peeking into the BIOS keyboard buffer (Mainline DOSBox behavior). You will need to set this\n"
            "option for programs that hook INT 16h to handle keyboard input ahead of the DOS console.\n"
            "Microsoft Scandisk needs this option to respond to keyboard input correctly.");

    Pbool = secprop->Add_bool("zero memory on int 21h memory allocation",Property::Changeable::OnlyAtStart,false);
    Pbool->Set_help("If set, memory returned by the INT 21h allocation call is zeroed first. This is NOT what\n"
            "DOS actually does, but if set, can help certain DOS games and demos cope with problems\n"
            "related to uninitialized variables in the data or stack segment. If you intend to run a\n"
            "game or demo known to have this problem (Second Unreal, for example), set to true, else\n"
            "set to false. When enabled this option may incur a slight to moderate performance penalty.");

    secprop=control->AddSection_prop("ipx",&Null_Init,true);
    Pbool = secprop->Add_bool("ipx",Property::Changeable::WhenIdle, false);
    Pbool->Set_help("Enable ipx over UDP/IP emulation.");

    secprop=control->AddSection_prop("ne2000",&Null_Init,true);
    MSG_Add("NE2000_CONFIGFILE_HELP",
        "macaddr -- The physical address the emulator will use on your network.\n"
        "           If you have multiple DOSBoxes running on your network,\n"
        "           this has to be changed. Modify the last three number blocks.\n"
        "           I.e. AC:DE:48:88:99:AB.\n"
        "realnic -- Specifies which of your network interfaces is used.\n"
        "           Write \'list\' here to see the list of devices in the\n"
        "           Status Window. Then make your choice and put either the\n"
        "           interface number (2 or something) or a part of your adapters\n"
        "           name, e.g. VIA here.\n"

    );

    Pbool = secprop->Add_bool("ne2000", Property::Changeable::WhenIdle, false);
    Pbool->Set_help("Enable Ethernet passthrough. Requires [Win]Pcap.");

    Phex = secprop->Add_hex("nicbase", Property::Changeable::WhenIdle, 0x300);
    Phex->Set_help("The base address of the NE2000 board.");

    Pint = secprop->Add_int("nicirq", Property::Changeable::WhenIdle, 3);
    Pint->Set_help("The interrupt it uses. Note serial2 uses IRQ3 as default.");

    Pstring = secprop->Add_string("macaddr", Property::Changeable::WhenIdle,"AC:DE:48:88:99:AA");
    Pstring->Set_help("The physical address the emulator will use on your network.\n"
        "If you have multiple DOSBoxes running on your network,\n"
        "this has to be changed for each. AC:DE:48 is an address range reserved for\n"
        "private use, so modify the last three number blocks.\n"
        "I.e. AC:DE:48:88:99:AB.");

    /* TODO: Change default to "nat" and then begin implementing support for emulating
     *       an ethernet connection with DOSBox-X as a NAT/firewall between the guest
     *       and the OS. Sort of like "NAT" mode in VirtualBox. When that works, we
     *       can then compile NE2000 support with and without libpcap/winpcap support. */
    Pstring = secprop->Add_string("realnic", Property::Changeable::WhenIdle,"list");
    Pstring->Set_help("Specifies which of your network interfaces is used.\n"
        "Write \'list\' here to see the list of devices in the\n"
        "Status Window. Then make your choice and put either the\n"
        "interface number (2 or something) or a part of your adapters\n"
        "name, e.g. VIA here.");

    /* floppy controller emulation options and setup */
    secprop=control->AddSection_prop("fdc, primary",&Null_Init,false);

    /* Primary FDC on by default, secondary is not. Most PCs have only one floppy controller. */
    Pbool = secprop->Add_bool("enable",Property::Changeable::OnlyAtStart,false);
    Pbool->Set_help("Enable floppy controller interface");

    Pbool = secprop->Add_bool("pnp",Property::Changeable::OnlyAtStart,true);
    Pbool->Set_help("List floppy controller in ISA PnP BIOS enumeration");

    Pint = secprop->Add_int("irq",Property::Changeable::WhenIdle,0/*use FDC default*/);
    Pint->Set_help("IRQ used by floppy controller. Set to 0 for default.\n"
        "WARNING: Setting the IRQ to non-standard values will not work unless the guest OS is using the ISA PnP BIOS to detect the floppy controller.\n"
        "         Setting the IRQ to one already occupied by another device or IDE controller will trigger \"resource conflict\" errors in Windows 95.\n"
        "         Normally, floppy controllers use IRQ 6.");

    Phex = secprop->Add_hex("io",Property::Changeable::WhenIdle,0/*use FDC default*/);
    Phex->Set_help("Base I/O port for floppy controller. Set to 0 for default.\n"
        "WARNING: Setting the I/O port to non-standard values will not work unless the guest OS is using the ISA PnP BIOS to detect the IDE controller.\n"
        "         Standard I/O ports are 3F0 and 370.");

    Pint = secprop->Add_int("dma",Property::Changeable::WhenIdle,-1/*use FDC default*/);
    Pint->Set_help("DMA channel for floppy controller. Set to -1 for default.\n"
        "WARNING: Setting the DMA channel to non-standard values will not work unless the guest OS is using the ISA PnP BIOS to detect the IDE controller.\n"
        "         Standard DMA channel is 2.");

    Pbool = secprop->Add_bool("int13fakev86io",Property::Changeable::WhenIdle,false);
    Pbool->Set_help(
        "If set, and int13fakeio is set, certain INT 13h commands will\n"
        "cause floppy emulation to issue fake CPU I/O traps (GPF) in\n"
        "virtual 8086 mode and a fake IRQ signal. you must enable this option\n"
        "if you want 32-bit floppy access in Windows 95 to work with DOSBox.");

    Pbool = secprop->Add_bool("instant mode",Property::Changeable::WhenIdle,false);
    Pbool->Set_help(
        "If set, all floppy operations are 'instantaneous', they are carried\n"
        "out without any delay. Real hardware of course has motor, command\n"
        "and data I/O delays and so this option is off by default for realistic\n"
        "emulation.");

    Pbool = secprop->Add_bool("auto-attach to int 13h",Property::Changeable::WhenIdle,true);
    Pbool->Set_help(
        "If set, DOSBox-X will automatically attach a disk image as being\n"
        "inserted into a floppy drive attached to the controller when imgmount is used\n"
        "to mount a disk image to drive 0/1 or A/B. If not set, you must specify\n"
        "the -fdc option to imgmount to attach drive A/B to the floppy controller\n"
        "manually. You must use the -fdc option regardless if loading floppies into\n"
        "drives attached to any other FDC than the primary controller");

    /* FIXME: From http://wiki.osdev.org/Floppy_Disk_Controller#Configure
     *
     *    "The three modes are PC-AT mode, PS/2 mode, and Model 30 mode. The most likely mode ... is model 30 mode.
     *    You may find some pre-1996 Pentium machines using PS/2 mode. You can ignore PC-AT mode."
     *
     *    What? What the fuck are you talking about?
     *
     *    "AT mode" seems to imply the presense of port 3F7. PS/2 mode seems to imply the presense of 3F0-3F1 and 3F7.
     *    A Toshiba laptop (Satellite Pro 465CDX) has port 3F7 but not 3F0-3F1. By other documentation I've found, that
     *    means this laptop (which came out late 1997) is running in AT mode! There's plenty of hardware running in both
     *    PS/2 and AT mode, even some very old stuff in my pile of junk dating back to 1990!
     *
     *    Somehow I think this information is as correct as their ATAPI programming docs on how to read CD-ROM
     *    sectors: it's a start but it's mostly wrong. Hopefully DOSLIB will shed light on what the real differences
     *    are and what is most common. --J.C. */
    Pstring = secprop->Add_string("mode",Property::Changeable::WhenIdle,"ps2");
    Pstring->Set_help(
        "Floppy controller mode. What the controller acts like.\n"
        "  ps2                          PS/2 mode (most common)\n"
        "  ps2_model30                  PS/2 model 30\n"
        "  at                           AT mode\n"
        "  xt                           PC/XT mode");

    /* FIXME: Not yet implemented. Future plans */
    Pstring = secprop->Add_string("chip",Property::Changeable::WhenIdle,"82077aa");
    Pstring->Set_help(
        "Floppy controller chipset\n"
        "  82077aa                      Intel 82077AA chipset\n"
        "  82072                        Intel 82072 chipset\n"
        "  nec_uPD765                   NEC uPD765 chipset\n"
        "  none                         No chipset (For PC/XT mode)");

    /* IDE emulation options and setup */
    for (size_t i=0;i < MAX_IDE_CONTROLLERS;i++) {
        secprop=control->AddSection_prop(ide_names[i],&Null_Init,false);//done

        /* Primary and Secondary are on by default, Teritary and Quaternary are off by default.
         * Throughout the life of the IDE interface it was far more common for a PC to have just
         * a Primary and Secondary interface */
        Pbool = secprop->Add_bool("enable",Property::Changeable::OnlyAtStart,(i < 2) ? true : false);
        if (i == 0) Pbool->Set_help("Enable IDE interface");

        Pbool = secprop->Add_bool("pnp",Property::Changeable::OnlyAtStart,true);
        if (i == 0) Pbool->Set_help("List IDE device in ISA PnP BIOS enumeration");

        Pint = secprop->Add_int("irq",Property::Changeable::WhenIdle,0/*use IDE default*/);
        if (i == 0) Pint->Set_help("IRQ used by IDE controller. Set to 0 for default.\n"
                "WARNING: Setting the IRQ to non-standard values will not work unless the guest OS is using the ISA PnP BIOS to detect the IDE controller.\n"
                "         Setting the IRQ to one already occupied by another device or IDE controller will trigger \"resource conflict\" errors in Windows 95.\n"
                "         Using IRQ 9, 12, 13, or IRQ 2-7 may cause problems with MS-DOS CD-ROM drivers.");

        secprop->Add_hex("io",Property::Changeable::WhenIdle,0/*use IDE default*/);
        if (i == 0) Pint->Set_help("Base I/O port for IDE controller. Set to 0 for default.\n"
                "WARNING: Setting the I/O port to non-standard values will not work unless the guest OS is using the ISA PnP BIOS to detect the IDE controller.\n"
                "         Using any port other than 1F0, 170, 1E8 or 168 can prevent MS-DOS CD-ROM drivers from detecting the IDE controller.");

        Phex = secprop->Add_hex("altio",Property::Changeable::WhenIdle,0/*use IDE default*/);
        if (i == 0) Pint->Set_help("Alternate I/O port for IDE controller (alt status, etc). Set to 0 for default.\n"
                "WARNING: Setting the I/O port to non-standard values will not work unless the guest OS is using the ISA PnP BIOS to detect the IDE controller.\n"
                "         For best compatability set this value to io+0x206, for example, io=1F0 altio=3F6.\n"
                "         The primary IDE controller will not claim port 3F7 if the primary floppy controller is enabled due to I/O port overlap in the 3F0-3F7 range.");

        Pbool = secprop->Add_bool("int13fakeio",Property::Changeable::WhenIdle,false);
        if (i == 0) Pbool->Set_help(
                "If set, force IDE state change on certain INT 13h commands.\n"
                "IDE registers will be changed as if BIOS had carried out the action.\n"
                "If you are running Windows 3.11 or Windows 3.11 Windows for Workgroups\n"
                "you must enable this option (and use -reservecyl 1) if you want 32-bit\n"
                "disk access to work correctly in DOSBox.");

        Pbool = secprop->Add_bool("int13fakev86io",Property::Changeable::WhenIdle,false);
        if (i == 0) Pbool->Set_help(
                "If set, and int13fakeio is set, certain INT 13h commands will\n"
                "cause IDE emulation to issue fake CPU I/O traps (GPF) in\n"
                "virtual 8086 mode and a fake IRQ signal. you must enable this option\n"
                "if you want 32-bit disk access in Windows 95 to work with DOSBox.");

        Pbool = secprop->Add_bool("enable pio32",Property::Changeable::WhenIdle,false);
        if (i == 0) Pbool->Set_help(
                "If set, 32-bit I/O reads and writes are handled directly (much like PCI IDE implementations)\n"
                "If clear, 32-bit I/O will be handled as if two 16-bit I/O (much like ISA IDE implementations)");

        Pbool = secprop->Add_bool("ignore pio32",Property::Changeable::WhenIdle,false);
        if (i == 0) Pbool->Set_help(
                "If 32-bit I/O is enabled, attempts to read/write 32-bit I/O will be ignored entirely.\n"
                "In this way, you can have DOSBox emulate one of the strange quirks of 1995-1997 era\n"
                "laptop hardware");

        Pint = secprop->Add_int("cd-rom spinup time",Property::Changeable::WhenIdle,0/*use IDE or CD-ROM default*/);
        if (i == 0) Pint->Set_help("Emulated CD-ROM time in ms to spin up if CD is stationary.\n"
                "Set to 0 to use controller or CD-ROM drive-specific default.");

        Pint = secprop->Add_int("cd-rom spindown timeout",Property::Changeable::WhenIdle,0/*use IDE or CD-ROM default*/);
        if (i == 0) Pint->Set_help("Emulated CD-ROM time in ms that drive will spin down automatically when not in use\n"
                "Set to 0 to use controller or CD-ROM drive-specific default.");

        Pint = secprop->Add_int("cd-rom insertion delay",Property::Changeable::WhenIdle,0/*use IDE or CD-ROM default*/);
        if (i == 0) Pint->Set_help("Emulated CD-ROM time in ms that drive will report \"medium not present\"\n"
                "to emulate the time it takes for someone to take out a CD and insert a new one when\n"
                "DOSBox is instructed to swap or change CDs.\n"
                "When running Windows 95 or higher a delay of 4000ms is recommended to ensure that\n"
                "auto-insert notification triggers properly.\n"
                "Set to 0 to use controller or CD-ROM drive-specific default.");
    }

    //TODO ?
    control->AddSection_line("autoexec",&Null_Init);
    MSG_Add("AUTOEXEC_CONFIGFILE_HELP",
        "Lines in this section will be run at startup.\n"
        "You can put your MOUNT lines here.\n"
    );
    MSG_Add("CONFIGFILE_INTRO",
            "# This is the configuration file for DOSBox %s. (Please use the latest version of DOSBox)\n"
            "# Lines starting with a # are comment lines and are ignored by DOSBox.\n"
            "# They are used to (briefly) document the effect of each option.\n"
        "# To write out ALL options, use command 'config -all' with -wc or -writeconf options.\n");
    MSG_Add("CONFIG_SUGGESTED_VALUES", "Possible values");
}

int utf8_encode(char **ptr,char *fence,uint32_t code) {
    int uchar_size=1;
    char *p = *ptr;

    if (!p) return UTF8ERR_NO_ROOM;
    if (code >= (uint32_t)0x80000000UL) return UTF8ERR_INVALID;
    if (p >= fence) return UTF8ERR_NO_ROOM;

    if (code >= 0x4000000) uchar_size = 6;
    else if (code >= 0x200000) uchar_size = 5;
    else if (code >= 0x10000) uchar_size = 4;
    else if (code >= 0x800) uchar_size = 3;
    else if (code >= 0x80) uchar_size = 2;

    if ((p+uchar_size) > fence) return UTF8ERR_NO_ROOM;

    switch (uchar_size) {
        case 1: *p++ = (char)code;
            break;
        case 2: *p++ = (char)(0xC0 | (code >> 6));
            *p++ = (char)(0x80 | (code & 0x3F));
            break;
        case 3: *p++ = (char)(0xE0 | (code >> 12));
            *p++ = (char)(0x80 | ((code >> 6) & 0x3F));
            *p++ = (char)(0x80 | (code & 0x3F));
            break;
        case 4: *p++ = (char)(0xF0 | (code >> 18));
            *p++ = (char)(0x80 | ((code >> 12) & 0x3F));
            *p++ = (char)(0x80 | ((code >> 6) & 0x3F));
            *p++ = (char)(0x80 | (code & 0x3F));
            break;
        case 5: *p++ = (char)(0xF8 | (code >> 24));
            *p++ = (char)(0x80 | ((code >> 18) & 0x3F));
            *p++ = (char)(0x80 | ((code >> 12) & 0x3F));
            *p++ = (char)(0x80 | ((code >> 6) & 0x3F));
            *p++ = (char)(0x80 | (code & 0x3F));
            break;
        case 6: *p++ = (char)(0xFC | (code >> 30));
            *p++ = (char)(0x80 | ((code >> 24) & 0x3F));
            *p++ = (char)(0x80 | ((code >> 18) & 0x3F));
            *p++ = (char)(0x80 | ((code >> 12) & 0x3F));
            *p++ = (char)(0x80 | ((code >> 6) & 0x3F));
            *p++ = (char)(0x80 | (code & 0x3F));
            break;
    }

    *ptr = p;
    return 0;
}

int utf8_decode(const char **ptr,const char *fence) {
    const char *p = *ptr;
    int uchar_size=1;
    int ret = 0,c;

    if (!p) return UTF8ERR_NO_ROOM;
    if (p >= fence) return UTF8ERR_NO_ROOM;

    ret = (unsigned char)(*p);
    if (ret >= 0xFE) { p++; return UTF8ERR_INVALID; }
    else if (ret >= 0xFC) uchar_size=6;
    else if (ret >= 0xF8) uchar_size=5;
    else if (ret >= 0xF0) uchar_size=4;
    else if (ret >= 0xE0) uchar_size=3;
    else if (ret >= 0xC0) uchar_size=2;
    else if (ret >= 0x80) { p++; return UTF8ERR_INVALID; }

    if ((p+uchar_size) > fence)
        return UTF8ERR_NO_ROOM;

    switch (uchar_size) {
        case 1: p++;
            break;
        case 2: ret = (ret&0x1F)<<6; p++;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= c&0x3F;
            break;
        case 3: ret = (ret&0xF)<<12; p++;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= (c&0x3F)<<6;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= c&0x3F;
            break;
        case 4: ret = (ret&0x7)<<18; p++;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= (c&0x3F)<<12;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= (c&0x3F)<<6;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= c&0x3F;
            break;
        case 5: ret = (ret&0x3)<<24; p++;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= (c&0x3F)<<18;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= (c&0x3F)<<12;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= (c&0x3F)<<6;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= c&0x3F;
            break;
        case 6: ret = (ret&0x1)<<30; p++;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= (c&0x3F)<<24;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= (c&0x3F)<<18;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= (c&0x3F)<<12;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= (c&0x3F)<<6;
            c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
            ret |= c&0x3F;
            break;
    }

    *ptr = p;
    return ret;
}

int utf16le_encode(char **ptr,char *fence,uint32_t code) {
    char *p = *ptr;

    if (!p) return UTF8ERR_NO_ROOM;
    if (code > 0x10FFFF) return UTF8ERR_INVALID;
    if (code > 0xFFFF) { /* UTF-16 surrogate pair */
        uint32_t lo = (code - 0x10000) & 0x3FF;
        uint32_t hi = ((code - 0x10000) >> 10) & 0x3FF;
        if ((p+2+2) > fence) return UTF8ERR_NO_ROOM;
        *p++ = (char)( (hi+0xD800)       & 0xFF);
        *p++ = (char)(((hi+0xD800) >> 8) & 0xFF);
        *p++ = (char)( (lo+0xDC00)       & 0xFF);
        *p++ = (char)(((lo+0xDC00) >> 8) & 0xFF);
    }
    else if ((code&0xF800) == 0xD800) { /* do not allow accidental surrogate pairs (0xD800-0xDFFF) */
        return UTF8ERR_INVALID;
    }
    else {
        if ((p+2) > fence) return UTF8ERR_NO_ROOM;
        *p++ = (char)( code       & 0xFF);
        *p++ = (char)((code >> 8) & 0xFF);
    }

    *ptr = p;
    return 0;
}

int utf16le_decode(const char **ptr,const char *fence) {
    const char *p = *ptr;
    unsigned int ret,b=2;

    if (!p) return UTF8ERR_NO_ROOM;
    if ((p+1) >= fence) return UTF8ERR_NO_ROOM;

    ret = (unsigned char)p[0];
    ret |= ((unsigned int)((unsigned char)p[1])) << 8;
    if (ret >= 0xD800U && ret <= 0xDBFFU)
        b=4;
    else if (ret >= 0xDC00U && ret <= 0xDFFFU)
        { p++; return UTF8ERR_INVALID; }

    if ((p+b) > fence)
        return UTF8ERR_NO_ROOM;

    p += 2;
    if (ret >= 0xD800U && ret <= 0xDBFFU) {
        /* decode surrogate pair */
        unsigned int hi = ret & 0x3FFU;
        unsigned int lo = (unsigned char)p[0];
        lo |= ((unsigned int)((unsigned char)p[1])) << 8;
        p += 2;
        if (lo < 0xDC00U || lo > 0xDFFFU) return UTF8ERR_INVALID;
        lo &= 0x3FFU;
        ret = ((hi << 10U) | lo) + 0x10000U;
    }

    *ptr = p;
    return (int)ret;
}

