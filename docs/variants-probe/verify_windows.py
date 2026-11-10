"""Real Host/Runner launches with strict sandbox; inspect only task-owned windows."""
from pathlib import Path
import ctypes as C
from ctypes import wintypes as W
import json, os, subprocess, sys, time
BASE = Path(r'D:/dev/github/winshun-dev')
OUT = BASE / 'build/binarylab-variants-20261109'
sys.path.insert(0, str(BASE / 'ra9fael.whatis/tests'))
import integration as t
import window_icons as icons
U, K = t.U, t.K
K.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]; K.OpenProcess.restype = W.HANDLE
K.QueryFullProcessImageNameW.argtypes = [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)]
K.CloseHandle.argtypes = [W.HANDLE]
U.ShowWindow.argtypes = [W.HWND, C.c_int]

def owned_window(exe):
    found=[]
    @t.CALLBACK
    def visit(hwnd, _):
        if not U.IsWindowVisible(hwnd): return True
        h=K.OpenProcess(0x1000,False,t.pid_of(hwnd))
        if h:
            try:
                path=C.create_unicode_buffer(32768);size=W.DWORD(32768)
                if K.QueryFullProcessImageNameW(h,0,path,C.byref(size)) and Path(path.value)==exe:
                    found.append(hwnd)
            finally:K.CloseHandle(h)
        return True
    U.EnumWindows(visit,0)
    return found[0] if found else None

def verify(hwnd, root, label, dpi=None, qt=False):
    icons.ROOT=root
    if not qt:
        icons.check_window_icons(hwnd,label,dpi)
        return
    for kind in (1,0):
        handle=t.send(hwnd,0x7F,kind)
        t.check(label+f' window icon {kind} exists',bool(handle))
        size=icons.icon_size(handle)[0]
        t.check(label+f' icon {kind} uses supplied resolution',size in (16,20,24,32,40,48,64,256),size)
        expected=U.LoadImageW(None,str(root/'app.ico'),1,size,size,0x10)
        try:
            actual=icons.icon_pixels(handle,size);reference=icons.icon_pixels(expected,size)
            largest=max(abs(a-b) for a,b in zip(actual,reference))
            t.check(label+f' icon {kind} matches shared artwork',largest<=1,{'size':size,'max_delta':largest})
        finally:U.DestroyIcon(expected)

prefix=sys.argv[1] if len(sys.argv)>1 else 'deploy'
results=[]
runs=[('gdi','1'),('qml','1')] if prefix != 'deploy' else [('gdi','1'),('qml','1'),('qml','1.25'),('qml','1.5'),('qml','2')]
for variant,scale in runs:
    label=f'{variant}-{scale}'
    root=OUT/f'{prefix}-{variant}'/'ra9fael.binarylab'
    log=OUT/f'{prefix}-window-{label}.log'
    environment=os.environ.copy()
    environment['PATH']=r'D:\tools\Qt\6.12.0\msvc2022_64\bin;'+environment['PATH']
    environment['QT_SCALE_FACTOR']=scale
    with log.open('wb') as output:
        proc=subprocess.Popen([str(BASE/'WinShun/build/debug/wsbench.exe'),'--plugin',str(root),
            '--query','bits f32 1.5','--runs','1','--action','openEditor','--linger','8000',
            '--app-dir',r'C:\Program Files\WinShun'],stdout=output,stderr=output,env=environment,
            creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            hwnd=t.wait(lambda:owned_window(root/'binarylab.exe'),7)
            verify(hwnd,root,label,qt=variant=='qml')
            time.sleep(.6)
            if scale=='1':t.screenshot(hwnd,OUT/f'{prefix}-{variant}-window.png')
            if variant=='gdi':
                U.ShowWindow(hwnd,0)
                for dpi in [96,120,144,192,96]:
                    bounds=t.rect(hwnd);t.send(hwnd,0x2E0,(dpi<<16)|dpi,C.addressof(bounds))
                    verify(hwnd,root,label+f'-{dpi}',dpi)
            code=proc.wait(timeout=12)
            t.check(label+' real host exits cleanly',code==0,code)
            body=log.read_text(encoding='utf-8',errors='replace')
            t.check(label+' plugin uses strict sandbox','integrity LOW' in body and 'in a job yes' in body)
            t.check(label+' action opened editor','openEditor' in body)
            results.append({'variant':variant,'scale':scale,'passed':True})
        finally:
            if proc.poll() is None:proc.terminate();proc.wait(timeout=8)
(OUT/f'{prefix}-window-results.json').write_text(json.dumps({'runs':results,'checks':t.checks},ensure_ascii=False,indent=2),encoding='utf-8')
print('ALL WINDOW CHECKS PASSED',len(t.checks),flush=True)
