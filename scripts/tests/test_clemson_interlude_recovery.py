import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'run_treepir_clemson_interlude.sh'


class ClemsonInterludeRecoveryTest(unittest.TestCase):
    def test_failed_generic_does_not_block_independent_work_or_parent_resume(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            root = base / 'treepir-backend-tuning-20260905'
            (root / 'deps/TreePIR/.git').mkdir(parents=True)
            (base / 'outputs/profiling/8gb').mkdir(parents=True)
            (base / 'outputs/profiling/8gb/generic-process.json').write_text(json.dumps({'exit_code': 1}))
            fake = base / 'bin'
            fake.mkdir()
            py = fake / 'python3'
            py.write_text('#!' + sys.executable + '\n' + '''import subprocess,sys
if len(sys.argv)>1 and sys.argv[1].endswith('.py'):
    raise SystemExit('failed Generic output must not be validated as success')
code=sys.stdin.read()
if 'generic-process.json' in code:
    raise SystemExit(subprocess.run([sys.executable,'-c',code,*sys.argv[2:]]).returncode)
# Simulated /proc: predecessor exited; original parent remains stopped.
''')
            py.chmod(0o755)
            cmake = fake / 'cmake'
            cmake.write_text('#!/bin/sh\ntouch "$BUILD_STARTED"\nexit 17\n')
            cmake.chmod(0o755)
            env = dict(os.environ, PATH=str(fake) + os.pathsep + os.environ['PATH'],
                       BUILD_STARTED=str(base/'build-started'), SIGNALS=str(base/'signals'))
            result = subprocess.run(['bash','-c',
                'kill() { printf "%s\\n" "$*" >> "$SIGNALS"; }; export -f kill; exec bash "$@"',
                'recovery-test',str(SCRIPT),str(base),'123456','654321'],
                env=env,text=True,capture_output=True)
            self.assertEqual(result.returncode,17,result.stderr)
            self.assertTrue((base/'build-started').exists())
            state=json.loads((root/'metadata/generic-prerequisite.json').read_text())
            self.assertEqual(state['exit_code'],1)
            self.assertEqual(state['formal_result_status'],'failed')
            self.assertEqual((base/'signals').read_text().splitlines(),['-CONT 654321'])
            self.assertEqual((root/'metadata/queue.status').read_text().strip(),'failed_parent_resumed')


if __name__ == '__main__':
    unittest.main()
