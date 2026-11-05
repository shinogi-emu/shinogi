#!/usr/bin/env python3
"""Exercise Mac launcher path/copy/firmware rules with a dyld-path shim.
This runs on Linux; real Mach-O signing/runtime remain GitHub macOS checks.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
work=Path(tempfile.mkdtemp(prefix='shinogi-mac-paths-'))
include=work/'include/mach-o';include.mkdir(parents=True)
(include/'dyld.h').write_text('#include <stdint.h>\nint _NSGetExecutablePath(char *, uint32_t *);\n')
shim=work/'dyld.c';shim.write_text('''#include <unistd.h>
#include <stdint.h>
int _NSGetExecutablePath(char *out, uint32_t *n) {
ssize_t size=readlink("/proc/self/exe",out,*n-1);if(size<0)return -1;out[size]=0;return 0;}
''')
home=work/"Home with space's";home.mkdir()
tmp=work/'tmp';tmp.mkdir()
helper=work/'helper'
subprocess.run(['cc',str(root/'tools/hostfsd/shinogi-hostfsd.c'),'-O2','-Wall','-Wextra','-Werror','-o',str(helper)],check=True)
for edition,cpu in [('Shinogi-040','m68040'),('Shinogi-060','m68060')]:
    app=work/(edition+'.app');mac=app/'Contents/MacOS';res=app/'Contents/Resources'
    (mac/'qemu/bin').mkdir(parents=True);(res/'drive-c/AUTO').mkdir(parents=True)
    (res/'drive-c/AUTO/MINT.PRG').write_text('kernel')
    (res/'drive-c/EMUTOS.IMG').write_text('old guest copy')
    (res/'emutos-virt.elf').write_text('current bundled firmware')
    subprocess.run(['cc',str(root/'tools/shinogi-launcher.c'),str(shim),'-I',str(work/'include'),'-D__APPLE__','-DSHINOGI_VERSION="test"',f'-DSHINOGI_EDITION="{edition}"',f'-DSHINOGI_CPU="{cpu}"','-O2','-Wall','-Wextra','-Werror','-o',str(mac/'shinogi')],check=True)
    (mac/'shinogi-hostfsd').symlink_to(helper)
    emulator=mac/'qemu/bin/qemu-system-m68k'
    emulator.write_text('''#!/usr/bin/env python3
import json,os,socket,struct,sys
from pathlib import Path
args=sys.argv[1:]; transport=args[args.index('-chardev')+1]
path=next(p[5:] for p in transport.split(',') if p.startswith('path='))
with socket.socket(socket.AF_UNIX) as s:
 s.connect(path);s.sendall(struct.pack('<HHIII',16,1,1,0,0));s.recv(4096)
Path(os.environ['CAPTURE']).write_text(json.dumps(args))
''');emulator.chmod(0o755)
    drive=home/edition/'drive-c';capture=work/(edition+'.json')
    env=os.environ|{'HOME':str(home),'TMPDIR':str(tmp),'SHINOGI_DISPLAY':'none','CAPTURE':str(capture)}
    env.pop('SHINOGI_HOSTFS',None)
    def launch():
        subprocess.run([str(mac/'shinogi')],env=env,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        args=json.loads(capture.read_text());return Path(args[args.index('-kernel')+1]).resolve()
    assert launch()==res/'emutos-virt.elf'
    assert (drive/'AUTO/MINT.PRG').read_text()=='kernel'
    (drive/'KEEP.TXT').write_text('personal file')
    assert launch()==res/'emutos-virt.elf'
    assert (drive/'KEEP.TXT').read_text()=='personal file'
    (drive/'CUSTOM.IMG').write_text('explicit override')
    assert launch()==drive/'CUSTOM.IMG'
    print('PASS',edition,'independent boot folder, quoted first copy, bundled firmware upgrade and explicit override')
assert (home/'Shinogi-040/drive-c')!=(home/'Shinogi-060/drive-c')
print('Artifacts:',work)
