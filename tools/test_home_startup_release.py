"""Offline receipt checks only; no process/event handles."""
import json
import unittest
from release_home_startup_barrier import validate_armed


class Tests(unittest.TestCase):
    def test_job_requires_version9(self):
        data = self.receipt('job')
        for version in (7, 8):
            with self.assertRaises(ValueError):
                validate_armed(data.replace(b'"diagnostic_version": 7', f'"diagnostic_version": {version}'.encode()), 123, 456, 'ab')
        data = data.replace(b'"diagnostic_version": 7', b'"diagnostic_version": 9')
        self.assertEqual(validate_armed(data, 123, 456, 'ab'), 'job')
    def test_route_version_and_phase_contract(self):
        data = self.receipt('route')
        with self.assertRaises(ValueError):
            validate_armed(data, 123, 456, 'ab')
        data = data.replace(b'"diagnostic_version": 7', b'"diagnostic_version": 8')
        self.assertEqual(validate_armed(data, 123, 456, 'ab'), 'route')
        with self.assertRaises(ValueError):
            validate_armed(data.replace(b'"phase": "route"', b'"phase": "particle0"'), 123, 456, 'ab')

    def receipt(self, mode='particle0', tail=None):
        rows = [dict(event='provenance', diagnostic_version=7, lifecycle_version=2,
                     pid=123, created_filetime=456, exe_sha256='ab', trace_mode=mode),
                dict(event='armed', phase=mode)]
        if tail:
            rows.append(tail)
        return b''.join(json.dumps(r).encode() + b'\n' for r in rows)

    def test_both_particle_modes(self):
        for mode in ('particle0', 'particle1'):
            self.assertEqual(validate_armed(self.receipt(mode), 123, 456, 'ab'), mode)

    def test_native_not_particle_coverage(self):
        with self.assertRaises(ValueError):
            validate_armed(self.receipt('native'), 123, 456, 'ab')

    def test_wrong_pid_generation_or_image(self):
        for pid, created, sha in [(124, 456, 'ab'), (123, 457, 'ab'), (123, 456, 'cd')]:
            with self.assertRaises(ValueError):
                validate_armed(self.receipt(), pid, created, sha)

    def test_closed_and_draining(self):
        for event in ('end', 'shutdown_begin', 'shutdown_released'):
            with self.assertRaises(ValueError):
                validate_armed(self.receipt(tail=dict(event=event)), 123, 456, 'ab')

    def test_truncated_armed_is_not_ready(self):
        with self.assertRaises(ValueError):
            validate_armed(self.receipt()[:-1], 123, 456, 'ab')

    def test_trailing_partial_non_authorization_ignored(self):
        self.assertEqual(validate_armed(self.receipt() + b'{', 123, 456, 'ab'), 'particle0')

    def test_duplicate_receipt_and_oversize_rejected(self):
        for data in (self.receipt() * 2, b' ' * 16777217):
            with self.assertRaises(ValueError):
                validate_armed(data, 123, 456, 'ab')


if __name__ == '__main__':
    unittest.main()
