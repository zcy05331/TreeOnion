import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_wisconsin_alpha_entries import run_stage


class StageProgressTests(unittest.TestCase):
    def test_failed_stage_is_preserved_and_next_stage_can_complete(self):
        def fail(folder):
            (folder/'driver.log').write_text('actual failure')
            raise RuntimeError('test failure')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'metadata').mkdir()
            state = {'status':'running','finished_stages':[],'stages':{}}
            run_stage(root, state, 'alpha_ab', fail)
            run_stage(root, state, 'entry16', lambda folder: {'result':'passed'})
            stored = json.loads((root/'metadata/progress.json').read_text())
            self.assertEqual(stored['finished_stages'], ['alpha_ab','entry16'])
            self.assertEqual(stored['stages']['alpha_ab']['status'], 'failed')
            self.assertEqual(stored['stages']['entry16']['status'], 'complete')
            self.assertEqual((root/'outputs/alpha_ab/driver.log').read_text(), 'actual failure')


if __name__ == '__main__':
    unittest.main()
