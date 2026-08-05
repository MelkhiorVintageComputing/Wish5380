#!/usr/bin/env python3
"""Build an Atari TT hard disk for the emulated SCSI chip.

The disk is an AHDI partition table in sector 0 and one FAT16 partition behind
it, which is what EmuTOS looks for and mounts as C:.  It is assembled here
rather than formatted inside the machine because EmuTOS has no formatter, and
because mkdosfs is not a dependency worth having for four hundred bytes of
boot sector.

What EmuTOS does with it, in the order the SCSI trace shows:

  sector 0          the AHDI root sector - four partition entries and a
                    checksum, all big-endian
  partition start   the FAT16 boot sector, whose BPB is little-endian because
                    Atari filesystems are DOS-compatible on purpose
  then              two FATs, a fixed-size root directory, and the clusters

The two byte orders in the same disk are not a mistake: the partition table is
Atari's own and the filesystem is IBM's, and each kept the convention of
whoever designed it.
"""
import os
import struct
import sys

SECTOR = 512

# The AHDI root sector.  Offsets from the Atari HD driver documentation; the
# partition table sits where a PC's does but counts in big-endian.
AHDI_SIZE_OFF = 0x1C2                                   # total sectors
AHDI_PART_OFF = 0x1C6                                   # four 12-byte entries
AHDI_BSL_OFF = 0x1F6                                    # bad sector list
AHDI_SUM_OFF = 0x1FE                                    # checksum word
AHDI_SUM_TARGET = 0x1234

# Partition type.  GEM is the original and stops at 16 MB; BGM is the "big GEM"
# that came with AHDI 3 and is what anything larger has to use.
GEM_MAX_SECTORS = 32768


def ahdi_root_sector(total_sectors, part_start, part_sectors):
    """The root sector: one partition, marked existing and bootable."""
    sec = bytearray(SECTOR)
    struct.pack_into('>I', sec, AHDI_SIZE_OFF, total_sectors)

    ident = b'BGM' if part_sectors > GEM_MAX_SECTORS else b'GEM'
    # Bit 0 says the entry is used, bit 7 says it is the one to boot from.
    struct.pack_into('>B3sII', sec, AHDI_PART_OFF,
                     0x81, ident, part_start, part_sectors)
    struct.pack_into('>II', sec, AHDI_BSL_OFF, 0, 0)

    # The checksum word is chosen so every big-endian word in the sector sums
    # to 0x1234.  A driver that finds any other total treats the disk as
    # unpartitioned, so this is what makes the difference between a disk and a
    # pile of blocks.
    words = struct.unpack('>256H', bytes(sec))
    struct.pack_into('>H', sec, AHDI_SUM_OFF,
                     (AHDI_SUM_TARGET - sum(words)) & 0xFFFF)
    return bytes(sec)


