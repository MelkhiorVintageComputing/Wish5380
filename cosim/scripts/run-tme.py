#!/usr/bin/env python3
"""Drive TME, the other Sun-3 emulator, and the machine console it opens.

TME is not part of this project and nothing in `src/` or `tb/` depends on it.
It is here for one job: SunOS 4.1.1 installs itself under TME, and its own
`installboot` writes the boot block we cannot write ourselves - 4.1.1's
installboot patches `/boot`'s block list and a checksum into a copy of
`bootsd`, and the array it lands in is in bss, so there is nothing in the file
to patch by hand.  What comes out is a disk image the co-simulation boots on
the Verilated chip.  See cosim/README.md.

    run-tme.py CONFIG -s 'PATTERN:=TEXT' ... -m 'PATTERN:=TMESH COMMAND' ...

There are two channels and this multiplexes them: tmesh talks on its own
terminal, and the emulated machine's ttya is a pseudo-terminal.  Rules fire in
the order given, once each; `-s` types at the machine, `-m` at tmesh, which is
how a tape is swapped without stopping the emulator.

The config is read with `@CONSOLE@` replaced by the console's device path.
TME 0.8 has no /dev/ptmx support - there is no ptsname() call anywhere in it,
whatever its example configuration says - so the pty is ours to make.

Three carats are a break, which is the only way to interrupt a PROM that has
started to auto-boot.
"""
import argparse
import os
import pty
import re
import select
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TME = os.environ.get('TME', os.path.join(ROOT, 'work', 'tme'))

ECHO_WAIT = 8          # seconds to wait for a typed line to come back


def rules(specs):
    out = []
    for s in specs:
        if ':=' not in s:
            sys.exit('want PATTERN:=TEXT, not %r' % s)
        pat, text = s.split(':=', 1)
        out.append((re.compile(pat.encode()), text))
    return out


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('config', help='a tmesh configuration using @CONSOLE@')
    ap.add_argument('-s', '--send', action='append', default=[],
                    metavar='PATTERN:=TEXT', help='type TEXT at the machine')
    ap.add_argument('-m', '--tmesh', action='append', default=[],
                    metavar='PATTERN:=CMD', help='type CMD at tmesh')
    ap.add_argument('-t', '--timeout', type=float, default=1800)
    ap.add_argument('-i', '--idle', type=float, default=180,
                    help='give up after this long with a silent console')
    ap.add_argument('--tme', default=TME, help='where TME is installed')
    args = ap.parse_args()

    tmesh = os.path.join(args.tme, 'bin', 'tmesh')
    if not os.path.exists(tmesh):
        sys.exit(f'TME missing: {tmesh}')
    env = dict(os.environ)
    env['LTDL_LIBRARY_PATH'] = os.path.join(args.tme, 'lib')

    cmaster, cslave = pty.openpty()
    slave_path = os.ttyname(cslave)
    generated = args.config + '.run'
    with open(generated, 'w') as f:
        f.write(open(args.config).read().replace('@CONSOLE@', slave_path))
    print('[drive] machine console on %s (config %s)' % (slave_path, generated))

    pid, tfd = pty.fork()
    if pid == 0:
        # tmesh logs to /dev/null unless told otherwise, which hides every
        # error the configuration makes.
        os.execve(tmesh, [tmesh, '--log', '-', generated], env)
        os._exit(1)

    console = cmaster
    tlog = clog = b''
    sendq, meshq = rules(args.send), rules(args.tmesh)
    cscan = mscan = 0
    echo, echo_by = None, 0.0
    deadline = time.time() + args.timeout
    last = time.time()

    try:
        while time.time() < deadline:
            r, _, _ = select.select([tfd, console], [], [], 0.5)
            for fd in r:
                try:
                    chunk = os.read(fd, 4096)
                except OSError:
                    chunk = b''
                if not chunk:
                    continue
                last = time.time()
                if fd == tfd:
                    tlog += chunk
                    sys.stdout.write('[tme] ' + chunk.decode('utf-8', 'replace'))
                else:
                    clog += chunk
                    sys.stdout.write(chunk.decode('utf-8', 'replace'))
                sys.stdout.flush()

            # A line typed at a shell comes straight back as an echo, so a
            # marker inside the command - "echo DONE" - reaches the console
            # before the command has run.  Matching that fires every remaining
            # rule at once and types the whole script into a shell still busy
            # with its second line.  So wait for the echo first; and give up
            # waiting after a while, because not everything is echoed - a
            # break is three carats the PROM never shows.
            if echo is not None:
                at = clog.find(echo, cscan)
                if at >= 0:
                    cscan, echo = at + len(echo), None
                elif time.time() > echo_by:
                    echo = None
            elif sendq:
                pat, text = sendq[0]
                if pat.search(clog, cscan):
                    time.sleep(0.4)
                    os.write(console, text.encode() + b'\r')
                    print('\n[drive] sent %r' % text)
                    cscan = len(clog)
                    echo = text.encode() if text else None
                    echo_by = time.time() + ECHO_WAIT
                    sendq.pop(0)

            if meshq:
                pat, cmd = meshq[0]
                if pat.search(clog, mscan) or pat.search(tlog):
                    time.sleep(0.4)
                    os.write(tfd, cmd.encode() + b'\r')
                    print('\n[drive] tmesh %r' % cmd)
                    mscan = len(clog)
                    meshq.pop(0)

            if time.time() - last > args.idle:
                print('\n[drive] idle for %gs, stopping' % args.idle)
                break
    finally:
        os.close(tfd)
        try:
            os.kill(pid, 9)
            os.waitpid(pid, 0)
        except OSError:
            pass

    for pat, _ in sendq:
        print('[drive] NEVER PROMPTED: %s' % pat.pattern.decode())
    return 1 if sendq else 0


if __name__ == '__main__':
    sys.exit(main())
