"""Check downloaded release bytes, identities and executable icon resources."""
from pathlib import Path
import ctypes as C
from ctypes import wintypes as W
import hashlib, json, zipfile

BASE = Path(r'D:/dev/github/winshun-dev')
OUT = BASE / 'build/binarylab-variants-20261109'
PACKAGES = BASE / 'dist/binarylab-variants-20261109/published'
K, U, V = C.windll.kernel32, C.windll.user32, C.windll.version
K.LoadLibraryExW.argtypes=[W.LPCWSTR,W.HANDLE,W.DWORD];K.LoadLibraryExW.restype=W.HMODULE
K.FreeLibrary.argtypes=[W.HMODULE]
U.LoadImageW.argtypes=[W.HINSTANCE,W.LPCWSTR,W.UINT,C.c_int,C.c_int,W.UINT];U.LoadImageW.restype=W.HANDLE
U.DestroyIcon.argtypes=[W.HICON]
ENUM=C.WINFUNCTYPE(W.BOOL,W.HMODULE,C.c_void_p,C.c_void_p,W.LPARAM)
K.EnumResourceNamesW.argtypes=[W.HMODULE,C.c_void_p,ENUM,W.LPARAM]
V.GetFileVersionInfoSizeW.argtypes=[W.LPCWSTR,C.POINTER(W.DWORD)];V.GetFileVersionInfoSizeW.restype=W.DWORD
V.GetFileVersionInfoW.argtypes=[W.LPCWSTR,W.DWORD,W.DWORD,W.LPVOID]
V.VerQueryValueW.argtypes=[W.LPCVOID,W.LPCWSTR,C.POINTER(W.LPVOID),C.POINTER(W.UINT)]

def exe_info(exe, version):
    ignored=W.DWORD();size=V.GetFileVersionInfoSizeW(str(exe),C.byref(ignored));assert size
    buf=C.create_string_buffer(size);assert V.GetFileVersionInfoW(str(exe),0,size,buf)
    ptr=W.LPVOID();length=W.UINT();assert V.VerQueryValueW(buf,'\\',C.byref(ptr),C.byref(length))
    words=C.cast(ptr,C.POINTER(W.DWORD))
    actual=(words[2]>>16,words[2]&65535,words[3]>>16,words[3]&65535)
    assert actual==tuple(map(int,version.split('.')))+(0,),actual
    module=K.LoadLibraryExW(str(exe),None,0x22);assert module
    try:
        names={}
        for kind in (3,14):
            values=[]
            @ENUM
            def collect(h,ty,n,arg):values.append(n if n<65536 else C.wstring_at(n));return True
            assert K.EnumResourceNamesW(module,kind,collect,0)
            names[kind]=values
        assert names[14]==['IDI_ICON1'] and len(names[3])==8,names
        for pixels in (16,20,24,32,40,48,64,256):
            handle=U.LoadImageW(module,'IDI_ICON1',1,pixels,pixels,0);assert handle
            U.DestroyIcon(handle)
    finally:K.FreeLibrary(module)
    return {'fileVersion':'.'.join(map(str,actual)),'iconGroup':'IDI_ICON1','iconFrames':8}

report={'date':'2026-11-09','packages':[]}
artwork=[]
for variant,version in [('gdi','0.3.2'),('qml','0.4.0')]:
    filename=f'ra9fael.binarylab-{variant}-{version}.wsplugin'
    archive=PACKAGES/variant/filename
    checksum=(archive.parent/(filename+'.sha256')).read_text(encoding='ascii').split()
    digest=hashlib.sha256(archive.read_bytes()).hexdigest()
    assert checksum==[digest,filename]
    target=OUT/f'published-{variant}'/'ra9fael.binarylab';target.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(archive) as z:
        assert not z.testzip()
        names=z.namelist()
        for name in names:
            path=target.joinpath(name).resolve()
            assert path.is_relative_to(target.resolve())
        manifest=json.loads(z.read('plugin.json'));release=json.loads(z.read('release.json'))
        assert manifest['id']=='ra9fael.binarylab' and manifest['version']==version
        assert manifest['icon']=='app.ico' and release['variant']==variant
        assert release['prerelease']==(variant=='qml')
        dlls=sorted(n for n in names if n.lower().endswith('.dll'))
        assert dlls==(['BinaryLabApp.dll'] if variant=='qml' else [])
        artwork.append(hashlib.sha256(z.read('app.ico')).hexdigest())
        z.extractall(target)
    report['packages'].append({'variant':variant,'version':version,'tag':f'{variant}-v{version}',
        'file':filename,'bytes':archive.stat().st_size,'sha256':digest,'prerelease':release['prerelease'],
        **exe_info(target/'binarylab.exe',version)})
assert artwork[0]==artwork[1]
report['sharedIconSha256']=artwork[0]
(OUT/'published-packages.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(json.dumps(report,ensure_ascii=False,indent=2))
