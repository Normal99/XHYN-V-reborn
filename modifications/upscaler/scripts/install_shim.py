#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 XHYN-PH
"""Put the upscaler shim into the BCn layer slot of an XHYN V APK.

libgtav_vulkan.so loads lib/arm64-v8a/libgtav_bcn.so by name. This tool

  1. renames the original library to libxhyn_bcn.so (and patches its SONAME to match,
     same length, so the two never collide in the linker),
  2. installs the shim built from this directory as libgtav_bcn.so,
  3. drops the old signature and optionally signs with apksigner the same way
     modifications/english-1.7.4/build.py does.

Nothing else in the APK is touched. The input APK is never modified.

  install_shim.py --apk XHYN-V-1.7.4-English.apk --shim build/android/libgtav_bcn.so \
      --out XHYN-V-1.7.4-English-sgsr.apk \
      [--apksigner <build-tools>/apksigner --keystore my.p12 --key-alias alias --password-file pw.txt]
"""
import argparse
import hashlib
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import zipfile

LIB_DIR = 'lib/arm64-v8a/'
SLOT = LIB_DIR + 'libgtav_bcn.so'
RENAMED = LIB_DIR + 'libxhyn_bcn.so'
OLD_SONAME = b'libgtav_bcn.so'
NEW_SONAME = b'libxhyn_bcn.so'
EXPECTED_SYMBOLS = {'BCnLayer_GetInstanceProcAddr', 'BCnLayer_GetDeviceProcAddr', 'BCnLayer_Diagnostics'}
# SHA-256 of the original libgtav_bcn.so shipped in XHYN-V-1.7.4-English.apk
KNOWN_BCN_SHA256 = '726586bf8eb65eb41779da66866a40682fc1acdf498e0b307774cbb082c37768'
SIGNATURE_ENTRY = re.compile(r'^META-INF/[^/]+\.(SF|RSA|DSA|EC)$|^META-INF/MANIFEST\.MF$')


