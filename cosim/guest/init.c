/* SPDX-License-Identifier: MIT
 *
 * The whole of the guest's userspace.
 *
 * It reads a file the host put on the card before the machine started, and
 * writes one back.  That is deliberate: the kernel's own probe messages
 * already prove that INQUIRY and READ CAPACITY worked, but a file read and a
 * file written mean the filesystem's blocks crossed the SCSI bus in both
 * directions - a byte at a time, through the Verilated 5380, into and out of
 * an SD card model.
 *
 * Static, because there is no libc on the card and nothing to load one from.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>

int main(void)
{
    char buf[256];
    int fd, n;

    printf("WISH5380-COSIM: init running\n");

    fd = open("/hello.txt", O_RDONLY);
    if (fd < 0) {
        printf("WISH5380-COSIM: FAIL cannot open /hello.txt\n");
    } else {
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) {
            printf("WISH5380-COSIM: FAIL cannot read /hello.txt\n");
        } else {
            buf[n] = 0;
            if (n > 0 && buf[n - 1] == '\n') {
                buf[n - 1] = 0;
            }
            printf("WISH5380-COSIM: read back '%s'\n", buf);
        }
    }

    fd = open("/written-by-the-guest.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("WISH5380-COSIM: FAIL cannot create a file\n");
    } else {
        const char *msg = "the guest wrote this through the RTL\n";
        if (write(fd, msg, strlen(msg)) != (int)strlen(msg)) {
            printf("WISH5380-COSIM: FAIL short write\n");
        }
        fsync(fd);
        close(fd);
        printf("WISH5380-COSIM: wrote a file back\n");
    }

    sync();
    printf("WISH5380-COSIM: done\n");
    fflush(NULL);
    sleep(1);
    reboot(RB_POWER_OFF);
    for (;;) {
        pause();
    }
}
