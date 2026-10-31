"""Extract review candidates from mcpp's declarations, not from its manuals.

This is deliberately a bounded recognizer, not a C++ parser. An unrecognized
required table fails closed. Parser-reader candidates still require human
scope/type/consumer review; extraction does not establish effective behavior.
"""
from __future__ import annotations

import json
import re
from pathlib import Path

TOKEN = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')


def uncomment(text: str) -> str:
    return TOKEN.sub(lambda m: ' ' * len(m[0]) if m[0].startswith(('//', '/*')) else m[0], text)


def closing(text: str, start: int, left: str = '(', right: str = ')') -> int:
    """Find a balanced expression while ignoring delimiters in string literals."""
    depth, i = 0, start
    while i < len(text):
        if text[i] in ('"', "'"):
            quote = text[i]
            i += 1
            while i < len(text):
                if text[i] == '\\':
                    i += 2
                    continue
                if text[i] == quote:
                    break
                i += 1
        elif text[i] == left:
            depth += 1
        elif text[i] == right:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError('unbalanced source expression')


def literal(text: str, method: str) -> str:
    match = re.search(r'\.' + method + r'\(\s*((?:"(?:\\.|[^"\\])*"\s*)+)', text)
    if not match:
        return ''
    return ''.join(json.loads(s) for s in re.findall(r'"(?:\\.|[^"\\])*"', match[1]))


def cli_entries(text: str) -> list[dict]:
    text = uncomment(text)
    pattern = re.compile(r'\.subcommand\(cl::App\("([^"]+)"\)')
    internal = {'dyndep', 'link-output', 'platform-step', 'place-dlls', 'coff-def',
                'bmi-equal', 'bmi-compile', 'bmi-supervise', 'bmi-await'}
    result = []

    def walk(fragment: str, parent: str = '') -> str:
        cursor, pieces = 0, []
        while match := pattern.search(fragment, cursor):
            end = closing(fragment, fragment.index('(', match.start()))
            name = (parent + ' ' + match[1]).strip()
            child = fragment[match.end():end]
            own = walk(child, name)
            status = 'internal' if name.split()[0] in internal else 'public'
            if name == 'stage':
                status = 'action-helper'
            result.append(dict(id='cli:' + name, family='cli', key=name, status=status,
                               source='src/cli.cppm', detail=literal(own, 'description')))
            for opt in re.finditer(r'\.option\(cl::Option\("([^"]+)"\)', own):
                opt_end = closing(own, own.index('(', opt.start()))
                declaration = own[opt.start():opt_end + 1]
                short = re.search(r"\.short_name\('([^']+)'\)", declaration)
                result.append(dict(id='cli:' + name + '/--' + opt[1], family='cli',
                                   key=name + ' --' + opt[1], status=status,
                                   source='src/cli.cppm', detail=literal(declaration, 'help'),
                                   takes_value='.takes_value()' in declaration,
                                   repeatable='.multiple()' in declaration,
                                   short=short[1] if short else ''))
            for arg in re.finditer(r'\.arg\(cl::Arg\("([^"]+)"\)', own):
                arg_end = closing(own, own.index('(', arg.start()))
                declaration = own[arg.start():arg_end + 1]
                result.append(dict(id='cli:' + name + '/arg:' + arg[1], family='cli',
                                   key=name + ' <' + arg[1] + '>', status=status,
                                   source='src/cli.cppm', detail=literal(declaration, 'help'),
                                   required='.required()' in declaration))
            pieces.extend([fragment[cursor:match.start()], ' ' * (end + 1 - match.start())])
            cursor = end + 1
        pieces.append(fragment[cursor:])
        return ''.join(pieces)

    top = walk(text)
    # Global registered flags occur outside child App expressions. Preparse-only
    # switches are also explicitly inventoried below by the reviewer.
    for match in re.finditer(r'\.option\(cl::Option\("([^"]+)"\)', top):
        end = closing(top, top.index('(', match.start()))
        declaration = top[match.start():end + 1]
        result.append(dict(id='cli:global/--' + match[1], family='cli',
                           key='global --' + match[1], status='public', source='src/cli.cppm',
                           detail=literal(declaration, 'help'), takes_value='.takes_value()' in declaration))
    if not any(x['id'] == 'cli:build/--profile' for x in result):
        raise ValueError('CLI recognizer did not find build/profile')
    return result


