"""Production profile acceptance, paired semantics and atomic retention contracts."""

import copy
import json
from pathlib import Path
import tempfile
import unittest

from tools.bench import parse_arguments
from tools.production_cases import container_cases
from tools.production_json_cases import json_cases
from tools.production_report import (ALLOCATION_FIELDS, LATENCY_FIELDS, profile_names,
                                      promote, render, summarize)


def fixture():
    selected = [f'production/{backend}/containers/filter/64/real_time' for backend in ('cpp', 'aot')]
    throughput = dict(benchmarks=[])
    profiles = {}
    for name in selected:
        throughput['benchmarks'].append(dict(name=name, real_time=100, time_unit='ns',
                                             result=42, expected_result=42))
        profiles[name] = {}
        for profile, fields in (('latency', LATENCY_FIELDS), ('allocations', ALLOCATION_FIELDS)):
            row = {field: 0 for field in fields}
            row.update(name=profile_names(name, profile, 4), real_time=200, time_unit='ns',
                       iterations=4, result=42, expected_result=42)
            if profile == 'latency':
                row.update(p50_ns=100, p95_ns=200, p99_ns=300, maximum_ns=400,
                           latency_samples=4, process_resident_bytes=100,
                           process_peak_resident_bytes=110)
            else:
                row.update(remaining_saved_bytes=0, remaining_suspended_frames=0)
            profiles[name][profile] = dict(samples=4, report=dict(benchmarks=[row]))
    return selected, throughput, profiles


class ProductionReportTest(unittest.TestCase):
    def test_checked_profiles_are_separate_from_throughput(self):
        selected, throughput, profiles = fixture()
        result = summarize(throughput, selected, profiles, 1)
        self.assertEqual(result['cases'][selected[0]]['median_ns'], 100)
        self.assertEqual(result['cases'][selected[0]]['latency']['p99_ns'], 300)
        self.assertIn('AOT/C++', render(result))

    def test_rejects_missing_pair_partial_samples_wrong_checksum_and_retention(self):
        selected, throughput, profiles = fixture()
        invalid = copy.deepcopy(throughput)
        invalid['benchmarks'].pop()
        with self.assertRaises(ValueError):
            summarize(invalid, selected, profiles, 1)
        for field, value in [('latency_samples', 3), ('p95_ns', 999), ('result', 0)]:
            broken = copy.deepcopy(profiles)
            broken[selected[0]]['latency']['report']['benchmarks'][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                summarize(throughput, selected, broken, 1)
        broken = copy.deepcopy(profiles)
        broken[selected[0]]['allocations']['report']['benchmarks'][0]['retained_bytes'] = 1
        with self.assertRaises(ValueError):
            summarize(throughput, selected, broken, 1)

    def test_atomic_retention_preserves_only_two_entries_and_failure_preserves_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'rolling.json'
            path.write_text(json.dumps(dict(previous={}, latest=dict(suite='old'))), encoding='utf-8')
            baseline = path.read_bytes()
            with self.assertRaises(ValueError):
                promote(dict(all_expected_cases_passed=False), path)
            self.assertEqual(path.read_bytes(), baseline)
            promote(dict(all_expected_cases_passed=True, suite='new'), path)
            result = json.loads(path.read_text(encoding='utf-8'))
            self.assertEqual(set(result), {'previous', 'latest'})
            self.assertEqual(result['previous']['suite'], 'old')
            self.assertFalse(result['latest']['comparison']['comparable'])

    def test_cli_requires_release_and_positive_counts(self):
        parsed = parse_arguments(['--production-compare', '--profile-samples=4'])
        self.assertEqual(parsed.report_mode, 'production_compare')
        with self.assertRaises(SystemExit):
            parse_arguments(['--production-compare', '--variant=debug'])
        with self.assertRaises(SystemExit):
            parse_arguments(['--production-compare', '--profile-samples=0'])

    def test_registry_semantics_and_utf8_byte_offset(self):
        containers = {case['name']: case for case in container_cases()}
        self.assertEqual(containers['containers/map/NaN_zero']['expected'], 28)
        self.assertEqual(containers['containers/filter/narrow']['expected'], 87)
        cases = {case['name']: case for case in json_cases()}
        error = cases['json/utf8_failure']['json']
        offset = error['offset']
        self.assertEqual(error['input'].encode()[offset:offset + 5], b'false')
        self.assertGreater(offset, error['input'].index('false'))
        for name in ('early_failure', 'late_failure'):
            self.assertIn('ConstructItem[]', cases['json/' + name]['body'])
            self.assertIn('jsonCalls != 0', cases['json/' + name]['body'])


if __name__ == '__main__':
    unittest.main()