class Fat16:
    """A FAT16 filesystem, built whole in memory.

    Only what a fresh partition needs: a boot sector, two identical FATs, a
    fixed-size root directory and the data area.  There is no free-space
    search worth the name because nothing is ever deleted here - clusters are
    handed out in order as files are added.
    """

    def __init__(self, sectors, label=b'WISH5380  ', spc=2, root_entries=512):
        self.sectors = sectors
        self.label = label.ljust(11)[:11]
        self.spc = spc
        self.root_entries = root_entries
        self.root_sectors = (root_entries * 32 + SECTOR - 1) // SECTOR

        # Sectors per FAT and the cluster count define each other, so settle
        # them by iteration rather than by an algebraic rearrangement that
        # would be off by one in exactly one direction.
        self.spf = 1
        while True:
            data = sectors - 1 - 2 * self.spf - self.root_sectors
            clusters = data // spc
            need = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
            if need <= self.spf:
                break
            self.spf = need
        self.clusters = clusters
        if not 4085 <= self.clusters <= 65524:
            raise ValueError('%d clusters is not a FAT16' % self.clusters)

        self.fat = [0] * (self.clusters + 2)
        self.fat[0], self.fat[1] = 0xFFF8, 0xFFFF
        self.next_free = 2
        self.root = bytearray(self.root_sectors * SECTOR)
        self.dirs = {None: [self.root, 0, self.root_entries]}
        self.data = bytearray(self.clusters * spc * SECTOR)

    def _alloc(self, content):
        """Hand out consecutive clusters and chain them.  Returns the first."""
        need = (len(content) + self.spc * SECTOR - 1) // (self.spc * SECTOR)
        if self.next_free + need > self.clusters + 2:
            raise ValueError('%d bytes do not fit' % len(content))
        first = self.next_free
        for i in range(need):
            cluster = first + i
            off = (cluster - 2) * self.spc * SECTOR
            self.data[off:off + self.spc * SECTOR] = \
                content[i * self.spc * SECTOR:(i + 1) * self.spc * SECTOR] \
                .ljust(self.spc * SECTOR, b'\0')
            self.fat[cluster] = 0xFFFF if i == need - 1 else cluster + 1
        self.next_free += need
        return first

    def _entry(self, where, name, ext, attr, first, size):
        buf, used, limit = self.dirs[where][:3]
        if used >= limit:
            raise ValueError('directory is full')
        struct.pack_into('<8s3sB10sHHHI', buf, used * 32,
                         name.ljust(8)[:8], ext.ljust(3)[:3],
                         attr,
                         b'\0' * 10,
                         0x6000, 0x5865,                # a fixed time and date
                         first, size)
        self.dirs[where][1] = used + 1

    def mkdir(self, name, where=None):
        """One subdirectory, one cluster - room for enough entries here."""
        cluster = self._alloc(b'\0' * (self.spc * SECTOR))
        buf = bytearray(self.spc * SECTOR)
        limit = self.spc * SECTOR // 32
        self.dirs[name] = [buf, 0, limit]
        # A subdirectory begins with its own two links.  ".." points at
        # cluster 0 when the parent is the root, because the root has no
        # cluster of its own to point at.
        self._entry(name, b'.', b'', 0x10, cluster, 0)
        self._entry(name, b'..', b'', 0x10, 0, 0)
        self._entry(where, name.encode() if isinstance(name, str) else name,
                    b'', 0x10, cluster, 0)
        self.dirs[name].append(cluster)
        return name

    def add(self, name, ext, content, where=None):
        """Write one file into the root directory or into a subdirectory."""
        first = self._alloc(content) if content else 0
        self._entry(where, name, ext, 0x20, first, len(content))

    def image(self):
        # Subdirectories were given their cluster when they were created and
        # filled in afterwards, so their contents go in last.
        for name, entry in self.dirs.items():
            if name is None:
                continue
            buf, _, _, cluster = entry
            off = (cluster - 2) * self.spc * SECTOR
            self.data[off:off + len(buf)] = buf

        boot = bytearray(SECTOR)
        # A branch that jumps over the BPB.  Nothing here is ever executed -
        # the TT boots off the AHDI entry, not off this - but a boot sector
        # whose first byte is zero is one some drivers refuse.
        boot[0:3] = b'\xEB\x3C\x90'
        struct.pack_into('<8sHBHBHHBHHHII', boot, 3,
                         b'WISH5380',
                         SECTOR, self.spc, 1, 2, self.root_entries,
                         self.sectors if self.sectors < 0x10000 else 0,
                         0xF8, self.spf,
                         32, 16,                        # a plausible geometry
                         0,                             # hidden sectors
                         self.sectors if self.sectors >= 0x10000 else 0)
        struct.pack_into('<BBB I 11s 8s', boot, 0x24,
                         0x80, 0, 0x29, 0x57495348, self.label, b'FAT16   ')
        boot[0x1FE:0x200] = b'\x55\xAA'

        fat = bytearray(self.spf * SECTOR)
        struct.pack_into('<%dH' % len(self.fat), fat, 0, *self.fat)

        img = bytes(boot) + bytes(fat) * 2 + bytes(self.root) + bytes(self.data)
        return img.ljust(self.sectors * SECTOR, b'\0')