def extract(root: Path) -> list[dict]:
    def read(path):
        return (root / path).read_text(encoding='utf-8')

    rows = cli_entries(read('src/cli.cppm'))
    path = 'modules/manifest/src/key_registry.cppm'
    registry = re.findall(r'\{"([^"]+)",\s*"([^"]*)",\s*KeyScope::(\w+),\s*KeyMerge::(\w+),\s*(true|false),\s*(true|false)\s*\}', uncomment(read(path)))
    if not registry:
        raise ValueError('empty manifest key registry extraction')
    for table, key, scope, merge, shared, conditional in registry:
        full = table + ('.' + key if key else '')
        rows.append(dict(id='manifest:' + full, family='manifest', key=full, source=path,
                         detail=f'scope={scope}; merge={merge}; shared={shared}; conditional={conditional}',
                         status='review-candidate'))
    path = 'modules/manifest/src/toml.cppm'
    parser = uncomment(read(path))
    for kind, key in re.findall(r'->get_(string_array|string|bool|int|table)\("([^"]+)"\)', parser):
        rows.append(dict(id='manifest:' + key, family='manifest', key=key, source=path,
                         detail='parser reader: ' + kind, status='review-candidate'))
    scopes = {'Feature': 'features.<name>', 'CAbi': 'c-abi', 'KernelAbi': 'kernel-abi',
              'Package': 'package', 'Target': 'targets.<name>', 'Runtime': 'runtime',
              'Resource': 'resources', 'VersionInfo': 'resources.version-info',
              'Profile': 'profile.<name>', 'Pack': 'pack', 'Toolchain': 'toolchain'}
    for group, body in re.findall(r'kKnown(\w+)Keys\[\]\s*=\s*\{([^}]+)\}', parser):
        scope = scopes.get(group, 'parser.' + group)
        for key in re.findall(r'"([^"]+)"', body):
            full = scope + '.' + key
            rows.append(dict(id='manifest:' + full, family='manifest', key=full, source=path,
                             detail='accepted-key table: ' + group, status='review-candidate'))
    # Platform reader dispatch is function-scoped. These are reader fields, not
    # a flattened claim that every key is valid directly in [platform].
    path = 'modules/manifest/src/platform_toml.cppm'
    platform = uncomment(read(path))
    funcs = list(re.finditer(r'\bReader::(\w+)\([^;]*?\)\s*const\s*\{', platform))
    for func in funcs:
        begin = func.end() - 1
        body = platform[begin:closing(platform, begin, '{', '}')]
        for key in sorted(set(re.findall(r'\bk\s*==\s*"([^"]+)"', body))):
            full = 'platform-reader.' + func[1] + '.' + key
            rows.append(dict(id='manifest:' + full, family='manifest', key=full, source=path,
                             detail='Reader::' + func[1] + '; context-sensitive field', status='review-candidate'))
    if not any(x['key'].startswith('platform-reader.statement.') for x in rows):
        raise ValueError('platform statement recognition failed')
    path = 'src/build/hostprogram.cppm'
    host = read(path)
    start = host.index('kMcppModuleSource = R"CPP(') + len('kMcppModuleSource = R"CPP(')
    embedded = uncomment(host[start:host.index(')CPP"', start)])
    for match in re.finditer(r'\binline\s+([^;{}\n]+?\([^\n{}]*\))\s*\{', embedded):
        signature = match[1].strip()
        name = re.search(r'(\w+)\s*\(', signature)[1]
        rows.append(dict(id='api:' + signature, family='api', key='mcpp::' + name,
                         signature=signature, source=path,
                         status='internal' if name.endswith('_') or name == 'env_or' else 'public',
                         detail='bundled public module; wrapper signature, not an internal engine export'))
    action = embedded[embedded.index('struct action {'):]
    action = action[:action.index('private:')]
    for match in re.finditer(r'(action&|void)\s+(\w+)\(([^)]*)\)\s*(?:const\s*)?\{', action):
        signature = match[1] + ' ' + match[2] + '(' + match[3] + ')'
        rows.append(dict(id='api:action::' + signature, family='api', key='mcpp::action::' + match[2],
                         signature=signature, source=path, status='public', detail='action public method'))
    for key in ('action', 'diagnostic', 'roles::source', 'roles::check', 'roles::object', 'roles::artifact', 'roles::prepare'):
        rows.append(dict(id='api:type:' + key, family='api', key='mcpp::' + key,
                         source=path, status='public', detail='public type or role constant'))
    for typename in ('action', 'diagnostic'):
        section = embedded[embedded.index('struct ' + typename + ' {'):]
        section = section[:section.index('private:')] if typename == 'action' else section[:section.index('};')]
        for field_type, name, value in re.findall(r'^\s*(const char\*|bool)\s+(\w+)\s*=\s*([^;\n]+);', section, re.M):
            rows.append(dict(id='api:field:' + typename + '::' + name, family='api',
                             key='mcpp::' + typename + '::' + name, source=path,
                             status='public', detail=f'{field_type}; default={value.strip()}'))
    path = 'modules/buildmcpp/src/directives.cppm'
    directives = uncomment(read(path))
    pattern = r'\{"([^"]+)",\s*"([^"]*)",\s*Slot::(\w+),\s*Scope::(\w+),\s*Transform::(\w+),\s*(true|false),\s*"([^"]*)",\s*"([^"]*)",\s*(\d+)\}'
    defs = re.findall(pattern, directives)
    declared = re.search(r'std::array<Def,\s*(\d+)>\s+kTable', directives)
    if not declared or len(defs) != int(declared[1]):
        raise ValueError('directive table recognition is incomplete')
    for wire, tag, slot, scope, transform, required, prefix, suffix, since in defs:
        rows.append(dict(id='directive:' + wire, family='directive', key='mcpp:' + wire,
                         source=path, status='public', detail=f'scope={scope}; transform={transform}; since protocol={since}; requires output={required}'))
    path = 'src/wire.cppm'
    wire = uncomment(read(path))
    kinds = re.findall(r'\{"(mcpp\.[^"]+)",\s*(\d+)\}', wire)
    declared = re.search(r'std::array<KindVersion,\s*(\d+)>\s+kKinds', wire)
    if not declared or len(kinds) != int(declared[1]):
        raise ValueError('wire kind table recognition is incomplete')
    for kind, version in kinds:
        rows.append(dict(id='wire:' + kind, family='wire', key=kind, source=path,
                         status='public', detail='data version=' + version))
    path = 'modules/source-kind/src/source_kind.cppm'
    source = uncomment(read(path))
    sets = re.findall(r'k(Cxx|C|Gas|Nasm|Header|Device|DeviceHeader)Extensions\[\]\s*=\s*\{([^}]+)\}', source)
    if len(sets) != 7 or '.moduleInterface = { ".cppm" }' not in source:
        raise ValueError('source role table recognition is incomplete')
    for kind, values in sets + [('ModuleInterface', '".cppm"')]:
        for ext in re.findall(r'"(\.[^"]+)"', values):
            rows.append(dict(id='source-kind:' + ext, family='source-kind', key=ext,
                             source=path, status='public', detail='source role=' + kind))
    # Environment identifiers are an audit/navigation surface, not a promise
    # that internal knobs should be set by consumers.
    env_sources = ('src/cli.cppm', 'src/config.cppm', 'src/effects.cppm', 'src/build/build_program.cppm', 'src/build/hostprogram.cppm')
    for path in env_sources:
        for name in sorted(set(re.findall(r'"(MCPP_[A-Z0-9_]+)"', uncomment(read(path))))):
            rows.append(dict(id='env:' + name, family='env', key=name, source=path,
                             status='review-candidate', detail='source identifier; inspect caller before setting'))
    unique = {}
    for row in rows:
        if row['id'] in unique:
            old = unique[row['id']]
            old['detail'] += ' / ' + row['detail'] if row['detail'] not in old['detail'] else ''
            old['sources'] = sorted(set(old.get('sources', [old['source']]) + [row['source']]))
        else:
            unique[row['id']] = row
    if not any(x['family'] == 'api' for x in unique.values()):
        raise ValueError('empty API extraction')
    return sorted(unique.values(), key=lambda row: row['id'])
