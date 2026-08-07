#!/usr/bin/env python3
"""Boot the emulated Sun-3/60 with the Verilated 5380 on its onboard SCSI.

The Sun-3 machine model lives in a QEMU fork, not in this tree - see
cosim/scripts/build-sun3-qemu.sh.  What this script adds is the wiring: it
points the machine's si-rtl= property at libwish5380rtl.so, gives it a disk
image, and drives the PROM monitor so a boot can be checked without a person
watching it.

    run-sun3.py                             boot and print the console
    run-sun3.py 'b sd()'                    ... then type that at ">"
    run-sun3.py -w 'sd0' -q 'b sd()'        exit 1 unless "sd0" appears
    run-sun3.py --no-si                     without the board, for comparison
    run-sun3.py -- -d guest_errors          extra arguments for QEMU
    run-sun3.py -s 'a):=a' 'b sd()'         answer a prompt the guest prints

The positional commands are typed at the PROM's ">" and nothing else; -s is
for everything after the PROM has handed over, where what to say depends on
what the guest asked.  Each rule is `PATTERN:=TEXT`, they fire in the order
given, once each, and the pattern is a Python regular expression matched
against the console as it arrives.

**The guest writes to a copy**, `<image>-run.<ext>` beside the one --image
names, so a run does not change what the next one starts from; --keep gives
it the original instead.  This matters more than it looks: the library loads
the image at startup and writes it back from wish_rtl_flush, so a run that
ends cleanly rewrites the disk while a run that is killed does not - and this
script kills QEMU.  Whether two runs of the same command started from the same
disk therefore depended on how the first one happened to die.  run-cosim.py
does the same thing for the ISA card's image and for the same reason.

There is a script very like this one in the QEMU fork.  This is not a copy for
its own sake: that one hardwires its own build directory and knows nothing
about the shared library, and it is not ours to change.

The console is the *fourth* serial line.  The 3/60 wires two Z8530 pairs -
keyboard, mouse, TTY B, TTY A - and with no keyboard attached the PROM falls
back to "RS232 Port A", which is TTY A.  Hence the three null serials.  It is
also a raw line with no discipline, and the PROM draws its progress spinner
with backspaces, so this needs a pty rather than a pipe.
"""
import argparse
import os
import pty
import re
import select
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SUN3 = os.environ.get('SUN3_QEMU', os.path.join(ROOT, 'work', 'sun3-qemu'))
FORK = os.environ.get('SUN3_FORK', os.path.expanduser('~/qemu-sun3'))

