#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Build a small, clean SunOS 4.1.1 disk for the Sun-3 co-simulation, under
# TME, from the install tape.
#
# The disk is a Quantum ProDrive 80S - 832 data cylinders, 2 alternates, 6
# heads, 34 sectors, so 204 sectors per cylinder and 169728 usable blocks,
# which is 82.9 MB.  That geometry is the one /etc/format.dat gives for the
# drive.  Its *sample partition table* is not used, because that block assumes
# 256 sectors per cylinder and does not match the disk_type printed above it
# in the same file; the partitions below are computed from the real geometry.
#
#   a  cyl   0, 672 cyl = 137088 blocks = 66.9 MB   /
#   b  cyl 672, 160 cyl =  32640 blocks = 15.9 MB   swap, and the miniroot
#   c  cyl   0, 832 cyl = 169728 blocks = 82.9 MB   whole disk, no filesystem
#
# Small on purpose.  newfs scales the inode count with the size of the
# filesystem and fsck's cost is mostly the inode scan, so a 62 MB filesystem
# carries about 29000 free inodes where a 200 MB one carried about 100000 -
# for the same 30 MB of content.
#
# ---------------------------------------------------------------------------
# Everything is in one filesystem, and that is forced by the media.
#
# The sun3 proto root on this tape is a *diskless client's* root: its stock
# /etc/fstab is "server:/rootdir / nfs", its /sbin is an empty directory, and
# /etc/mount, /etc/fsck, /etc/umount and /bin are all symlinks into /usr.
# Nothing on it can run before /usr is mounted.  A diskless kernel mounts /usr
# over NFS from bootparams before it execs init; a local disk has no such
# mechanism, so a split /usr install dies in "panic: icode" having never
# mounted anything.  Seeding /sbin from the miniroot does not rescue it
# either - every miniroot binary is dynamically linked against /usr/lib/ld.so.
#
# So there is no /usr partition and no /usr line in fstab.  What there is
# instead is a one-line change to /etc/rc.single, whose "mount -o remount
# /usr" fails on a machine with no separate /usr and answers `exit 2`, which
# /etc/rc.boot turns into its own exit status (line 122) and init reads as a
# failed single-user setup.  Saying so and carrying on is honest.  The
# alternative, an fstab entry pointing /usr at the root's own device, is a
# fiction, and it also makes fsck check sd0a twice under two names.
#
# ---------------------------------------------------------------------------
# fsck is switched off in fstab, by the pass field rather than by /fastboot.
#
# `fsck -p` checks only entries whose sixth field is non-zero, so a 0 makes
# "checking filesystems" return at once.  That matters because fsck is where
# the co-simulation stopped: about 2700 s to walk the inodes at the ~12 KB/s
# the Verilated chip sustains, and the "sd0: I/O request timeout" fault fires
# during it.  /fastboot would also work but /etc/rc removes it after one boot
# (line 35), and a clean shutdown buys nothing at all - 4.1.1's fsck is
# 4.3BSD-derived and has no clean-flag logic, so it runs all five phases
# however the filesystem was unmounted.
#
# Skipping the check is safe here because the image is built from the tape by
# this script and restored from a pristine copy, not maintained in place.
#
# ---------------------------------------------------------------------------
# Needs the TME machine directory that cosim/README.md describes, holding
# tmesh's configuration, the PROM and idprom images, and the tape sets.  TME
# resolves the paths in its configuration relative to its own directory, so
# everything runs from there.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$(dirname "$HERE")")"
RUNDIR="${RUNDIR:-$ROOT/work/tme-run}"
BASECFG="${BASECFG:-SUN3-emulex}"
IMG="${IMG:-sun3-80s.img}"
CFG="${CFG:-SUN3-80s}"
PRISTINE="${PRISTINE:-$ROOT/work/sun3/sunos-80s-pristine.img}"
TME="${TME:-$ROOT/work/tme}"
DRIVE="$HERE/run-tme.py"

cd "$RUNDIR"
[ -f "$BASECFG" ] || { echo "no $BASECFG in $RUNDIR - see cosim/README.md" >&2; exit 1; }

say() { printf '\n== %s\n' "$*"; }

# 834 physical cylinders x 6 heads x 34 sectors x 512 bytes.
say "creating $IMG"
rm -f "$IMG"
truncate -s $((834 * 6 * 34 * 512)) "$IMG"
sed -e "s|file [A-Za-z0-9._-]*\.img|file $IMG|" "$BASECFG" > "$CFG"

# The tape boot is intermittent under TME's cooperative threads, so retry.
# Nothing touches the disk until "label".  This stage also plants the miniroot
# in partition b, which is the installer's own next step once it has a label.
say "stage 1: geometry, partitions and label"
for i in 1 2 3 4 5 6 7 8; do
  python3 "$DRIVE" "$CFG" --tme "$TME" -t 1100 -i 240 \
    -s '>:=b st()' \
    -s '2 - exit to single user shell:=1' \
    -s 'run format:=1' \
    -s 'format>:=disk' \
    -s 'Specify disk .enter its number:=0' \
    -s 'Specify disk type:=5' \
    -s 'number of data cylinders:=832' \
    -s 'alternate cylinders:=' \
    -s 'physical cylinders:=' \
    -s 'number of heads:=6' \
    -s 'data sectors/track:=34' \
    -s 'rpm of drive:=' \
    -s 'buffer skew:=' \
    -s 'write precomp:=' \
    -s 'disk type name:="ProDrive80S"' \
    -s 'format>:=partition' \
    -s 'partition>:=a' -s 'starting cyl:=0'   -s 'blocks:=137088' \
    -s 'partition>:=b' -s 'starting cyl:=672' -s 'blocks:=32640' \
    -s 'partition>:=c' -s 'starting cyl:=0'   -s 'blocks:=169728' \
    -s 'partition>:=g' -s 'starting cyl:=0'   -s 'blocks:=0' \
    -s 'partition>:=name' -s 'table name:="prodrive80s"' \
    -s 'partition>:=label' -s 'continue.:=y' \
    -s 'partition>:=quit' \
    -s 'format>:=quit' \
    > "label-$i.log" 2>&1 || true
  if grep -aq 'Mini-root installation complete' "label-$i.log"; then
    echo "labelled and miniroot planted (attempt $i)"; break
  fi
  echo "attempt $i: tape did not boot"
  [ "$i" = 8 ] && { echo "giving up - see label-*.log" >&2; exit 1; }
