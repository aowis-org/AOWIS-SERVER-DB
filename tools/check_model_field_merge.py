#!/usr/bin/env python3
"""Ensure field-wise merge has exactly the fields in the pinned Model codec."""
import json
import pathlib
import re
import sys

schema = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
source = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
header = pathlib.Path(sys.argv[3]).read_text(encoding="utf-8")
entities = re.findall(r'template<> struct HydraulicObjectKind<(\w+)>', header)
errors = []
for typ in entities:
    pattern = (rf'template<> std::optional<ModelFieldMerge> mergeFieldsForType<{typ}>\('
               rf'(.*?)\n\}}')
    match = re.search(pattern, source, re.S)
    if not match:
        errors.append(f"No merge specialization for {typ}")
        continue
    found = re.findall(rf'&{typ}::(\w+), "(\w+)"', match.group(1))
    expected = [name for _, name in schema[typ]]
    if found != [(name, name) for name in expected]:
        errors.append(f"Mismatch in {typ} fields: expected {expected}, got {found}")
    # A merge that omits field choices would silently discard explicit
    # resolutions while still satisfying the serialization schema check.
    if any(not line.endswith('result.conflicting_fields, field_choices);')
           for line in match.group(1).splitlines()
           if 'chooseMergeField(' in line):
        errors.append(f"{typ} does not forward field choices")
    if f'return mergeFieldsForType<{typ}>(object_uuid, base, left, right, field_choices);' not in source:
        errors.append(f"Missing kind dispatcher for {typ}")
# Nested metadata must not silently lose new fields added to Model/codec.
# Verify its explicit merge path against the canonical EntityMetadata schema.
metadata_block = re.search(
    r'if constexpr \(std::is_same_v<Member, EntityMetadata>\)(.*?)\n    else\n',
    source, re.S)
if not metadata_block:
    errors.append("Missing nested EntityMetadata merge")
else:
    metadata_names = re.findall(
        r'&EntityMetadata::(\w+), "metadata\.(\w+)"', metadata_block.group(1))
    expected_metadata = [name for _, name in schema['EntityMetadata']]
    if metadata_names != [(name, name) for name in expected_metadata]:
        errors.append(f"EntityMetadata fields out of sync: expected {expected_metadata}, found {metadata_names}")
if errors:
    sys.exit("\n".join(errors))
print(f"Field-wise merge schema synchronized: {len(entities)} entity kinds; "
      f"{len(schema['EntityMetadata'])} nested metadata fields")