SPINNER = re.compile(rb'[\b]|[-\\|/]{3,}')


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('commands', nargs='*',
                    help='lines to type once the ">" prompt appears')
    ap.add_argument('-w', '--wait-for', action='append', default=[],
                    help='pattern that must appear; may be repeated')
    ap.add_argument('-q', '--quiet', action='store_true',
                    help='do not echo the console as it arrives')
    ap.add_argument('-m', '--memory', default='4M')
    ap.add_argument('-t', '--timeout', type=float, default=600,
                    help='seconds to allow the run in total')
    ap.add_argument('-i', '--idle', type=float, default=60,
                    help='give up after this long with nothing on the console')
    ap.add_argument('--qemu', default=os.path.join(SUN3, 'build',
                                                   'qemu-system-m68k'))
    ap.add_argument('--prom', default=os.path.join(FORK, 'roms',
                                                   'sun3_60_v3.0.1.bin'))
    ap.add_argument('--rtl', default=os.path.join(ROOT, 'work', 'lib',
                                                  'libwish5380rtl.so'))
    ap.add_argument('--image', default=os.path.join(ROOT, 'work', 'sun3',
                                                    'disk.img'))
    ap.add_argument('--keep', action='store_true',
                    help='let the guest write to --image itself, instead of '
                         'to a copy beside it')
    ap.add_argument('-s', '--send', action='append', default=[],
                    metavar='PATTERN:=TEXT',
                    help='type TEXT when PATTERN appears; may be repeated, '
                         'and the rules fire in order, once each')
    ap.add_argument('--no-si', action='store_true',
                    help='leave the SCSI slot empty')
    ap.add_argument('--icount', type=int, default=8, metavar='SHIFT',
                    help='QEMU instruction counter: one instruction every '
                         '2^SHIFT ns of guest time.  8 is about a real 3/60; '
                         '0 turns it off and lets guest time follow the wall '
                         'clock, which is neither fast nor faithful')
    ap.add_argument('--trace', action='store_true',
                    help='ask the RTL library for a waveform')
    # Split on "--" ourselves.  argparse consumes it and then hands the rest
    # to the "commands" positional, which types them at the PROM prompt - the
    # QEMU arguments end up as monitor commands and nothing says so.
    argv = sys.argv[1:]
    if '--' in argv:
        cut = argv.index('--')
        argv, extra = argv[:cut], argv[cut + 1:]
    else:
        extra = []
    args = ap.parse_args(argv)

    for what, path in (('QEMU', args.qemu), ('PROM', args.prom)):
        if not os.path.exists(path):
            sys.exit(f'{what} missing: {path}')

    machine = f'sun3'
    if not args.no_si:
        if not os.path.exists(args.rtl):
            sys.exit(f'RTL library missing: {args.rtl} - make -C cosim/rtl')
        machine += f',si-rtl={args.rtl}'
        image = args.image
        if os.path.exists(image) and not args.keep:
            # A run should not change what the next one starts from.  The
            # library loads the image at startup and writes it back from
            # wish_rtl_flush, which wish_rtl_free calls - so a run that ends
            # cleanly rewrites the disk and a run that is killed does not,
            # and this script kills QEMU.  Two runs of the same command were
            # therefore not starting from the same disk, which is no way to
            # chase an intermittent fault.
            base, ext = os.path.splitext(image)
            run = base + '-run' + ext
            shutil.copyfile(image, run)
            image = run
        if os.path.exists(image):
            machine += f',si-image={image}'

    # Guest time has to come from somewhere, and the wall clock is the wrong
    # place: a Verilated 5380 answers far slower than the part, so a guest
    # whose clock follows real time sees every command take an age and starts
    # timing them out.  The instruction counter makes guest time count guest
    # instructions instead, which is what a driver's timeout measures on real
    # hardware.  A shift of eight is one instruction every 256 ns - about the
    # four MIPS of a 20 MHz 68020, so the machine runs at roughly its own
    # speed.  A smaller shift makes the guest faster than the real one and its
    # probes start failing; that is a fault in the pacing and not in the chip.
    clock = []
    if args.icount:
        clock = ['-icount', f'shift={args.icount},sleep=off']

    cmd = [args.qemu,
           '-M', machine, '-m', args.memory, '-bios', args.prom,
           '-display', 'none',
           '-serial', 'null', '-serial', 'null', '-serial', 'null',
           '-serial', 'stdio'] + clock + extra

    env = dict(os.environ)
    if args.trace:
        env['WISH_RTL_TRACE'] = '1'

    pid, fd = pty.fork()
    if pid == 0:
        os.execve(cmd[0], cmd, env)
        os._exit(1)

    sendq = []
    for rule in args.send:
        if ':=' not in rule:
            sys.exit(f'--send wants PATTERN:=TEXT, not {rule!r}')
        pat, text = rule.split(':=', 1)
        sendq.append((re.compile(pat.encode()), text))

    buf = b''
    log = b''
    scanned = 0
    pending = list(args.commands)
    deadline = time.time() + args.timeout
    last_sent = None
    last_out = time.time()

    try:
        while time.time() < deadline:
            r, _, _ = select.select([fd], [], [], 0.5)
            if not r:
                continue
            try:
                chunk = os.read(fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            clean = SPINNER.sub(b'', chunk)
            if not args.quiet:
                sys.stdout.buffer.write(clean)
                sys.stdout.buffer.flush()
            buf += clean
            log += clean
            last_out = time.time()

            # The prompt is a ">" at the start of a line with nothing after it
            # yet; wait a beat so a command's own echo does not look like one.
            if buf.endswith(b'>') and pending:
                time.sleep(0.3)
                line = pending.pop(0)
                os.write(fd, line.encode() + b'\r')
                last_sent = time.time()
                buf = b''
            # A guest's prompt is not the PROM's: it can be anything, and it
            # arrives whenever the machine gets there.  Only the head rule is
            # armed, so a scripted exchange happens in the order it was
            # written even if a later pattern would match sooner.
            if sendq:
                pat, text = sendq[0]
                m = pat.search(log, scanned)
                if m:
                    time.sleep(0.3)
                    os.write(fd, text.encode() + b'\r')
                    last_sent = time.time()
                    scanned = len(log)
                    buf = b''
                    sendq.pop(0)

            # Stop when the machine goes quiet, not a fixed time after the
            # last command: booting an operating system through a Verilated
            # chip takes minutes, and most of them are silent ones.
            # A rule that never fires is a failed run and not a reason to
            # keep waiting: the machine has stopped saying anything, so it is
            # not about to ask.
            if not pending and last_sent and time.time() - last_out > args.idle:
                break
    finally:
        try:
            os.write(fd, b'\x01x')      # Ctrl-a x
            time.sleep(0.2)
        except OSError:
            pass
        os.close(fd)
        try:
            os.kill(pid, 9)
            os.waitpid(pid, 0)
        except OSError:
            pass

    text = log.decode('utf-8', 'replace')
    missing = [p for p in args.wait_for if not re.search(p, text)]
    for p in missing:
        print(f'\nNOT SEEN: {p}', file=sys.stderr)
    for pat, _ in sendq:
        print(f'\nNEVER PROMPTED: {pat.pattern.decode()}', file=sys.stderr)
    return 1 if missing or sendq else 0


if __name__ == '__main__':
    sys.exit(main())
