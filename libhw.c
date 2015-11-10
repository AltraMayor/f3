#include "libhw.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

int com_fd = 0;


int hw_open_com(char *device)
{
    if (com_fd != 0)
    {
        return 0;
    }

    com_fd = open(device, O_RDWR | O_NOCTTY);
    if (com_fd < 0)
    {
        perror(device);
        return -1;
    }

    return 0;
}

int hw_close_com()
{
    if (com_fd > 0)
    {
        close(com_fd);
        return 0;
    }

    return -1;
}

int hw_disconnect(bool state)
{
    int com_status = 0;
    int ret = 0;

    ret = ioctl(com_fd, TIOCMGET, &com_status);
    if (ret < 0)
    {
        perror("HW status get");
        return -1;
    }

    if (state)
    {
        com_status |= TIOCM_DTR;
    }
    else
    {
        com_status &= ~TIOCM_DTR;
    }

    ret = ioctl(com_fd, TIOCMSET, &com_status);
    if (ret < 0)
    {
        perror("HW status set");
        return -1;
    }

    return 0;
}

void hw_toggle()
{
    hw_disconnect(true);

    /* Don't hurry; the hardware need to do some slow physical operations */
    sleep(1);
    hw_disconnect(false);
}
