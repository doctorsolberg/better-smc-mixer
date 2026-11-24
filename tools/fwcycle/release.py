"""Strict local release pinning; no downloads or hardware access."""
import hashlib
import json
from pathlib import Path, PurePosixPath

def validate(path):
    path=Path(path).absolute()
    if path.is_symlink():
        raise ValueError('manifest must not be a symlink')
    raw=path.read_bytes()
    m=json.loads(raw)
    if m.get('product')!='better-smc-mixer':
        raise ValueError('wrong release product')
    install=m.get('install')
    if not isinstance(install,dict) or set(install)!={'app','cfg'}:
        raise ValueError('release needs app and cfg only')
    pinned={}
    root=path.parent.resolve()
    for role,name in install.items():
        if not isinstance(name,str) or not name or '\\' in name:
            raise ValueError('invalid release path')
        rel=PurePosixPath(name)
        if rel.is_absolute() or '..' in rel.parts or '.'==name:
            raise ValueError('release path escapes package')
        candidate=root
        for part in rel.parts:
            candidate=candidate/part
            if candidate.is_symlink():raise ValueError('symlink in release path')
        if not candidate.is_file() or not candidate.resolve().is_relative_to(root):
            raise ValueError('release input missing or outside package')
        want=m.get('files',{}).get(name)
        if not isinstance(want,dict):raise ValueError('missing release file record')
        data=candidate.read_bytes()
        digest=hashlib.sha256(data).hexdigest()
        if not data or type(want.get('size')) is not int or want['size']!=len(data) or want.get('sha256')!=digest:
            raise ValueError('release input hash or size mismatch: '+role)
        pinned[role]={'name':name,'size':len(data),'sha256':digest}
    images=[im for im in m.get('images',{}).values() if isinstance(im,dict) and im.get('file')==install['app']]
    expected=None
    if len(images)>1:raise ValueError('ambiguous installed image')
    if images:
        im=images[0]
        expected=im.get('build_id')
        if not isinstance(expected,str) or len(expected)!=40 or any(c not in '0123456789abcdef' for c in expected):
            raise ValueError('invalid expected build')
        if im.get('sha256')!=pinned['app']['sha256'] or im.get('size')!=pinned['app']['size']:
            raise ValueError('image identity does not bind installed app')
    return {'path':str(path),'manifest_sha256':hashlib.sha256(raw).hexdigest(),
            'expected_build':expected,'product':m['product'],'version':m.get('version'),'install':pinned,'manifest':m}
