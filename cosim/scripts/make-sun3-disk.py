#!/usr/bin/env python3
"""Build a bootable NetBSD/sun3 disk for the emulated SCSI card.

The disk is NetBSD's own sun3 miniroot with a Sun disk label in front of it and
NetBSD's own first-stage boot program in the fifteen blocks after that.  It is
assembled here rather than installed inside the machine because installing it
inside the machine needs a machine that already boots.

What the 3/60 PROM does with it:

  block 0        the Sun label - geometry and the partition table
  blocks 1-15    bootxx, which the PROM loads whole and jumps into
  block 32 on    the filesystem, holding /ufsboot and /netbsd.sun3

bootxx is not a filesystem reader.  It carries a list of raw block numbers,
patched in by installboot(8), and copies those blocks into memory and jumps to
them; that is how the second stage gets loaded before anything understands
FFS.  We do the patching ourselves - the block list comes from reading the
miniroot's own inode for /ufsboot, which is what installboot would have asked
the running kernel for.

Nothing is written into the filesystem, so no FFS writer is needed: the label
and bootxx go in the sixteen kilobytes the filesystem reserves ahead of its own
superblock, which is exactly what that space is for.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from ffs import Ffs                                     # noqa: E402

SUN_DKMAGIC = 0xDABE
BBINFO_MAGIC = b'NetBSD/sun68k bootxx   20020515\0'
BOOT_BLOCK_OFFSET = 512
BOOT_BLOCK_MAX_SIZE = 512 * 15
SECTOR = 512

# struct sun_disklabel, from sys/dev/sun/disklabel.h.
SL_RPM, SL_PCYL, SL_SPARES = 420, 422, 424
SL_INTERLEAVE, SL_NCYL, SL_ACYL = 430, 432, 434
SL_NTRACKS, SL_NSECTORS = 436, 438
SL_PART, SL_MAGIC, SL_CKSUM = 444, 508, 510


def sun_label(text, ncyl, ntracks, nsectors, parts):
    """parts: {index: (cyloffset, nsectors)}."""
    lbl = bytearray(SECTOR)
    t = text.encode('ascii')[:127]
    lbl[0:len(t)] = t
    struct.pack_into('>H', lbl, SL_RPM, 3600)
    struct.pack_into('>H', lbl, SL_PCYL, ncyl)
    struct.pack_into('>H', lbl, SL_SPARES, 0)
    struct.pack_into('>H', lbl, SL_INTERLEAVE, 1)
    struct.pack_into('>H', lbl, SL_NCYL, ncyl)
    struct.pack_into('>H', lbl, SL_ACYL, 0)
    struct.pack_into('>H', lbl, SL_NTRACKS, ntracks)
    struct.pack_into('>H', lbl, SL_NSECTORS, nsectors)
    for i, (cyl, n) in parts.items():
        struct.pack_into('>ii', lbl, SL_PART + i * 8, cyl, n)
    struct.pack_into('>H', lbl, SL_MAGIC, SUN_DKMAGIC)

    # The checksum is defined so that the exclusive-or of every short in the
    # block is zero, which is why it is computed over the magic as well.
    x = 0
    for off in range(0, SECTOR, 2):
        x ^= struct.unpack_from('>H', lbl, off)[0]
    struct.pack_into('>H', lbl, SL_CKSUM, x)
    return bytes(lbl)


def install_boot(bootxx, block_size, blocks):
    """Patch the second stage's block list into a copy of bootxx."""
    bb = bytearray(bootxx)
    at = bb.find(BBINFO_MAGIC)
    if at < 0:
        raise SystemExit('bootxx: bbinfo magic not found - wrong file?')
    struct.pack_into('>ii', bb, at + 32, block_size, len(blocks))
    for i, blk in enumerate(blocks):
        struct.pack_into('>i', bb, at + 40 + i * 4, blk)
    if len(bb) > BOOT_BLOCK_MAX_SIZE:
        raise SystemExit('bootxx is %d bytes, the PROM loads only %d'
                         % (len(bb), BOOT_BLOCK_MAX_SIZE))
    return bytes(bb)


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--miniroot', required=True, help='miniroot.fs, unpacked')
    ap.add_argument('--bootxx', required=True, help='usr/mdec/bootxx')
    ap.add_argument('--stage2', default='/ufsboot',
                    help='second stage, by path inside the miniroot')
    ap.add_argument('--out', required=True)
    ap.add_argument('--size-mb', type=int, default=16,
                    help='whole disk; the filesystem keeps its own size')
    args = ap.parse_args()

    fsdata = open(args.miniroot, 'rb').read()
    bootxx = open(args.bootxx, 'rb').read()
    fs = Ffs(fsdata)

    ino = fs.lookup(args.stage2)
    if ino is None:
        raise SystemExit('%s not in the miniroot' % args.stage2)
    inode = fs.inode(ino)
    blocks = fs.blocks(inode)

    # fs_fsbtodb converts a filesystem block number to a 512-byte disk block
    # number, and it is what bootxx will hand to the PROM.
    dblocks = [b << fs.fsbtodb for b in blocks]

    # A geometry that divides the disk exactly.  Nothing reads a real head or
    # sector here, but the PROM and the label both insist the numbers multiply
    # out, and a partition is measured in cylinders.
    total = args.size_mb * 1024 * 1024 // SECTOR
    ntracks, nsectors = 16, 32
    percyl = ntracks * nsectors
    ncyl = total // percyl
    total = ncyl * percyl
    fssec = len(fsdata) // SECTOR
    if fssec > total:
        raise SystemExit('the filesystem is bigger than the disk')
    acyl = (fssec + percyl - 1) // percyl

    label = sun_label('Wish5380 NetBSD/sun3 miniroot', ncyl, ntracks, nsectors,
                      {0: (0, acyl * percyl),      # a: the miniroot
                       2: (0, total)})             # c: the whole disk

    img = bytearray(total * SECTOR)
    img[0:len(fsdata)] = fsdata
    img[0:SECTOR] = label
    boot = install_boot(bootxx, fs.bsize, dblocks)
    img[BOOT_BLOCK_OFFSET:BOOT_BLOCK_OFFSET + len(boot)] = boot

    # The filesystem's own superblock must have survived all of that.
    Ffs(bytes(img))

    with open(args.out, 'wb') as f:
        f.write(img)

    print('%s: %d MB, %d cylinders of %d x %d' %
          (args.out, total * SECTOR // (1024 * 1024), ncyl, ntracks, nsectors))
    print('  a: %d sectors (the miniroot), c: %d sectors' %
          (acyl * percyl, total))
    print('  bootxx: %d bytes at block 1' % len(boot))
    print('  %s: %d bytes, blocks %s of %d each' %
          (args.stage2, inode['size'], dblocks, fs.bsize))


if __name__ == '__main__':
    main()
