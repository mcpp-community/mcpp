#!/usr/bin/env python3
"""Seed the candidate xim checkout into a native admission home before init."""
import argparse
import json
from pathlib import Path
import re
import tomllib


def seed(home, checkout):
    checkout = checkout.resolve(strict=True)
    if not (checkout / 'pkgs/l/llvm.lua').is_file():
        raise ValueError('candidate checkout does not contain the LLVM recipe')
    home.mkdir(parents=True, exist_ok=True)
    config = home / 'config.toml'
    text = config.read_text() if config.exists() else ''
    tomllib.loads(text)
    match = re.search(r'(?m)^\[index\.repos\.(?:xim|"xim")\]\s*$', text)
    line = f'url = {json.dumps(str(checkout))}\n'
    if match:
        start = match.end()
        end_match = re.search(r'(?m)^\[', text[start:])
        end = start + end_match.start() if end_match else len(text)
        body = text[start:end]
        if re.search(r'(?m)^url\s*=', body):
            body = re.sub(r'(?m)^url\s*=.*(?:\n|$)', lambda _: line, body)
        else:
            body = '\n' + line + body.lstrip('\n')
        text = text[:start] + body + text[end:]
    else:
        text += '\n[index.repos.xim]\n' + line
    assert tomllib.loads(text)['index']['repos']['xim']['url'] == str(checkout)
    config.write_text(text)
    print(f'Native admission candidate: {checkout} -> {config}')


def verify(home, checkout):
    checkout = checkout.resolve(strict=True)
    registry = home / 'registry/.xlings.json'
    data = json.loads(registry.read_text())
    matches = [repo for repo in data.get('index_repos', []) if repo.get('name') == 'xim']
    if len(matches) != 1 or matches[0].get('url') != str(checkout):
        raise ValueError(f'candidate index is not effective in {registry}: {matches}')
    print(f'PASS: {registry} uses candidate xim {checkout}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('home', type=Path)
    parser.add_argument('checkout', type=Path)
    args = parser.parse_args()
    (verify if args.verify else seed)(args.home, args.checkout)
