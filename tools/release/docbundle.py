"""Offline guide/reference links for the dry package. Standard library; never fetches a URL.

Only eligible public Markdown and linked text assets are added. Existing package artifacts may
be linked through explicit aliases. Code-only references remain labelled source paths, not
broken links or copies of the source/tools. Ignored and external Markdown cannot be pulled in.
"""
import os
import posixpath
import re
import shutil

import check_docs
import check_public

REFERENCE_NOTE = ("> Developer reference copied from the source tree. Commands and source paths refer to\n"
                  "> a full checkout, not this binary package. References labelled as source-only\n"
                  "> are not included here. Device confidence marks and draft warnings are unchanged.\n\n")
ASSETS = {'.json', '.svg', '.txt'}


def normal(path):
    return posixpath.normpath(os.fspath(path).replace('\\', '/'))


def inside(path, directory):
    try:
        return os.path.commonpath([os.path.normcase(path), os.path.normcase(directory)]) == os.path.normcase(directory)
    except ValueError:  # separate Windows drives
        return False


def bundle(root, out, aliases):
    root, out = os.path.realpath(root), os.path.realpath(out)
    public = set(check_public.published(root))
    paths = {}
    for src, dst in aliases.items():
        src = os.path.relpath(src, root) if os.path.isabs(src) else src
        dst = normal(dst)
        if posixpath.isabs(dst) or re.match(r'^[A-Za-z]:', dst) or dst == '..' or dst.startswith('../'):
            raise ValueError('package documentation alias leaves the package')
        paths[normal(src)] = dst
    existing = set(paths)
    files = []

    def eligible(rel):
        source = os.path.realpath(os.path.join(root, *rel.split('/')))
        resolved = os.path.relpath(source, root).replace(os.sep, '/')
        return (rel in public and resolved in public and inside(source, root) and os.path.isfile(source)
                and not inside(source, out) and not rel.startswith('third_party/')
                and not resolved.startswith('third_party/'))

    markdown = []
    for rel in sorted(public):
        if not rel.endswith('.md') or rel.startswith('third_party/'):
            continue
        source = os.path.realpath(os.path.join(root, *rel.split('/')))
        if inside(source, out):
            continue
        if not eligible(rel):
            raise ValueError('documentation path is not in the public source tree: ' + rel)
        if rel in existing:
            continue  # an already-copied license or artifact stays byte-for-byte unchanged
        paths[rel] = 'guide/' + rel[len('docs/user/'):] if rel.startswith('docs/user/') else 'reference/' + rel
        markdown.append(rel)

    def add_asset(rel):
        dst = 'reference/' + rel
        target = os.path.join(out, *dst.split('/'))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copyfile(os.path.join(root, *rel.split('/')), target)
        paths[rel] = dst
        files.append(dst)

    for src in markdown:
        dst = paths[src]
        with open(os.path.join(root, *src.split('/')), encoding='utf8') as f:
            text = f.read()
        scan = check_docs.prose(text)
        edits = []
        for match in check_docs.LINK.finditer(scan):
            url = match.group(1)
            if re.match(r'^[a-z][a-z0-9+.-]*:', url, re.I) or url.startswith('#'):
                continue
            path, _, anchor = url.partition('#')
            path = re.sub(r':\d+(?:-\d+)?$', '', path)
            rel = normal(posixpath.join(posixpath.dirname(src), path))
            if rel not in paths and posixpath.splitext(rel)[1] in ASSETS and eligible(rel):
                add_asset(rel)
            if rel in paths:
                target = posixpath.relpath(paths[rel], posixpath.dirname(dst) or '.')
                if path.endswith('/'):
                    target += '/'
                if anchor:
                    target += '#' + anchor
                edits.append((match.start(1), match.end(1), target))
                continue
            if rel.endswith('.md') or src.startswith('docs/user/'):
                raise ValueError('guide/reference dependency is not publishable: %s -> %s' % (src, url))
            label = text[match.start() + 1:text.find(']', match.start())]
            suffix = text[match.end() - len(match.group(2)):match.end()] if match.group(2) else ''
            start = match.start() - int(match.start() > 0 and text[match.start() - 1] == '!')
            edits.append((start, match.end(), '%s (source reference, not included: `%s`)%s' % (label, url, suffix)))
        for start, end, replacement in reversed(edits):
            text = text[:start] + replacement + text[end:]
        if dst.startswith('reference/'):
            text = REFERENCE_NOTE + text
        target = os.path.join(out, *dst.split('/'))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, 'w', encoding='utf8', newline='\n') as f:
            f.write(text)
        files.append(dst)
    return sorted(set(files))
