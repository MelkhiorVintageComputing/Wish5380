#!/usr/bin/env python3
"""Read a big-endian UFS1 (FFS) image: list directories, extract files.

Enough of the filesystem to look inside NetBSD's sun3 miniroot and to find out
where a file's blocks are, which is what building a bootable disk needs.  It
does not write, and it does not need to: the second-stage boot program is
placed outside the filesystem, in space the label does not give to it.

Field offsets come from NetBSD's sys/ufs/ffs/fs.h, sys/ufs/ufs/dinode.h and
sys/ufs/ufs/dir.h.  Everything is big-endian because the machine is.
"""
import struct
import sys

FS_UFS1_MAGIC = 0x011954
SBLOCK_UFS1 = 8192
DINODE1_SIZE = 128
UFS_ROOTINO = 2
NDADDR, NIADDR = 12, 3

# struct fs, by byte offset from the start of the superblock.
SB = {
    'sblkno': 8, 'cblkno': 12, 'iblkno': 16, 'dblkno': 20,
    'old_cgoffset': 24, 'old_cgmask': 28,
    'old_size': 36, 'ncg': 44, 'bsize': 48, 'fsize': 52, 'frag': 56,
    'bshift': 80, 'fshift': 84, 'fragshift': 96, 'fsbtodb': 100,
    'nindir': 116, 'inopb': 120, 'ipg': 184, 'fpg': 188,
    'magic': 1372,
}

# struct ufs1_dinode.
DI_MODE, DI_SIZE, DI_DB, DI_IB = 0, 8, 40, 88

IFMT, IFDIR, IFREG, IFLNK = 0o170000, 0o040000, 0o100000, 0o120000


class Ffs(object):
    def __init__(self, data, offset=0):
        self.d = data
        self.o = offset          # where the partition starts in `data`
        sb = offset + SBLOCK_UFS1
        for k, v in SB.items():
            setattr(self, k, struct.unpack_from('>i', data, sb + v)[0])
        if self.magic != FS_UFS1_MAGIC:
            raise ValueError('not a big-endian UFS1 filesystem '
                             '(magic 0x%08x)' % (self.magic & 0xffffffff))

    # A "frag address" is the filesystem's own block number; a fragment is
    # fs_fsize bytes and that is what everything is counted in.
    def _frag(self, fsb, n=1):
        at = self.o + fsb * self.fsize
        return self.d[at:at + n * self.fsize]

    def _cgstart(self, c):
        return self.fpg * c + self.old_cgoffset * (c & ~self.old_cgmask)

    def inode(self, ino):
        cg = ino // self.ipg
        base = self._cgstart(cg) + self.iblkno
        base += ((ino % self.ipg) // self.inopb) << self.fragshift
        at = self.o + base * self.fsize + (ino % self.inopb) * DINODE1_SIZE
        raw = self.d[at:at + DINODE1_SIZE]
        mode, = struct.unpack_from('>H', raw, DI_MODE)
        size, = struct.unpack_from('>Q', raw, DI_SIZE)
        db = list(struct.unpack_from('>%di' % NDADDR, raw, DI_DB))
        ib = list(struct.unpack_from('>%di' % NIADDR, raw, DI_IB))
        return {'mode': mode, 'size': size, 'db': db, 'ib': ib}

    def _indirect(self, fsb, level, want):
        """Logical-to-physical for the indirect trees, appending to `want`."""
        if fsb == 0 or len(want) >= self.nblocks:
            return
        blk = self._frag(fsb, self.frag)
        for i in range(self.nindir):
            if len(want) >= self.nblocks:
                return
            nxt, = struct.unpack_from('>i', blk, i * 4)
            if level == 1:
                want.append(nxt)
            else:
                self._indirect(nxt, level - 1, want)

    def blocks(self, ino):
        """The file's blocks, in order, as filesystem block numbers."""
        inode = self.inode(ino) if isinstance(ino, int) else ino
        bsize = self.bsize
        self.nblocks = (inode['size'] + bsize - 1) // bsize
        out = [b for b in inode['db'][:self.nblocks]]
        for level in (1, 2, 3):
            if len(out) >= self.nblocks:
                break
            self._indirect(inode['ib'][level - 1], level, out)
        return out[:self.nblocks]

    def read(self, ino):
        inode = self.inode(ino) if isinstance(ino, int) else ino
        if (inode['mode'] & IFMT) == IFLNK and inode['size'] < 60:
            raw = struct.pack('>%di' % NDADDR, *inode['db'])
            raw += struct.pack('>%di' % NIADDR, *inode['ib'])
            return raw[:inode['size']]
        out = bytearray()
        for b in self.blocks(inode):
            out += self._frag(b, self.frag)
        return bytes(out[:inode['size']])

    def readdir(self, ino):
        data = self.read(ino)
        at, out = 0, []
        while at + 8 <= len(data):
            fileno, reclen, dtype, namlen = struct.unpack_from('>IHBB', data, at)
            if reclen < 8 or at + reclen > len(data):
                break
            if fileno:
                name = data[at + 8:at + 8 + namlen].decode('latin-1')
                out.append((name, fileno, dtype))
            at += reclen
        return out

    def lookup(self, path):
        ino = UFS_ROOTINO
        for part in path.strip('/').split('/'):
            if not part:
                continue
            hit = [i for n, i, _ in self.readdir(ino) if n == part]
            if not hit:
                return None
            ino = hit[0]
        return ino


def main():
    if len(sys.argv) < 3:
        sys.exit('usage: ffs.py <image> ls <path> | cat <path> | blocks <path>')
    data = open(sys.argv[1], 'rb').read()
    fs = Ffs(data)
    cmd, path = sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else '/'
    ino = fs.lookup(path)
    if ino is None:
        sys.exit('%s: not found' % path)
    if cmd == 'ls':
        for name, i, t in fs.readdir(ino):
            inode = fs.inode(i)
            print('%-24s ino %-6d mode %06o %10d' %
                  (name, i, inode['mode'], inode['size']))
    elif cmd == 'cat':
        sys.stdout.buffer.write(fs.read(ino))
    elif cmd == 'blocks':
        inode = fs.inode(ino)
        print('size %d, bsize %d, fsize %d' % (inode['size'], fs.bsize, fs.fsize))
        print(fs.blocks(inode))
    elif cmd == 'sb':
        for k in sorted(SB):
            print('%-14s %d' % (k, getattr(fs, k)))
    else:
        sys.exit('unknown command %s' % cmd)


if __name__ == '__main__':
    main()
