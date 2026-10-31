#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 XHYN-PH
"""Add the upscaler rows to the launcher's Graphics > Display page.

The launcher's menu is compiled code, so this patches two lines of its smali and adds
menu/src as a new dex. Run install_shim.py on the result. The input APK is never modified.

  add_menu.py --apk XHYN-V-1.7.4-English.apk --out with-menu.apk \
      --android-jar <sdk>/platforms/android-36/android.jar [--d8 d8] [--apktool apktool]
"""
import argparse
import pathlib
import subprocess
import tempfile
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
ACTIVITY = 'smali/com/gtavsource/android/GameMenuActivity.smali'
PAGE_FIELD = '.field private page:I\n'
DISPLAY_METHOD = '.method synthetic lambda$display$0$com-gtavsource-android-GameMenuActivity()V\n'
HOOK = ('    iget-object v0, p0, Lcom/gtavsource/android/GameMenuActivity;->content:Landroid/widget/LinearLayout;\n\n'
        '    invoke-static {p0, v0}, Lcom/gtavsource/android/XhynUpscalerMenu;->add('
        'Lcom/gtavsource/android/GameMenuActivity;Landroid/widget/LinearLayout;)V\n\n')


def patch_smali(path):
    text = path.read_text(encoding='utf-8')
    if text.count(PAGE_FIELD) != 1 or text.count(DISPLAY_METHOD) != 1:
        raise SystemExit('GameMenuActivity does not look like the expected 1.7.4 build')
    text = text.replace(PAGE_FIELD, '.field page:I\n')
    start = text.index(DISPLAY_METHOD)
    end = text.index('.end method', start)
    position = text.rindex('    return-void', start, end)
    path.write_text(text[:position] + HOOK + text[position:], encoding='utf-8')


def build_dex(work, android_jar, d8):
    classes = work / 'classes'
    classes.mkdir()
    sources = sorted(map(str, (ROOT / 'menu').rglob('*.java')))
    subprocess.run(['javac', '--release', '8', '-nowarn', '-cp', str(android_jar), '-d', str(classes)] + sources,
                   check=True, capture_output=True, text=True)
    helper = sorted(map(str, classes.glob('com/gtavsource/android/XhynUpscalerMenu*.class')))
    output = work / 'dex'
    output.mkdir()
    subprocess.run([d8, '--min-api', '30', '--lib', str(android_jar), '--classpath', str(classes),
                    '--output', str(output)] + helper, check=True, capture_output=True, text=True)
    return (output / 'classes.dex').read_bytes()


def next_dex_name(names):
    index = 2
    while 'classes%d.dex' % index in names:
        index += 1
    return 'classes%d.dex' % index


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--apk', required=True, type=pathlib.Path)
    parser.add_argument('--out', required=True, type=pathlib.Path)
    parser.add_argument('--android-jar', required=True, type=pathlib.Path)
    parser.add_argument('--d8', default='d8')
    parser.add_argument('--apktool', default='apktool')
    args = parser.parse_args()
    if args.out.exists():
        parser.error('output already exists: %s' % args.out)
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(tmp)
        decoded = work / 'decoded'
        # -r leaves the manifest and resources untouched.
        subprocess.run([args.apktool, 'd', '-r', '-q', '-o', str(decoded), str(args.apk)], check=True)
        patch_smali(decoded / ACTIVITY)
        dex = build_dex(work, args.android_jar, args.d8)
        rebuilt = work / 'rebuilt.apk'
        subprocess.run([args.apktool, 'b', '-q', '-o', str(rebuilt), str(decoded)], check=True)
        with zipfile.ZipFile(rebuilt) as src, zipfile.ZipFile(args.out, 'w') as dst:
            for info in src.infolist():
                entry = zipfile.ZipInfo(info.filename, info.date_time)
                entry.compress_type = info.compress_type
                entry.external_attr = info.external_attr
                dst.writestr(entry, src.read(info.filename))
            name = next_dex_name(src.namelist())
            dst.writestr(name, dex, zipfile.ZIP_DEFLATED)
    print('wrote %s with the menu in %s' % (args.out, name))


if __name__ == '__main__':
    main()