class M68k:
    """Just enough of an assembler to emit one program.

    A cross-assembler is a heavy dependency for forty words, and a table of
    hand-computed offsets is a thing that silently rots.  This is the middle:
    words go in a list, labels remember where they were, and the PC-relative
    displacements are filled in at the end when every address is known.
    """

    def __init__(self):
        self.words = []
        self.labels = {}
        self.fixups = []

    def w(self, *words):
        self.words.extend(words)

    def label(self, name):
        self.labels[name] = len(self.words) * 2

    def pea_pc(self, name):
        """pea name(pc) - the displacement is from the extension word."""
        self.w(0x487A)
        self.fixups.append((len(self.words), name))
        self.w(0)

    def bytes(self, name, data):
        self.label(name)
        if len(data) & 1:
            data += b'\0'
        self.words.extend(struct.unpack('>%dH' % (len(data) // 2), data))

    def code(self):
        out = list(self.words)
        for at, name in self.fixups:
            out[at] = (self.labels[name] - at * 2) & 0xFFFF
        return struct.pack('>%dH' % len(out), *out)


def greeter(message):
    """A GEMDOS .PRG that says its piece, writes a file, and exits.

    EmuTOS finds this in C:\\AUTO, loads it off the chip and jumps into it -
    a filesystem read, a program load and an execution that all had to go
    through the replica to happen at all.  Then it writes a file, which is
    the same journey in the other direction and over the DMA controller's
    send path rather than its receive path.  The two are not one piece of
    logic and a lineage that only ever read would not know it.

    Everything is PC-relative, so the header can say there is nothing to
    relocate and no relocation table has to be built.
    """
    a = M68k()

    a.pea_pc('msg')                                 # Cconws(msg)
    a.w(0x3F3C, 0x0009, 0x4E41, 0x5C8F)

    a.w(0x4267)                                     # Fcreate(name, 0)
    a.pea_pc('name')
    a.w(0x3F3C, 0x003C, 0x4E41, 0x4FEF, 0x0008)
    a.w(0x3E00)                                     # keep the handle in d7

    a.pea_pc('data')                                # Fwrite(d7, len, data)
    a.w(0x2F3C, 0, 0, 0x3F07, 0x3F3C, 0x0040, 0x4E41, 0x4FEF, 0x000C)
    count = len(a.words) - 8                        # where that long landed

    a.w(0x3F07, 0x3F3C, 0x003E, 0x4E41, 0x588F)     # Fclose(d7)

    a.pea_pc('done')                                # Cconws(done)
    a.w(0x3F3C, 0x0009, 0x4E41, 0x5C8F)

    a.w(0x4267, 0x4E41)                             # Pterm0

    written = message.encode() + b'\r\n'
    a.bytes('msg', message.encode() + b'\r\n\0')
    a.bytes('name', b'C:\\WROTE.TXT\0')
    a.bytes('data', written)
    a.bytes('done', b'and it wrote that back to C:\\WROTE.TXT\r\n\0')

    a.words[count] = len(written) >> 16
    a.words[count + 1] = len(written) & 0xFFFF

    code = a.code()
    header = struct.pack('>HIIIIIIH',
                         0x601A,                    # branch, the PRG magic
                         len(code), 0, 0,           # text, data, bss
                         0,                         # no symbols
                         0, 0,                      # reserved, flags
                         1)                         # absflag: no relocations
    return header + code


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('image')
    ap.add_argument('--size-mb', type=int, default=32,
                    help='disk size in megabytes (default 32)')
    ap.add_argument('--label', default='WISH5380')
    ap.add_argument('--add', action='append', default=[], metavar='NAME=PATH',
                    help='put a file in the root directory, 8.3 name')
    ap.add_argument('--auto', metavar='TEXT',
                    help='put a program in C:\\AUTO that prints TEXT at boot')
    args = ap.parse_args()

    total = args.size_mb * 1024 * 1024 // SECTOR
    # The partition starts at sector 2 rather than 1 because AHDI's own
    # convention leaves the sector after the root sector alone.
    start = 2
    fs = Fat16(total - start, label=args.label.encode())

    for spec in args.add:
        name, _, path = spec.partition('=')
        with open(path, 'rb') as f:
            content = f.read()
        base, _, ext = name.partition('.')
        fs.add(base.upper().encode(), ext.upper().encode(), content)

    if args.auto:
        auto = fs.mkdir('AUTO')
        fs.add(b'HELLO', b'PRG', greeter(args.auto), where=auto)

    with open(args.image, 'wb') as f:
        f.write(ahdi_root_sector(total, start, total - start))
        f.write(b'\0' * SECTOR * (start - 1))
        f.write(fs.image())

    print('%s: %d MB, one %s partition of %d clusters, %d root entries'
          % (args.image, args.size_mb,
             'BGM' if total - start > GEM_MAX_SECTORS else 'GEM',
             fs.clusters, fs.dirs[None][1]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
