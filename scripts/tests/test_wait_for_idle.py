import tempfile
from pathlib import Path
import unittest
from scripts.wait_for_idle import busy_jobs, maximum_cpu_busy


class IdleGateTests(unittest.TestCase):
    def test_paused_jobs_and_own_queue_are_not_treated_as_foreign_work(self):
        with tempfile.TemporaryDirectory() as directory:
            proc = Path(directory)
            for pid, state, command in ((1, 'T', b'/other/Onion-PIR'),
                                         (2, 'S', b'python3\0/ours/run_primary_matrix.py'),
                                         (3, 'R', b'/other/Onion-PIR')):
                d = proc / str(pid)
                d.mkdir()
                (d / 'stat').write_text(f'{pid} (Onion-PIR) {state} 1')
                (d / 'comm').write_text('Onion-PIR')
                (d / 'cmdline').write_bytes(command)
            self.assertEqual(busy_jobs(Path('/ours'), proc),
                             [{'pid': 3, 'name': 'Onion-PIR', 'state': 'R'}])

    def test_unknown_job_can_still_block_by_cpu_activity(self):
        before = {'cpu0': [0] * 8, 'cpu1': [0] * 8}
        after = {'cpu0': [100, 0, 0, 0, 0, 0, 0, 0],
                 'cpu1': [0, 0, 0, 100, 0, 0, 0, 0]}
        self.assertEqual(maximum_cpu_busy(before, after), 1.0)

    def test_idle_and_io_wait_are_not_compute_activity(self):
        self.assertEqual(maximum_cpu_busy({'cpu0': [0] * 8},
                         {'cpu0': [0, 0, 0, 90, 10, 0, 0, 0]}), 0.0)


if __name__ == '__main__':
    unittest.main()
