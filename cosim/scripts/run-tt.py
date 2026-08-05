#!/usr/bin/env python3
"""Boot an emulated Atari TT with the Verilated 5380 as its SCSI controller.

The machine is Hatari's, patched to load libwish5380rtl.so in place of its own
5380 *and* its own disk - see cosim/scripts/build-hatari.sh.  What this script
adds is the wiring and a way to check the result without a person watching it.

    run-tt.py                       boot and print the console
    run-tt.py -w 'drives:      ABC' exit 1 unless that appears
    run-tt.py --stock               Hatari's own 5380 instead, for comparison
    run-tt.py -- --trace scsi_cmd   extra arguments for Hatari

The card image is made by cosim/scripts/make-tt-disk.py and is created here if
it is missing, because a disk of the right shape is not something to have to
remember.

Why an Atari and not the Macintosh the design is shaped for: EmuTOS.  The TT
has a free TOS that a script can fetch, and the Macintosh has no free ROM at
all - which is the same argument cosim/README.md makes about the Sun-3, and
the reason this lineage exists on top of that one.

The console is EmuTOS's, redirected to stdout with --conout.  There is no
serial line to drive and nothing to type: an unattended check puts what it
wants to prove in C:\\AUTO and reads it back off stdout.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
WORK = os.path.join(ROOT, 'work')

HATARI = os.path.join(WORK, 'hatari-build', 'src', 'hatari')
LIB = os.path.join(WORK, 'lib', 'libwish5380rtl.so')
TOS = os.path.join(WORK, 'emutos', 'emutos-512k-1.4', 'etos512us.img')
CARD = os.path.join(WORK, 'hatari-run', 'tt-card.img')

# Hatari's bus-error warnings during the machine probe are what a TOS does on
# purpose - it reads addresses that may not be there and catches the fault -
# so they are noise here and not news.
NOISE = re.compile(r'^WARN : Bus Error ')


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.split('\n')[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='\n'.join(__doc__.split('\n')[1:]))
    ap.add_argument('-w', '--want', action='append', default=[], metavar='TEXT',
                    help='exit 1 unless this appears on the console')
    ap.add_argument('-c', '--card', default=CARD,
                    help='the SD card image behind the chip')
    ap.add_argument('--size-mb', type=int, default=32,
                    help='size of the card image, if it has to be made')
    ap.add_argument('--auto', metavar='TEXT',
                    default='the 5380 is a replica and the disk is a memory card',
                    help='what the program in C:\\AUTO prints')
    ap.add_argument('--stock', action='store_true',
                    help="Hatari's own 5380 and its own disk, for comparison")
    ap.add_argument('--vbls', type=int, default=4000,
                    help='how long to run, in video frames')
    ap.add_argument('--tos', default=TOS)
    ap.add_argument('-v', '--verbose', action='store_true',
                    help='keep the bus-error warnings a TOS causes on purpose')
    ap.add_argument('rest', nargs='*', help='further arguments for Hatari')
    args = ap.parse_args()

    if not os.path.exists(args.card):
        os.makedirs(os.path.dirname(args.card), exist_ok=True)
        subprocess.check_call([sys.executable,
                               os.path.join(HERE, 'make-tt-disk.py'),
                               args.card, '--size-mb', str(args.size_mb),
                               '--auto', args.auto])

    for path, what in ((HATARI, 'cosim/scripts/build-hatari.sh'),
                       (args.tos, 'cosim/scripts/build-hatari.sh'),
                       (LIB, 'make -C cosim/rtl')):
        if not os.path.exists(path) and not (args.stock and path is LIB):
            sys.exit('%s is missing - run %s' % (path, what))

    cmd = [HATARI, '--machine', 'tt', '--tos', args.tos,
           '--confirm-quit', 'off', '--sound', 'off', '--conout', '2',
           '--run-vbls', str(args.vbls)]
    env = dict(os.environ,
               SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')

    if args.stock:
        # Hatari's own model reads the same image as a plain SCSI disk, which
        # is what makes the two runs comparable at all.
        cmd += ['--scsi', '0=' + args.card]
        env.pop('WISH5380_LIB', None)
        env.pop('WISH5380_SD', None)
    else:
        env['WISH5380_LIB'] = LIB
        env['WISH5380_SD'] = args.card

    cmd += args.rest

    seen = []
    proc = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True,
                            errors='replace')
    for line in proc.stdout:
        seen.append(line)
        if args.verbose or not NOISE.match(line):
            sys.stdout.write(line)
            sys.stdout.flush()
    proc.wait()

    console = ''.join(seen)
    missing = [w for w in args.want if w not in console]
    for w in missing:
        print('run-tt: never saw %r' % w, file=sys.stderr)
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
