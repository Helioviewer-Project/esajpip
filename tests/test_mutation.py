#!/usr/bin/env python3
"""Mutation CLI and runner regressions. No C build is needed for these tests."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

TESTS = Path(__file__).resolve().parent


class MutationTests(unittest.TestCase):
    def test_cli_operator_ids(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, output = Path(tmp) / 'input.c', Path(tmp) / 'mutant.c'
            original = 'int f(int x) { return x < 2 && !x; }\n'
            source.write_text(original)
            cases = [
                ('relational', ['<'], ['<=']),
                ('logical', ['&&'], ['||']),
                ('negation', ['!'], ['']),
                ('constant', ['2'], ['3']),
                ('rule,relational,logical,negation', ['<', '&&', '!'], ['<=', '||', '']),
                ('rule,relational,logical,negation,constant', ['<', '2', '&&', '!'], ['<=', '3', '||', '']),
            ]
            for operators, old, new in cases:
                command = [sys.executable, str(TESTS / 'mutate.py')]
                rows = subprocess.check_output(command + ['list', str(source), operators], text=True).splitlines()
                self.assertEqual(len(rows), len(old))
                for i, (before, after) in enumerate(zip(old, new)):
                    self.assertEqual(rows[i].split('\t')[0], str(i))
                    subprocess.run(command + ['apply', str(source), str(i), str(output), operators], check=True)
                    self.assertEqual(output.read_text(), original.replace(before, after, 1))
                self.assertEqual(source.read_text(), original)

    def test_checkout_edit_during_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            repo, work, commands = root / 'repo', root / 'work', root / 'commands'
            (repo / 'tests').mkdir(parents=True)
            commands.mkdir()
            for name in ('run_mutation.sh', 'run_common.sh', 'mutate.py'):
                shutil.copyfile(TESTS / name, repo / 'tests' / name)
            original = 'return "one";\nreturn "two";\nreturn "three";\n'
            live = repo / 'rules.c'
            live.write_text(original)
            # Capture each built source, and edit the live checkout after the
            # list was made. The listed final ID must remain applicable.
            cmake = commands / 'cmake'
            cmake.write_text('''#!''' + sys.executable + '''
import os
from pathlib import Path
import sys
work = Path(os.environ['ESAJPIP_TEST_BUILD_DIR'])
if sys.argv[1] == '--build':
    count_file = work / 'build-count'
    count = int(count_file.read_text()) if count_file.exists() else 0
    (work / ('built-%d.c' % count)).write_bytes((work / 'src/rules.c').read_bytes())
    count_file.write_text(str(count + 1))
    if count == 1:
        Path(os.environ['LIVE_SOURCE']).write_text('return "edited";\\n')
''')
            cmake.chmod(0o755)
            ctest = commands / 'ctest'
            ctest.write_text('#!/bin/sh\ncase "$*" in *missing*)\n'
                             '  case "$*" in *--no-tests=error*) exit 8;; esac\n'
                             'esac\nexit 0\n')
            ctest.chmod(0o755)
            env = {k: v for k, v in os.environ.items() if not k.startswith('ESAJPIP_')}
            env.update(PATH=str(commands) + os.pathsep + os.environ['PATH'],
                       PYTHON=sys.executable, ESAJPIP_TEST_BUILD_DIR=str(work),
                       ESAJPIP_MUTATION_OPERATORS='rule', LIVE_SOURCE=str(live))
            result = subprocess.run(['sh', str(repo / 'tests/run_mutation.sh'), 'rules.c'],
                                    env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            rows = (work / 'report.tsv').read_text().splitlines()
            self.assertEqual([r.split('\t')[1] for r in rows], ['0', '1', '2'])
            self.assertTrue(all(r.endswith('\tsurvived') for r in rows))
            expected = [original] + [original.replace('"' + word + '"', 'NULL')
                                     for word in ('one', 'two', 'three')] + [original]
            self.assertEqual(int((work / 'build-count').read_text()), len(expected))
            for i, text in enumerate(expected):
                self.assertEqual((work / ('built-%d.c' % i)).read_text(), text)
            self.assertEqual((work / 'src/rules.c').read_text(), original)
            self.assertEqual((work / 'original/rules.c').read_text(), original)
            self.assertEqual(live.read_text(), 'return "edited";\n')
            env['ESAJPIP_MUTATION_TESTS'] = '-R missing'
            result = subprocess.run(['sh', str(repo / 'tests/run_mutation.sh'), 'rules.c'],
                                    env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('unmutated sources fail the tests', result.stderr)


if __name__ == '__main__':
    unittest.main()
