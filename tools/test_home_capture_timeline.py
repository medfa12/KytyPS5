"""Disposable-file controls; no emulator, live logs or process access."""
import json
from pathlib import Path
import tempfile
import unittest
from home_capture_timeline import analyze, sample, classify_line


class Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.log = Path(self.temp.name) / 'host.log'
        self.trace = Path(self.temp.name) / 'trace.jsonl'
        self.home = b'[SceShellCore] I/RNPS.rnps-home.NPXS40002 : instance promoted from private-value\n'
        self.mount = b'[SceShellCore] I/RNPS.rnps-home.NPXS40002 : MountApp runApplication\n'

    def write_trace(self, anchors=None):
        rows = [dict(event='provenance'), dict(event='armed'), dict(event='shutdown_begin'), dict(event='end', cleanup_verified=True)]
        if anchors is not None:
            for row, anchor in zip(rows, anchors):
                row['timeline'] = anchor
        self.trace.write_text(''.join(json.dumps(r) + '\n' for r in rows))

    def fixture(self):
        self.log.write_bytes(self.home)
        before = sample(self.log)
        self.log.write_bytes(self.home + b'unrelated\n')
        armed = sample(self.log)
        with self.log.open('ab') as f:
            f.write(self.mount)
        end = sample(self.log)
        self.write_trace([before, armed, end, end])
        return [before, armed, end, end]

    def test_before_during_after_and_no_payload(self):
        self.fixture()
        with self.log.open('ab') as f:
            f.write(self.home)
        report = analyze(self.trace, self.log)
        self.assertEqual([x['placement'] for x in report['milestones']],
                         ['persisted_before_armed', 'persisted_during_capture', 'persisted_after_capture'])
        self.assertNotIn('private-value', json.dumps(report))

    def test_old_capture_remains_unknown(self):
        self.log.write_bytes(self.home)
        self.write_trace()
        self.assertEqual(analyze(self.trace, self.log)['milestones'][0]['placement'], 'alignment_unknown')

    def test_drain_is_not_active_capture(self):
        anchors = self.fixture()
        with self.log.open('ab') as f:
            f.write(self.home)
        anchors[-1] = sample(self.log)
        self.write_trace(anchors)
        self.assertEqual(analyze(self.trace, self.log)['milestones'][-1]['placement'], 'persisted_after_capture')

    def test_mutation_rejected(self):
        self.fixture()
        self.log.write_bytes(b'changed' + self.log.read_bytes()[7:])
        with self.assertRaisesRegex(ValueError, 'mismatch'):
            analyze(self.trace, self.log)

    def test_truncation_rejected(self):
        self.fixture()
        self.log.write_bytes(b'')
        with self.assertRaises(ValueError):
            analyze(self.trace, self.log)

    def test_rotation_rejected_even_if_bytes_match(self):
        anchors = self.fixture()
        anchors[2]['file_identity'][1] += 1
        self.write_trace(anchors)
        with self.assertRaisesRegex(ValueError, 'file_replaced'):
            analyze(self.trace, self.log)

    def test_missing_file_nonfatal_sample(self):
        self.assertEqual(sample(self.log)['status'], 'unavailable')

    def test_unavailable_anchor_is_not_alignment(self):
        self.log.write_bytes(self.home)
        self.write_trace([sample(self.log), dict(status='unavailable'), sample(self.log), sample(self.log)])
        with self.assertRaisesRegex(ValueError, 'unavailable'):
            analyze(self.trace, self.log)

    def test_partial_capture_rejected(self):
        self.trace.write_text('{"event":"provenance"}\n')
        with self.assertRaisesRegex(ValueError, 'closed_capture'):
            analyze(self.trace, self.log)

    def test_straddled_line_unknown(self):
        self.assertEqual(classify_line(5, 15, 10, 20), 'straddles_sample_boundary')
        self.assertEqual(classify_line(15, 25, 10, 20), 'straddles_sample_boundary')

    def test_sample_bounded_for_large_log(self):
        self.log.write_bytes(b'x' * 100000)
        anchor = sample(self.log)
        self.assertEqual(anchor['head_bytes'], 4096)
        self.assertEqual(anchor['offset'] - anchor['tail_offset'], 4096)
        self.assertLess(len(json.dumps(anchor)), 600)


if __name__ == '__main__':
    unittest.main()
