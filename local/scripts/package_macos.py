#!/usr/bin/env python3
"""Package prebuilt WASM and a Go launcher; no dependencies on the user's Mac."""
import argparse
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import time

p = argparse.ArgumentParser()
p.add_argument('--wasm-build', required=True, type=Path)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--go', default='go')
p.add_argument('--archive-only', action='store_true', help='write only the signed ZIP, recommended for cloud-synced output folders')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
destination = a.output / 'Flight Replay Local.app'
if destination.exists():
    raise SystemExit(f'Refusing to replace existing app: {destination}')
# Sign outside File Provider folders; their asynchronous FinderInfo writes
# can race codesign even after explicitly clearing the generated metadata.
staging = tempfile.TemporaryDirectory(prefix='flight-replay-package-')
app = Path(staging.name) / 'Flight Replay Local.app'
mac = app / 'Contents/MacOS'
resources = app / 'Contents/Resources'
web = resources / 'web'
mac.mkdir(parents=True)
shutil.copytree(root / 'local/web', web, copy_function=shutil.copy)
(web / 'engine').mkdir()
for name in ['hawkeye.js', 'hawkeye.wasm', 'hawkeye.data']:
    # Neutral asset names avoid unrelated tracker-name content-blocker rules.
    shutil.copyfile(a.wasm_build / name, web / 'engine' / name.replace('hawkeye.', 'replay-core.'))
for name in ['LICENSE', 'NOTICE.md', 'FORKS.md']:
    shutil.copyfile(root / name, resources / name)
shutil.copyfile(root / 'fonts/OFL.txt', resources / 'FONT_LICENSE.txt')
subprocess.run([a.go, 'build', '-trimpath', '-ldflags=-s -w', '-o', str(mac/'FlightReplayLocal'), '.'], cwd=root/'local/launcher', check=True)
with (app/'Contents/Info.plist').open('wb') as f:
    plistlib.dump({'CFBundleName':'Flight Replay Local','CFBundleDisplayName':'Flight Replay Local','CFBundleIdentifier':'io.github.rhyne1012.flight-replay-local','CFBundleExecutable':'FlightReplayLocal','CFBundlePackageType':'APPL','CFBundleShortVersionString':'0.1.0','CFBundleVersion':'1','LSMinimumSystemVersion':'12.0','LSUIElement':True,'NSHighResolutionCapable':True},f)
# File Provider can add FinderInfo to newly created .app directories. Strip
# only signing-incompatible metadata, and only inside this generated bundle.
for item in [app, *app.rglob('*')]:
    attrs = subprocess.check_output(['xattr', str(item)], text=True).splitlines()
    for attr in ('com.apple.FinderInfo', 'com.apple.ResourceFork'):
        if attr in attrs:
            subprocess.run(['xattr','-d',attr,str(item)], check=True)
subprocess.run(['codesign','--force','--sign','-',str(app)],check=True)
subprocess.run(['codesign','--verify','--strict',str(app)],check=True)
a.output.mkdir(parents=True, exist_ok=True)
archive = a.output / 'Flight-Replay-Local-macos.zip'
subprocess.run(['ditto','-c','-k','--norsrc','--keepParent',str(app),str(archive)],check=True)
if a.archive_only:
    staging.cleanup()
    print(archive)
    raise SystemExit(0)
shutil.copytree(app, destination, copy_function=shutil.copy)
for attempt in range(4):
    for item in [destination, *destination.rglob('*')]:
        attrs = subprocess.check_output(['xattr', str(item)], text=True).splitlines()
        for attr in ('com.apple.FinderInfo', 'com.apple.ResourceFork'):
            if attr in attrs:
                subprocess.run(['xattr','-d',attr,str(item)], check=True)
    verification = subprocess.run(['codesign','--verify','--strict',str(destination)], capture_output=True, text=True)
    if verification.returncode == 0:
        break
    time.sleep(0.25)
else:
    raise SystemExit(f'Signed ZIP created at {archive}; destination metadata prevents bundle verification: {verification.stderr}')
staging.cleanup()
print(destination)
print(archive)
