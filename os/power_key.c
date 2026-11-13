#include "power_key.h"
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <unistd.h>

#define MAX_DEVICES 32

static int fds[MAX_DEVICES], count;

int power_key_open(void) {
    for (int i = 0; i < MAX_DEVICES && count < MAX_DEVICES; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned char keys[KEY_MAX / 8 + 1];
        memset(keys, 0, sizeof keys);
        int has = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) >= 0 && (keys[KEY_POWER / 8] & (1 << (KEY_POWER % 8)));
        if (!has) {
            close(fd);
            continue;
        }
        // Held exclusively so the system cannot act on a press before the prompt is answered.
        ioctl(fd, EVIOCGRAB, 1);
        fds[count++] = fd;
    }
    return count;
}

int power_key_pressed(void) {
    int pressed = 0;
    for (int i = 0; i < count; i++) {
        struct input_event ev[16];
        ssize_t n;
        while ((n = read(fds[i], ev, sizeof ev)) > 0) {
            for (size_t e = 0; e < (size_t)n / sizeof ev[0]; e++)
                if (ev[e].type == EV_KEY && ev[e].code == KEY_POWER && ev[e].value == 1) pressed = 1;
        }
    }
    return pressed;
}

void power_off(int force) {
    sync();
    if (force) reboot(RB_POWER_OFF);
    else if (system("systemctl --no-block poweroff") != 0) reboot(RB_POWER_OFF);
}
