#!/usr/bin/env python3
"""Validate a JSON document against a JSON Schema (draft 2020-12 subset).

Usage: _json_schema_subset.py <schema.json> <document.json>

Exit status: 0 when the document is valid, 1 when it is not (each violation is
printed with its JSON path), 3 when the schema uses a keyword this validator
does not implement. The last case is deliberate: a keyword that is silently
skipped is a rule that is silently not checked.
"""
import json
import re
import sys

ANNOTATIONS = {"$schema", "$id", "title", "description", "$comment", "examples", "default"}
IMPLEMENTED = {
    "$defs", "$ref", "type", "enum", "const", "required", "properties",
    "additionalProperties", "items", "minItems", "uniqueItems", "minLength",
    "minimum", "pattern", "oneOf", "not", "propertyNames",
}


def keywords(node, found):
    if isinstance(node, dict):
        for key, value in node.items():
            found.add(key)
            if key in ("properties", "$defs"):
                for sub in value.values():
                    keywords(sub, found)
            elif key in ("enum", "const", "required", "examples", "default"):
                continue
            else:
                keywords(value, found)
    elif isinstance(node, list):
        for item in node:
            keywords(item, found)


def type_ok(value, name):
    if name == "object":
        return isinstance(value, dict)
    if name == "array":
        return isinstance(value, list)
    if name == "string":
        return isinstance(value, str)
    if name == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if name == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if name == "boolean":
        return isinstance(value, bool)
    if name == "null":
        return value is None
    raise ValueError(f"unknown type {name}")


class Validator:
    def __init__(self, root):
        self.root = root

    def resolve(self, ref):
        if not ref.startswith("#/"):
            raise ValueError(f"only local references are supported: {ref}")
        node = self.root
        for part in ref[2:].split("/"):
            node = node[part]
        return node

    def check(self, schema, value, path, errors):
        if schema is True:
            return
        if schema is False:
            errors.append(f"{path}: not allowed")
            return
        if "$ref" in schema:
            self.check(self.resolve(schema["$ref"]), value, path, errors)
        if "type" in schema:
            names = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
            if not any(type_ok(value, n) for n in names):
                errors.append(f"{path}: expected {names}, got {type(value).__name__}")
                return
        if "const" in schema and value != schema["const"]:
            errors.append(f"{path}: expected {schema['const']!r}")
        if "enum" in schema and value not in schema["enum"]:
            errors.append(f"{path}: {value!r} is not one of {schema['enum']}")
        if isinstance(value, str):
            if "minLength" in schema and len(value) < schema["minLength"]:
                errors.append(f"{path}: shorter than {schema['minLength']}")
            if "pattern" in schema and not re.search(schema["pattern"], value):
                errors.append(f"{path}: does not match {schema['pattern']}")
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            if "minimum" in schema and value < schema["minimum"]:
                errors.append(f"{path}: less than {schema['minimum']}")
        if isinstance(value, list):
            if "minItems" in schema and len(value) < schema["minItems"]:
                errors.append(f"{path}: fewer than {schema['minItems']} items")
            if schema.get("uniqueItems"):
                seen = [json.dumps(v, sort_keys=True) for v in value]
                if len(seen) != len(set(seen)):
                    errors.append(f"{path}: items are not unique")
            if "items" in schema:
                for i, item in enumerate(value):
                    self.check(schema["items"], item, f"{path}[{i}]", errors)
        if isinstance(value, dict):
            for key in schema.get("required", []):
                if key not in value:
                    errors.append(f"{path}: missing required '{key}'")
            props = schema.get("properties", {})
            for key, item in value.items():
                if key in props:
                    self.check(props[key], item, f"{path}.{key}", errors)
                elif "additionalProperties" in schema:
                    self.check(schema["additionalProperties"], item, f"{path}.{key}", errors)
                if "propertyNames" in schema:
                    self.check(schema["propertyNames"], key, f"{path}.<{key}>", errors)
        if "oneOf" in schema:
            matched = 0
            for alternative in schema["oneOf"]:
                sub = []
                self.check(alternative, value, path, sub)
                if not sub:
                    matched += 1
            if matched != 1:
                errors.append(f"{path}: matches {matched} of the oneOf alternatives, not exactly one")
        if "not" in schema:
            sub = []
            self.check(schema["not"], value, path, sub)
            if not sub:
                errors.append(f"{path}: matches a schema it must not match")


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    with open(sys.argv[1], encoding="utf-8") as f:
        schema = json.load(f)
    with open(sys.argv[2], encoding="utf-8") as f:
        document = json.load(f)
    found = set()
    keywords(schema, found)
    unknown = sorted(found - IMPLEMENTED - ANNOTATIONS)
    if unknown:
        print(f"the schema uses keywords this validator does not implement: {unknown}")
        return 3
    errors = []
    Validator(schema).check(schema, document, "$", errors)
    for e in errors:
        print(e)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
