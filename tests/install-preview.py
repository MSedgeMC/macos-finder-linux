#!/usr/bin/env python3
"""Exercise viewer installation against temporary files, never the desktop."""
import configparser
import importlib.util
from pathlib import Path
import tempfile
spec=importlib.util.spec_from_file_location('preview_install',Path(__file__).resolve().parents[1]/'tools/install-preview.py')
installer=importlib.util.module_from_spec(spec);spec.loader.exec_module(installer)
with tempfile.TemporaryDirectory(prefix='finder-viewer-install-') as directory:
    root=Path(directory);installer.HOME=root;installer.STATE=root/'state';installer.finder.HOME=root
    mime=root/'.config/mimeapps.list';mime.parent.mkdir(parents=True)
    original=('# Keep this comment\n[Default Applications]\ninode/directory=org.local.finder.desktop;\n'
        'application/pdf=okularApplication_pdf.desktop;\nimage/png=org.kde.gwenview.desktop;\n'
        'text/plain=org.kde.kate.desktop;\napplication/vnd.openxmlformats-officedocument.wordprocessingml.document=libreoffice-writer.desktop;\n'
        '[Added Associations]\nimage/png=org.kde.gwenview.desktop;\n')
    mime.write_text(original);mime.chmod(0o600)
    binary=root/'.local/share/finder-v2/bin/finder';binary.parent.mkdir(parents=True);binary.write_bytes(b'fixture binary only')
    def run(*args):
        if args[:3]==('xdg-mime','query','default'):
            config=configparser.ConfigParser(interpolation=None);config.read(mime)
            return config.get('Default Applications',args[3],fallback='').split(';')[0]
        assert args==('kbuildsycoca6','--noincremental');return ''
    installer.run=run
    before=installer.defaults();state=installer.prepare()
    _,manifest=installer.load(state)
    assert len(manifest['files'])==3 and manifest['state']=='prepared' and mime.read_text()==original
    installer.apply(state);assert installer.verify(state)['passed']
    installed=mime.read_text()
    for value in ('# Keep this comment','inode/directory=org.local.finder.desktop;','text/plain=org.kde.kate.desktop;',
        'application/vnd.openxmlformats-officedocument.wordprocessingml.document=libreoffice-writer.desktop;','[Added Associations]\nimage/png=org.kde.gwenview.desktop;'):
        assert value in installed,value
    assert mime.stat().st_mode&0o777==0o600
    assert (root/'.local/bin/preview').stat().st_mode&0o777==0o755
    assert '--preview -- "$@"' in (root/'.local/bin/preview').read_text()
    installer.restore(state);assert mime.read_text()==original and installer.defaults()==before
    assert not (root/'.local/bin/preview').exists() and not (root/'.local/share/applications/org.local.preview.desktop').exists()
    # Stale staging never overwrites changed configuration.
    second=installer.prepare();mime.write_text(original+'# external edit\n')
    try:installer.apply(second)
    except RuntimeError as error:assert 'changed' in str(error).lower()
    else:raise AssertionError('stale MIME file accepted')
    assert mime.read_text().endswith('# external edit\n')
print('PASS Preview installer staging, exact scope, MIME preservation, permissions, rollback and stale configuration')
