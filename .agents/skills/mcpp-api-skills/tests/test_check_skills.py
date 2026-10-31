"""Observable structural defects, detached-copy behavior and extractor fixtures."""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

SUITE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SUITE / 'tools'))
from check_skills import check, inside
from inventory import cli_entries, closing, uncomment


class SuiteChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name) / 'detached-suite'
        shutil.copytree(SUITE, self.root, ignore=shutil.ignore_patterns('__pycache__', '.validation'))
        self.data = json.loads((self.root / 'catalog.json').read_text(encoding='utf-8'))

    def tearDown(self):
        self.temp.cleanup()

    def write_catalog(self):
        (self.root / 'catalog.json').write_text(json.dumps(self.data), encoding='utf-8')

    def test_detached_copy_needs_no_checkout(self):
        self.assertEqual(check(self.root)[0], [])

    def test_duplicate_names_are_rejected(self):
        self.data['skills'].append(self.data['skills'][0].copy())
        self.write_catalog()
        self.assertTrue(any('duplicate skill' in e for e in check(self.root)[0]))

    def test_missing_reference_is_rejected(self):
        (self.root / self.data['interfaces'][0]['reference']).unlink()
        self.assertTrue(any('missing interface reference' in e for e in check(self.root)[0]))

    def test_unknown_owner_is_rejected(self):
        self.data['interfaces'][0]['owner'] = 'mcpp-does-not-exist'
        self.write_catalog()
        self.assertTrue(any('unknown interface owner' in e for e in check(self.root)[0]))

    def test_unregistered_leaf_is_rejected(self):
        path = self.root / 'skills/basics/mcpp-extra/SKILL.md'
        path.parent.mkdir()
        path.write_text('---\nname: mcpp-extra\ndescription: Extra\n---\n', encoding='utf-8')
        self.assertTrue(any('catalog/files mismatch' in e for e in check(self.root)[0]))

    def test_missing_chinese_category_index_is_rejected(self):
        (self.root / 'skills/basics/README.zh-CN.md').unlink()
        self.assertTrue(any('missing index' in e for e in check(self.root)[0]))

    def test_leaf_external_relative_dependency_is_rejected(self):
        path = self.root / self.data['skills'][0]['path']
        path.write_text(path.read_text(encoding='utf-8') + '\n[dependency](../../../README.md)\n', encoding='utf-8')
        self.assertTrue(any('leaves skill' in e for e in check(self.root)[0]))

    def test_catalog_path_escape_is_rejected(self):
        self.data['skills'][0]['path'] = '../outside/SKILL.md'
        self.write_catalog()
        self.assertTrue(any('leaves suite' in e for e in check(self.root)[0]))

    def test_empty_catalog_is_not_success(self):
        self.data['interfaces'] = []
        self.write_catalog()
        self.assertTrue(check(self.root)[0])

    def test_broken_link_is_rejected(self):
        path = self.root / 'README.md'
        path.write_text(path.read_text(encoding='utf-8') + '\n[missing](missing.md)\n', encoding='utf-8')
        self.assertTrue(any('broken link' in e for e in check(self.root)[0]))


class ExtractionChecks(unittest.TestCase):
    def test_nested_options_stay_with_their_command(self):
        source = '''app.subcommand(cl::App("build").option(cl::Option("profile").takes_value())
            .subcommand(cl::App("nested").option(cl::Option("output").takes_value())))'''
        rows = {r['id']: r for r in cli_entries(source)}
        self.assertIn('cli:build nested/--output', rows)
        self.assertNotIn('cli:build/--output', rows)
        self.assertTrue(rows['cli:build/--profile']['takes_value'])

    def test_comments_do_not_add_options(self):
        source = '''// .subcommand(cl::App("fake"))
        app.subcommand(cl::App("build").option(cl::Option("profile").takes_value()))'''
        self.assertFalse(any('fake' in row['id'] for row in cli_entries(source)))

    def test_positionals_and_aliases_preserve_call_shape(self):
        source = '''app.subcommand(cl::App("build")
            .arg(cl::Arg("product").required())
            .option(cl::Option("profile").short_name('p').takes_value().multiple()))'''
        rows = {r['id']: r for r in cli_entries(source)}
        self.assertTrue(rows['cli:build/arg:product']['required'])
        self.assertEqual(rows['cli:build/--profile']['short'], 'p')
        self.assertTrue(rows['cli:build/--profile']['repeatable'])

    def test_delimiters_in_strings_are_ignored(self):
        self.assertEqual(closing('( "(" )', 0), 6)

    def test_empty_recognition_fails(self):
        with self.assertRaises(ValueError):
            cli_entries('// no declarations')

    def test_unbalanced_recognition_fails(self):
        with self.assertRaises(ValueError):
            closing('(broken', 0)


if __name__ == '__main__':
    unittest.main()
