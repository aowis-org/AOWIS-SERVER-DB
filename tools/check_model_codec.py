#!/usr/bin/env python3
"""Verify that every persisted NetworkHydraulic field matches the pinned V5 codec.

This does not change the binary encoding. Changes to the Model schema require
an explicit codec-version evolution and a new field manifest, not regeneration
of the existing V5 format without backward compatibility.
"""
import json
import pathlib
import re
import sys


def inspect(root):
    files = [root / 'entity.h', root / 'gis.h', root / 'measurement/water_meter.h']
    files.extend(sorted((root / 'hydraulic').glob('*.h')))
    models = {}
    for path in files:
        data = path.read_text(encoding='utf-8')
        data = re.sub(r'/\*.*?\*/', '', data, flags=re.S)
        data = re.sub(r'//[^\n]*', '', data)
        for found in re.finditer(r'\bstruct\s+(\w+)\s*\{', data):
            depth = 1
            index = found.end()
            while depth:
                if index >= len(data):
                    raise ValueError(f'unterminated {found.group(1)}')
                if data[index] == '{':
                    depth += 1
                if data[index] == '}':
                    depth -= 1
                index += 1
            fields = []
            for line in data[found.end():index-1].splitlines():
                line = line.strip()
                declaration = re.fullmatch(r'(.*?)\s+(\w+)\s*(?:=\s*[^;]+)?;', line)
                if declaration and '(' not in declaration.group(1):
                    fields.append([declaration.group(1), declaration.group(2)])
            models[found.group(1)] = fields
    aliases = {'HydraulicEntityMetadata': 'EntityMetadata'}
    reached = {}

    def visit(type_name):
        type_name = aliases.get(type_name, type_name)
        if type_name in reached:
            return
        if type_name not in models:
            return
        reached[type_name] = models[type_name]
        for field_type, _ in models[type_name]:
            while field_type.startswith('QList<') or field_type.startswith('std::optional<'):
                field_type = field_type[field_type.index('<')+1:-1]
            visit(field_type)
    visit('NetworkHydraulic')
    return dict(sorted(reached.items()))


def main():
    if len(sys.argv) != 4:
        print('Usage: check_model_codec.py MODEL_INCLUDE_ROOT MANIFEST_JSON CODEC_CPP', file=sys.stderr)
        return 2
    actual = inspect(pathlib.Path(sys.argv[1]))
    expected = json.loads(pathlib.Path(sys.argv[2]).read_text(encoding='utf-8'))
    if actual != expected:
        print('AOWIS Model layout has changed since the V5 serialization was defined.', file=sys.stderr)
        print('Review the fields, add a new codec format version with old-version readers,', file=sys.stderr)
        print('then update the field manifest after approving the migration.', file=sys.stderr)
        for key in sorted(actual.keys() | expected.keys()):
            if actual.get(key) != expected.get(key):
                print(' Changed struct:', key, file=sys.stderr)
                print('   stored:', expected.get(key), file=sys.stderr)
                print('   model: ', actual.get(key), file=sys.stderr)
        return 1
    codec = pathlib.Path(sys.argv[3]).read_text(encoding='utf-8')
    for type_name, fields in expected.items():
        for operation in ('write', 'read'):
            prefix = 'const ' if operation == 'write' else ''
            pattern = (r'void ' + operation + r'Struct\(QDataStream &stream, '
                       + prefix + re.escape(type_name) + r' &value\)\s*\{(.*?)\n\}')
            matches = re.findall(pattern, codec, flags=re.S)
            member_references = (re.findall(r'(?:writeValue|readValue)\(stream, value\.(\w+)\);', matches[0])
                                 if len(matches) == 1 else [])
            expected_names = [name for _, name in fields]
            if member_references != expected_names:
                print(f'V5 {operation} codec for {type_name} does not match the Model fields!',
                      file=sys.stderr)
                print('  Expected:', expected_names, file=sys.stderr)
                print('  Found:', member_references, file=sys.stderr)
                return 1
    print(f'AOWIS V5 codec matches {len(actual)} persisted Model structures.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