done

# The sets go on with tar: suninstall is a full-screen form program and this
# is a serial line.  Each set rewinds and seeks to its own tape file rather
# than trusting where the last one left the tape - zcat stops when the
# compressed stream ends, not at the filemark, so reading straight on gets
# "stdin: not in compressed format" and half a filesystem.
#
# Tape file order: 0 tpboot, 1 toc, 2 munix, 3 munixfs, 4 miniroot,
# 5 proto root, 6 usr, 7 kvm.
say "stage 2: the sets, the boot block, fstab and rc.single"
python3 "$DRIVE" "$CFG" --tme "$TME" -t 8800 -i 1200 \
  -s '>:=b sd(0,0,1) -sw' \
  -s '# :=umount /a/usr; umount /a; cp /dev/null /etc/mtab; echo cleared' \
  -s '# :=newfs /dev/rsd0a' \
  -s '# :=mount /dev/sd0a /a; echo mounted' \
  -s '# :=mt -f /dev/nrst0 rewind; mt -f /dev/nrst0 fsf 5' \
  -s '# :=cd /a; /usr/ucb/zcat < /dev/nrst0 | tar xpf -' \
  -s '# :=mt -f /dev/nrst0 rewind; mt -f /dev/nrst0 fsf 6' \
  -s '# :=cd /a/usr; /usr/ucb/zcat < /dev/nrst0 | tar xpf -' \
  -s '# :=mt -f /dev/nrst0 rewind; mt -f /dev/nrst0 fsf 7' \
  -s '# :=mkdir -p /a/usr/kvm; cd /a/usr/kvm; /usr/ucb/zcat < /dev/nrst0 | tar xpf -' \
  -s '# :=cp /a/usr/kvm/stand/vmunix /a/vmunix; cp /boot.sun3 /a/boot; ls -l /a/vmunix /a/boot' \
  -s '# :=echo "/dev/sd0a / 4.2 rw 1 0" > /a/etc/fstab' \
  -s '# :=echo "/dev/sd0b swap swap rw 0 0" >> /a/etc/fstab' \
  -s '# :=cat /a/etc/fstab' \
  -s '# :=sed -e "s/exit 2 ; fi/echo no separate usr ; fi/" /a/etc/rc.single > /tmp/rs; cp /tmp/rs /a/etc/rc.single; grep -n "no separate usr" /a/etc/rc.single' \
  -s '# :=/a/usr/kvm/mdec/installboot -vlh /a/boot /a/usr/kvm/mdec/bootsd /dev/rsd0a' \
  -s '# :=sync; sync; echo written' \
  -s '# :=cd /; umount /a; sync; echo unmounted' \
  > install.log 2>&1 || true
grep -aq 'unmounted' install.log || { echo "install did not finish - see install.log" >&2; exit 1; }

# The proto root ships /dev/MAKEDEV and no device nodes; suninstall would have
# run it.  Without them init starts, cannot open a console, and says nothing
# at all - which looks exactly like a hang, and cost an afternoon once.
# SunOS's tar refuses to archive special files, so the miniroot's own /dev
# cannot simply be copied across.
say "stage 3: device nodes"
python3 "$DRIVE" "$CFG" --tme "$TME" -t 2200 -i 600 \
  -s 'Auto-boot in progress:=^^^' \
  -s '>:=b sd(0,0,1) -sw' \
  -s '# :=umount /a; cp /dev/null /etc/mtab; mount /dev/sd0a /a; echo mounted' \
  -s '# :=cd /a/dev; sh MAKEDEV std sd0 st0 pty0; echo devsdone' \
  -s '# :=ls -l /a/dev/console /a/dev/sd0a /a/dev/tty' \
  -s '# :=cd /; sync; sync; umount /a; echo unmounted' \
  > devs.log 2>&1 || true
grep -aq 'devsdone' devs.log || { echo "MAKEDEV did not run - see devs.log" >&2; exit 1; }

mkdir -p "$(dirname "$PRISTINE")"
cp "$IMG" "$PRISTINE"
say "done"
echo "  image    $RUNDIR/$IMG"
echo "  pristine $PRISTINE"
echo
echo "Boot it with:"
echo "  cosim/scripts/run-sun3.py --image $RUNDIR/$IMG -t 14000 -i 5000 'b sd()'"
echo
echo "run-sun3.py writes to that image's -run copy rather than to the image"
echo "itself, so every run starts from the same disk.  The pristine copy is"
echo "the second line of defence, for whatever writes to the original anyway."
