#!/usr/bin/env python3
"""Check suite structure; optionally compare declarations in an mcpp checkout.

Run without mcpp for an independently copied suite. --mcpp-root checks source
existence and declaration drift; it does not build or prove runtime semantics.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from urllib.parse import unquote

from inventory import extract


def inside(root: Path, relative: str) -> Path:
    path = (root / relative).resolve()
    if not path.is_relative_to(root.resolve()):
        raise ValueError('path leaves suite: ' + relative)
    return path


def check_indexes(root: Path, skill_path: Path, name: str, relative: str,
                  errors: list[str]) -> None:
    """Append bilingual suite and category index diagnostics for one skill."""
    for directory in (root, root / 'skills', skill_path.parent.parent):
        for filename in ('README.md', 'README.zh-CN.md'):
            if not (directory / filename).is_file():
                errors.append('missing index: ' + str(directory / filename))
    for filename in ('README.md', 'README.zh-CN.md'):
        index = skill_path.parent.parent / filename
        if index.is_file() and f'({name}/SKILL.md)' not in index.read_text(encoding='utf-8'):
            errors.append('unindexed skill: ' + relative + ' in ' + filename)


def check(root: Path, mcpp: Path | None = None) -> tuple[list[str], list[str]]:
    errors, notices = [], []
    try:
        catalog = json.loads((root / 'catalog.json').read_text(encoding='utf-8'))
        skills = catalog['skills']
        entries = catalog['interfaces']
        if catalog['schema_version'] != 1 or not skills or not entries:
            raise ValueError('unsupported or empty catalog')
    except (OSError, ValueError, KeyError, TypeError) as exc:
        return ['catalog: ' + str(exc)], notices
    names, expected_paths = set(), set()
    for skill in skills:
        try:
            name, relative = skill['name'], skill['path']
            path = inside(root, relative)
            body = path.read_text(encoding='utf-8')
            if name in names:
                errors.append('duplicate skill: ' + name)
            names.add(name)
            expected_paths.add(path.resolve())
            if not re.fullmatch(r'mcpp-[a-z0-9]+(?:-[a-z0-9]+)*', name) or len(name) > 64:
                errors.append('invalid name: ' + name)
            if path.name != 'SKILL.md' or path.parent.name != name:
                errors.append('directory/name mismatch: ' + name)
            front = re.match(r'\A---\nname: ([^\n]+)\ndescription: ([^\n]+)\n---\n', body)
            if not front or front[1] != name or not front[2].strip():
                errors.append('invalid frontmatter: ' + relative)
            if re.search(r'\b(?:TODO|FIXME|TBD)\b|\[INSERT[^\]]*\]', body):
                errors.append('unfinished scaffold: ' + relative)
            if re.search(r'[\u4e00-\u9fff]', body):
                errors.append('non-English skill body: ' + relative)
            if name in {'mcpp-usage', 'mcpp-contributing', 'mcpp-docs-style', 'mcpp-release'}:
                errors.append('name collides with existing skill: ' + name)
            check_indexes(root, path, name, relative, errors)
        except (OSError, KeyError, ValueError, TypeError) as exc:
            errors.append('skill entry: ' + str(exc))
    actual = {p.resolve() for p in (root / 'skills').rglob('SKILL.md')}
    if actual != expected_paths:
        errors.append('catalog/files mismatch: ' + str(sorted(str(x) for x in actual ^ expected_paths)))
    if (root / 'SKILL.md').exists():
        errors.append('suite root must not register a giant skill')
    ids = set()
    for entry in entries:
        try:
            if entry['id'] in ids:
                errors.append('duplicate interface: ' + entry['id'])
            ids.add(entry['id'])
            if entry['owner'] not in names:
                errors.append('unknown interface owner: ' + entry['id'])
            if entry['status'] == 'internal' and not entry.get('reason'):
                errors.append('internal interface lacks disposition: ' + entry['id'])
            target = inside(root, entry['reference'])
            if not target.is_file():
                errors.append('missing interface reference: ' + entry['id'])
        except (OSError, KeyError, ValueError, TypeError) as exc:
            errors.append('interface entry: ' + str(exc))
    # File links are essential to a copied leaf; root provenance links are HTTPS.
    for path in root.rglob('*.md'):
        body = path.read_text(encoding='utf-8')
        for link in re.findall(r'\]\(([^\s)]+)\)', body):
            if link.startswith(('https://', 'http://', 'mailto:', '#')):
                continue
            try:
                target = inside(root, str(path.parent.relative_to(root) / unquote(link.split('#')[0])))
                if not target.exists():
                    errors.append(f'{path.relative_to(root)}: broken link {link}')
                if path.is_relative_to(root / 'skills') and path.name != 'README.md' and path.name != 'README.zh-CN.md':
                    leaf = next((p for p in path.parents if (p / 'SKILL.md').is_file()), None)
                    if leaf and not target.is_relative_to(leaf):
                        errors.append(f'{path.relative_to(root)}: required local link leaves skill {link}')
            except ValueError as exc:
                errors.append(str(exc))
    if mcpp:
        try:
            current = {x['id']: x for x in extract(mcpp)}
            recorded = {x['id']: x for x in entries if x.get('extracted')}
            for missing in sorted(current.keys() - recorded.keys()):
                errors.append('new declaration needs owner/review: ' + missing)
            for removed in sorted(recorded.keys() - current.keys()):
                errors.append('removed declaration needs review: ' + removed)
            for key in current.keys() & recorded.keys():
                fields = ('detail', 'status', 'key', 'signature', 'takes_value',
                          'repeatable', 'short', 'required')
                if any(current[key].get(f) != recorded[key].get(f) for f in fields):
                    errors.append('changed declaration needs review: ' + key)
            for source in catalog['sources']:
                path = (mcpp / source['path']).resolve()
                if not path.is_relative_to(mcpp.resolve()) or not path.is_file():
                    errors.append('missing/escaping source: ' + source['path'])
                    continue
                digest = hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest()
                if digest != source['sha256']:
                    notices.append('source changed; semantic review required: ' + source['path'])
        except (OSError, ValueError, KeyError) as exc:
            errors.append('source extraction failed: ' + str(exc))
    return errors, notices


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suite-root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--mcpp-root', type=Path)
    args = parser.parse_args()
    errors, notices = check(args.suite_root.resolve(), args.mcpp_root.resolve() if args.mcpp_root else None)
    for item in notices:
        print('REVIEW: ' + item)
    for item in errors:
        print('FAIL: ' + item)
    if not errors:
        print('OK: structure' + (' and source declaration inventory' if args.mcpp_root else '') + ' checked; runtime semantics require execution')
    return int(bool(errors))


if __name__ == '__main__':
    sys.exit(main())
