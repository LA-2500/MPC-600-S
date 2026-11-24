// USB DATA MODE: the SD card as a USB drive on a computer. The MPC 600 S lets go of the card
// (unmounted), and the USB port becomes a plain USB drive instead of audio and MIDI. On exit
// the drive goes, the card is checked, and the MPC 600 S takes it back.
//
// The device side (usbmode_hw.c) does each step in a thread of its own, so the screen never
// waits on it; the page asks how it is going each frame.
#pragma once
#include <stddef.h>

enum {
    USBM_IDLE,       // not in USB mode: the card is the MPC's
    USBM_WORKING,    // connecting, exiting or repairing
    USBM_CONNECTED,  // the card is the computer's
    USBM_FAILED,     // could not connect: the card is the MPC's again, as before
    USBM_DONE,       // exited: the card is back and checked
    USBM_ERRORS,     // exited: the card is back but has errors, so it is mounted read-only
    USBM_NO_CARD,    // exited: the card was taken out
};

enum { USBM_CONNECT, USBM_EXIT, USBM_REPAIR };

// While connected: what the computer is doing.
enum {
    USBM_HOST = 1,    // a computer is on the cable and has set the drive up
    USBM_CABLE = 2,   // the cable is in (the computer may not have set it up yet)
    USBM_EJECTED = 4, // the computer has ejected the drive: safe to exit
    USBM_CARD_OUT = 8 // the card has been taken out of the MPC
};

struct usbmode_hooks {
    int (*begin)(int op);                      // starts op; 0, or -1 when a step is still running
    int (*state)(int *flags, char *msg, size_t n); // USBM_*, the USBM_HOST... flags, and a message for failures
    int (*elapsed_ms)(void);                   // how long the running step has taken
};

// The device's (usbmode_hw.c). input_src: the OS's Input setting, put back on exit.
const struct usbmode_hooks *usbmode_hw(const int *input_src);
int usbmode_card_away(void); // 1 while the card is not the MPC's: nothing may write there

// USB individual outs: the stock USB gadget's audio with 8 channels to the computer (on), or its own
// stereo again (off). USB audio and MIDI let go of first and opened again after. Waits about two
// seconds: call it from a thread of its own. 0, or -1 when the port could not be found or bound.
int usbmode_outs(int on, const int *input_src);