class Elf:
    """Just enough ELF64 to read the dynamic section of a shared object."""

    def __init__(self, data):
        if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1:
            raise ValueError('not a little-endian ELF64 file')
        self.data = data
        self.type, self.machine = struct.unpack_from('<HH', data, 16)
        phoff, = struct.unpack_from('<Q', data, 32)
        phentsize, phnum = struct.unpack_from('<HH', data, 54)
        self.loads, self.alignments, self.dynamic = [], [], None
        for i in range(phnum):
            p_type, _, p_offset, p_vaddr, _, p_filesz, _, p_align = struct.unpack_from('<IIQQQQQQ', data, phoff + i * phentsize)
            if p_type == 1:
                self.loads.append((p_vaddr, p_offset, p_filesz))
                self.alignments.append(p_align)
            elif p_type == 2:
                self.dynamic = (p_offset, p_filesz)

    def offset(self, vaddr):
        for start, offset, size in self.loads:
            if start <= vaddr < start + size:
                return offset + vaddr - start
        raise ValueError('address outside loadable segments')

    def dynamic_entries(self):
        offset, size = self.dynamic
        for i in range(0, size, 16):
            tag, value = struct.unpack_from('<qQ', self.data, offset + i)
            if tag == 0:
                break
            yield tag, value

    def soname_offset(self):
        entries = dict(self.dynamic_entries())
        return self.offset(entries[5]) + entries[14]  # DT_STRTAB + DT_SONAME

    def soname(self):
        start = self.soname_offset()
        return self.data[start:self.data.index(b'\0', start)]

    def exported_symbols(self):
        data = self.data
        shoff, = struct.unpack_from('<Q', data, 40)
        shentsize, shnum = struct.unpack_from('<HH', data, 58)
        sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
        names = set()
        for _, sh_type, _, _, sh_offset, sh_size, sh_link, _, _, _ in sections:
            if sh_type != 11:  # SHT_DYNSYM
                continue
            strtab = sections[sh_link][4]
            for i in range(sh_size // 24):
                st_name, st_info, _, st_shndx, _, _ = struct.unpack_from('<IBBHQQ', data, sh_offset + i * 24)
                if st_shndx != 0 and (st_info >> 4) in (1, 2) and st_name:
                    start = strtab + st_name
                    names.add(data[start:data.index(b'\0', start)].decode())
        return names


def check_shim(data):
    elf = Elf(data)
    if elf.machine != 183 or elf.type != 3:
        raise ValueError('shim must be an AArch64 shared object (EM_AARCH64, ET_DYN)')
    if elf.soname() != OLD_SONAME:
        raise ValueError('shim SONAME is %r, expected libgtav_bcn.so' % elf.soname().decode())
    if any(a < 16384 for a in elf.alignments):
        raise ValueError('segments must be 16 KiB aligned (link with -Wl,-z,max-page-size=16384)')
    missing = EXPECTED_SYMBOLS - elf.exported_symbols()
    if missing:
        raise ValueError('shim does not export: ' + ', '.join(sorted(missing)))


def rename_soname(data):
    elf = Elf(data)
    if elf.soname() != OLD_SONAME:
        raise ValueError('original BCn library has SONAME %r, expected libgtav_bcn.so' % elf.soname().decode())
    assert len(OLD_SONAME) == len(NEW_SONAME)
    out = bytearray(data)
    start = elf.soname_offset()
    out[start:start + len(NEW_SONAME)] = NEW_SONAME
    if Elf(bytes(out)).soname() != NEW_SONAME:
        raise ValueError('SONAME patch did not take effect')
    if len(out) != len(data):
        raise ValueError('patched library changed size')
    return bytes(out)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def rewrite(apk, shim, out, allow_other):
    with zipfile.ZipFile(apk) as src:
        names = src.namelist()
        if SLOT not in names:
            raise SystemExit('APK has no ' + SLOT)
        if RENAMED in names:
            raise SystemExit('APK already contains ' + RENAMED + ' (shim installed?)')
        original = src.read(SLOT)
        digest = sha256(original)
        if digest != KNOWN_BCN_SHA256 and not allow_other:
            raise SystemExit('libgtav_bcn.so in the APK has SHA-256 %s, not the known one; pass --allow-other-bcn '
                             'if this APK was built differently' % digest)
        patched = rename_soname(original)
        with zipfile.ZipFile(out, 'w') as dst:
            for info in src.infolist():
                if SIGNATURE_ENTRY.match(info.filename):
                    continue
                if info.filename == SLOT:
                    for name, payload in ((SLOT, shim), (RENAMED, patched)):
                        entry = zipfile.ZipInfo(name, info.date_time)
                        entry.compress_type = zipfile.ZIP_STORED
                        entry.external_attr = info.external_attr
                        dst.writestr(entry, payload)
                    continue
                entry = zipfile.ZipInfo(info.filename, info.date_time)
                entry.compress_type = info.compress_type
                entry.external_attr = info.external_attr
                dst.writestr(entry, src.read(info.filename))
    return digest


def apksigner_command(args):
    # Android build-tools ship an executable wrapper; the pinned jar is run through java.
    return ['java', '-jar', str(args.apksigner)] if args.apksigner.suffix == '.jar' else [str(args.apksigner)]


def sign(args, unsigned, final):
    command = apksigner_command(args) + ['sign', '--ks', str(args.keystore), '--ks-key-alias', args.key_alias,
               '--ks-pass', 'file:' + str(args.password_file), '--v4-signing-enabled', 'false',
               '--alignment-preserved', 'false', '--lib-page-alignment', '16384', '--out', str(final), str(unsigned)]
    subprocess.run(command, check=True)
    subprocess.run(apksigner_command(args) + ['verify', '--verbose', str(final)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--check-only', action='store_true', help='only verify --shim, then exit')
    parser.add_argument('--apk', type=pathlib.Path)
    parser.add_argument('--shim', required=True, type=pathlib.Path, help='libgtav_bcn.so built from modifications/upscaler')
    parser.add_argument('--out', type=pathlib.Path)
    parser.add_argument('--allow-other-bcn', action='store_true')
    parser.add_argument('--apksigner', type=pathlib.Path, help='apksigner.jar; with --keystore signs the result')
    parser.add_argument('--keystore', type=pathlib.Path)
    parser.add_argument('--key-alias')
    parser.add_argument('--password-file', type=pathlib.Path)
    args = parser.parse_args()
    if not args.check_only and not (args.apk and args.out):
        parser.error('--apk and --out are required')
    signing = args.keystore is not None
    if signing and not (args.apksigner and args.key_alias and args.password_file):
        parser.error('signing needs --apksigner, --keystore, --key-alias and --password-file')
    shim = args.shim.read_bytes()
    try:
        check_shim(shim)
    except ValueError as error:
        sys.exit('shim check failed: %s' % error)
    if args.check_only:
        print('shim OK: AArch64 ET_DYN, SONAME libgtav_bcn.so, 16 KiB aligned, exports the three BCnLayer_* symbols '
              '(SHA-256 %s)' % sha256(shim))
        return
    if args.out.exists():
        parser.error('output already exists: %s' % args.out)

    if signing:
        with tempfile.TemporaryDirectory() as tmp:
            unsigned = pathlib.Path(tmp) / 'unsigned.apk'
            digest = rewrite(args.apk, shim, unsigned, args.allow_other_bcn)
            sign(args, unsigned, args.out)
    else:
        digest = rewrite(args.apk, shim, args.out, args.allow_other_bcn)
    print('original BCn layer (SHA-256 %s) is now %s' % (digest, RENAMED))
    print('shim (SHA-256 %s) installed as %s' % (sha256(shim), SLOT))
    print('wrote %s' % args.out)
    if not signing:
        print('UNSIGNED. Sign and align it before installing, e.g.:\n'
              '  java -jar apksigner.jar sign --ks KEY.p12 --ks-key-alias ALIAS --ks-pass file:PASSWORD.txt '
              '--v4-signing-enabled false --alignment-preserved false --lib-page-alignment 16384 --out FINAL.apk %s' % args.out)


if __name__ == '__main__':
    main()
