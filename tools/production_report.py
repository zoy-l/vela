"""Run isolated production profiles and retain only checked aggregate comparisons."""

from __future__ import annotations

from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import re
from statistics import geometric_mean, mean, median, stdev
import subprocess
import sys
import tempfile

from tools.benchmark_report import MICROSECONDS, checked_result, iteration_rows, validate_counters
from tools.config import ROOT, toolchain_tools


PATTERN = re.compile(r'^production/(cpp|aot)/(.+)/(?:iterations:\d+/)?(?:real_time|manual_time)$')
ALLOCATION_FIELDS = ('allocations_per_invocation', 'allocation_bytes_per_invocation',
                     'peak_live_growth_bytes', 'baseline_bytes', 'retained_bytes',
                     'expected_retained_bytes')
LATENCY_FIELDS = ('p50_ns', 'p95_ns', 'p99_ns', 'maximum_ns', 'latency_samples',
                  'resident_baseline_bytes', 'process_resident_bytes',
                  'process_peak_resident_bytes', 'process_peak_growth_bytes')


def read_option(arguments: list[str], name: str, default: str) -> str:
    result = default
    for index, argument in enumerate(arguments):
        if argument.startswith('--' + name + '='):
            result = argument.partition('=')[2]
        elif argument == '--' + name:
            if index + 1 == len(arguments):
                raise ValueError(f'--{name} requires a value')
            result = arguments[index + 1]
    return result


def profile_names(case: str, profile: str, samples: int) -> str:
    return case.removesuffix('/real_time') + f'/iterations:{samples}/' + (
        'manual_time' if profile == 'latency' else 'real_time')


def summarize(throughput: dict, selected: list[str], profiles: dict, repetitions: int) -> dict:
    rows = iteration_rows(throughput, selected, repetitions)
    cases = {}
    pairs = {}
    for name, entries in rows.items():
        match = PATTERN.fullmatch(name)
        if match is None:
            raise ValueError(f'Unsupported production case: {name}')
        backend, workload = match.groups()
        results = {checked_result(name, entry) for entry in entries}
        if len(results) != 1:
            raise ValueError(f'{name}: unstable checksum')
        times = [entry['real_time'] * MICROSECONDS[entry['time_unit']] * 1000 for entry in entries]
        record = dict(suite='production_compare', unit='ns per complete workload',
                      median_ns=median(times), min_ns=min(times), max_ns=max(times),
                      wall_cv=stdev(times) / mean(times) if len(times) > 1 else 0,
                      expected_result=results.pop(), samples=repetitions)
        for profile, fields in (('latency', LATENCY_FIELDS), ('allocations', ALLOCATION_FIELDS)):
            bundle = profiles[name][profile]
            expected_name = profile_names(name, profile, bundle['samples'])
            profiled = iteration_rows(bundle['report'], [expected_name], repetitions)[expected_name]
            for entry in profiled:
                if checked_result(name, entry) != record['expected_result']:
                    raise ValueError(f'{name}: profile checksum differs from throughput')
                validate_counters(name, entry, fields)
                if profile == 'latency':
                    if not (entry['p50_ns'] <= entry['p95_ns'] <= entry['p99_ns'] <= entry['maximum_ns']):
                        raise ValueError(f'{name}: unordered latency quantiles')
                    if entry['latency_samples'] != bundle['samples'] or entry['iterations'] != bundle['samples']:
                        raise ValueError(f'{name}: incomplete latency samples')
                    if (entry['process_resident_bytes'] <= 0 or
                            entry['process_peak_resident_bytes'] < entry['process_resident_bytes']):
                        raise ValueError(f'{name}: invalid process memory')
                else:
                    if entry['iterations'] != bundle['samples']:
                        raise ValueError(f'{name}: incomplete allocation invocations')
                    if entry['retained_bytes'] != entry['expected_retained_bytes']:
                        raise ValueError(f'{name}: retained temporary state')
                    if backend == 'aot' and (entry['remaining_saved_bytes'] != 0 or
                                              entry['remaining_suspended_frames'] != 0):
                        raise ValueError(f'{name}: retained asynchronous state')
                    if workload.startswith('longrun/'):
                        validate_counters(name, entry, ('steady_live_bytes',))
            counters = set(fields)
            if profile == 'allocations' and workload.startswith('longrun/'):
                counters.add('steady_live_bytes')
            record[profile] = {field: median(entry[field] for entry in profiled) for field in sorted(counters)}
            record[profile]['invocations_per_repetition'] = bundle['samples']
            record[profile]['total_invocations'] = bundle['samples'] * repetitions
            if profile == 'latency':
                record[profile]['measured_seconds_per_repetition'] = median(
                    entry['real_time'] * MICROSECONDS[entry['time_unit']] / 1e6 * entry['iterations']
                    for entry in profiled)
        cases[name] = record
        pairs.setdefault(workload, {})[backend] = name
    for workload, pair in pairs.items():
        if set(pair) != {'cpp', 'aot'}:
            raise ValueError(f'{workload}: select both C++ and AOT backends')
        cpp, aot = cases[pair['cpp']], cases[pair['aot']]
        if cpp['expected_result'] != aot['expected_result']:
            raise ValueError(f'{workload}: paired checksums differ')
        pair['ratios_to_cpp'] = dict(aot=aot['median_ns'] / cpp['median_ns'])
    paired_records = []
    for workload, pair in sorted(pairs.items()):
        paired_records.append(dict(workload=workload, cpp_reference='cpp', unit='ns per complete workload',
            backends={backend: dict(case=pair[backend], median_ns=cases[pair[backend]]['median_ns'],
                                    wall_cv=cases[pair[backend]]['wall_cv']) for backend in ('cpp', 'aot')},
            ratios_to_cpp=pair['ratios_to_cpp']))
    ratios = [pair['ratios_to_cpp']['aot'] for pair in paired_records]
    return dict(cases=cases, workload_pairs=paired_records, native_call_pairs=[], native_class_cpp_pairs=[],
                ratio_summary=dict(complete_workloads=dict(aot=dict(comparisons=len(ratios),
                    median=median(ratios), geometric_mean=geometric_mean(ratios), min=min(ratios), max=max(ratios))),
                    native_free_calls={}, native_class_calls={}),
                all_expected_cases_passed=True,
                validation=dict(all_expected_cases_and_repetitions_passed=True, all_paired_checksums=True,
                                all_isolated_profiles_and_cleanup_passed=True))


def render(summary: dict) -> str:
    lines = ['# Production AOT / C++ comparison', '',
             'Times cover complete checked calls, owned results, destruction/automatic GC and '
             'async completion. No entry overhead is subtracted. Throughput uses ordinary '
             'allocators; latency and allocation profiles run separately in a fresh process '
             'for each backend, case and repetition.', '',
             'C++ allocation counters cover owned container/string storage, coroutine frames, '
             'queues, yyjson arenas and input buffers. Vela counters cover Heap-managed and '
             'Heap-accounted external storage. Neither is a universal malloc count. RSS covers '
             'the whole process; peak growth is lifetime high-water minus post-warm-up RSS, '
             'not a resettable allocator peak. Profile timings are never used for throughput ratios.', '',
             'Latency values are medians of per-process nearest-rank quantiles, including clock '
             'cost. Long-run cases retain 64 records, replace one per call, validate the entire '
             'graph and check the live-memory plateau and cleanup. Duration and invocation counts '
             'are retained in JSON.', '',
             '| Workload | C++ ns | AOT ns | AOT/C++ | CV C++ / AOT |',
             '| --- | ---: | ---: | ---: | ---: |']
    for pair in summary['workload_pairs']:
        workload = pair['workload']
        cpp, aot = (summary['cases'][pair['backends'][key]['case']] for key in ('cpp', 'aot'))
        lines.append(f"| {workload} | {cpp['median_ns']:.1f} | {aot['median_ns']:.1f} | "
                     f"{pair['ratios_to_cpp']['aot']:.2f} | {cpp['wall_cv']:.1%} / {aot['wall_cv']:.1%} |")
    lines += ['', '| Workload | Backend | p50 ns | p95 ns | p99 ns | Max ns | Peak RSS MiB | RSS growth MiB | Allocations/call | Bytes/call | Peak live growth B | Steady live B | Retained B |',
              '| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for name, record in sorted(summary['cases'].items()):
        backend, workload = PATTERN.fullmatch(name).groups()
        latency, allocations = record['latency'], record['allocations']
        lines.append(f"| {workload} | {backend} | {latency['p50_ns']:.0f} | "
                     f"{latency['p95_ns']:.0f} | {latency['p99_ns']:.0f} | {latency['maximum_ns']:.0f} | "
                     f"{latency['process_peak_resident_bytes'] / 2**20:.2f} | "
                     f"{latency['process_peak_growth_bytes'] / 2**20:.2f} | "
                     f"{allocations['allocations_per_invocation']:.2f} | "
                     f"{allocations['allocation_bytes_per_invocation']:.1f} | "
                     f"{allocations['peak_live_growth_bytes']:.0f} | "
                     f"{allocations.get('steady_live_bytes', 0):.0f} | {allocations['retained_bytes']:.0f} |")
    return '\n'.join(lines) + '\n'


def promote(summary: dict, destination: Path) -> None:
    retained = json.loads(destination.read_text(encoding='utf-8'))
    if set(retained) != {'previous', 'latest'} or not summary['all_expected_cases_passed']:
        raise ValueError('Only a complete successful suite can be retained')
    previous = retained['latest']
    encoded = json.dumps(previous, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()
    comparable = all(previous.get(key) == summary.get(key) for key in (
        'suite', 'machine', 'toolchain', 'configuration', 'settings'))
    summary['comparison'] = dict(comparable=comparable,
        reason='Matching production suite settings' if comparable else 'Different suite, machine, build or measurement settings',
        previous_summary_sha256=hashlib.sha256(encoded).hexdigest(),
        previous_summary_encoding="UTF-8 JSON; ensure_ascii=False; sort_keys=True; separators=(',', ':'); no trailing newline")
    with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', newline='\n',
                                     dir=destination.parent, delete=False) as output:
        temporary = Path(output.name)
        json.dump(dict(previous=previous, latest=summary), output, ensure_ascii=False, indent=2)
        output.write('\n')
    try:
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


def run(executable: Path, output: Path, parsed) -> None:
    arguments = list(parsed.benchmark_arguments)
    if any(argument.startswith(('--profile', '--benchmark_iterations', '--benchmark_dry_run',
                                 '--benchmark_list_tests', '--benchmark_report_aggregates_only'))
           for argument in arguments):
        raise ValueError('Production profiles own invocation counts and require raw repetitions')
    defaults = dict(benchmark_filter='^production/', benchmark_min_time='0.2s',
                    benchmark_repetitions='7', benchmark_enable_random_interleaving='true')
    for name, value in defaults.items():
        if not any(argument == '--' + name or argument.startswith('--' + name + '=') for argument in arguments):
            arguments.append('--' + name + '=' + value)
    selected = subprocess.run([str(executable), '--benchmark_list_tests=true',
        '--benchmark_filter=' + read_option(arguments, 'benchmark_filter', '')],
        cwd=ROOT, check=True, capture_output=True, text=True, encoding='utf-8').stdout.splitlines()
    repetitions = int(read_option(arguments, 'benchmark_repetitions', '7'))
    if repetitions < 1 or not selected:
        raise ValueError('Production comparison requires cases and positive repetitions')
    manifest = json.loads((output / 'gen/benchmarks/production_native/cases.json').read_text(encoding='utf-8'))
    registry = {f"production/{backend}/{case['name']}/real_time"
                for case in manifest for backend in ('cpp', 'aot')}
    if not set(selected) <= registry:
        raise ValueError('Selected cases differ from the generated production registry')
    if parsed.retain and set(selected) != registry:
        raise ValueError('--retain requires the complete production suite')
    # 在启动昂贵测量前拒绝不成对的筛选。
    for case in selected:
        if case.replace('/cpp/', '/aot/') not in selected or case.replace('/aot/', '/cpp/') not in selected:
            raise ValueError('Production comparisons require paired C++ and AOT cases')
    report = (parsed.report or Path(tempfile.mkdtemp(prefix='vela-production-')) / 'production.json').resolve()
    report.parent.mkdir(parents=True, exist_ok=True)
    report.unlink(missing_ok=True)
    report.with_suffix('.md').unlink(missing_ok=True)
    raw = report.parent / (report.stem + '-samples')
    raw.mkdir(parents=True, exist_ok=True)
    throughput_path = raw / 'throughput.json'
    subprocess.run([str(executable), *arguments, f'--benchmark_out={throughput_path}',
                    '--benchmark_out_format=json'], cwd=ROOT, check=True)
    throughput = json.loads(throughput_path.read_text(encoding='utf-8'))
    iteration_rows(throughput, selected, repetitions)
    profiles = {}
    for index, case in enumerate(selected):
        profiles[case] = {}
        for profile, regular in (('latency', parsed.profile_samples), ('allocations', parsed.allocation_samples)):
            samples = parsed.longrun_samples if '/longrun/' in case else regular
            merged = dict(benchmarks=[])
            for repetition in range(repetitions):
                target = raw / f'{index:03}-{profile}-{repetition}.json'
                base = case.removesuffix('/real_time')
                command = [str(executable), f'--profile={profile}', f'--profile-samples={samples}',
                    '--benchmark_filter=^' + re.escape(base) + '/', '--benchmark_repetitions=1',
                    f'--benchmark_out={target}', '--benchmark_out_format=json']
                process = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, encoding='utf-8')
                if process.returncode:
                    raise RuntimeError(f'{case} {profile}: {process.stderr}')
                document = json.loads(target.read_text(encoding='utf-8'))
                expected_name = profile_names(case, profile, samples)
                entries = iteration_rows(document, [expected_name], 1)[expected_name]
                for entry in entries:
                    entry['repetitions'] = repetitions
                    entry['repetition_index'] = repetition
                    merged['benchmarks'].append(entry)
            profiles[case][profile] = dict(samples=samples, report=merged)
        print(f'[{index + 1}/{len(selected)}] {case}', flush=True)
    summary = summarize(throughput, selected, profiles, repetitions)
    metrics = json.loads((output / 'gen/benchmarks/production_native/build_metrics.json').read_text(encoding='utf-8'))
    cxx_version = subprocess.run([str(toolchain_tools()['cxx']), '--version'], check=True,
        capture_output=True, text=True, encoding='utf-8').stdout.strip()
    source_hashes = {path.relative_to(ROOT).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                     for path in sorted([*(ROOT / 'benchmarks/production').glob('*'),
                                         *(ROOT / 'tools').glob('production_*.py'),
                                         ROOT / 'tools/bench.py', ROOT / 'benchmarks/BUILD.gn'])
                     if path.is_file()}
    git = lambda *args: subprocess.run(['git', *args], cwd=ROOT, check=True,
        capture_output=True, text=True, encoding='utf-8').stdout.strip()
    summary.update(schema_version=1, suite='production-aot-cpp/v1', run_date=datetime.now().astimezone().date().isoformat(),
        benchmark_started_at=throughput['context']['date'], benchmark_context=throughput['context'],
        source=dict(revision=git('rev-parse', 'HEAD'), working_tree=git('status', '--porcelain=v1'),
                    tracked_diff_sha256=hashlib.sha256(git('diff', 'HEAD', '--binary').encode()).hexdigest(),
                    benchmark_file_sha256=source_hashes),
        machine=dict(os=platform.platform(), architecture=platform.machine(), cpu=platform.processor()),
        toolchain=dict(cxx=cxx_version, **{key: metrics[key] for key in ('toolchain_build', 'target', 'runtime_abi')}),
        configuration=dict(variant='release', optimization=metrics['optimization']),
        build=dict(variant='release', target='benchmarks',
            args_gn_sha256=hashlib.sha256((output / 'args.gn').read_bytes()).hexdigest(),
            benchmark_binaries={executable.name: dict(path=str(executable), sha256=hashlib.sha256(executable.read_bytes()).hexdigest())}),
        measurement=dict(minimum_time=read_option(arguments, 'benchmark_min_time', ''), repetitions=repetitions,
            random_interleaving=read_option(arguments, 'benchmark_enable_random_interleaving', 'false') == 'true',
            ratio='Vela AOT median wall time / matching C++ median wall time', samples_discarded=False,
            overhead_subtracted=False, profiles='one fresh process per backend/case/profile/repetition'),
        settings=dict(repetitions=repetitions, profile_samples=parsed.profile_samples,
                      allocation_samples=parsed.allocation_samples, longrun_samples=parsed.longrun_samples,
                      throughput_arguments=arguments),
        command=[sys.executable, '-m', 'tools.vl', 'bench', *sys.argv[1:]],
        raw_samples=str(raw), measurement_scope='complete invocation; separate uninstrumented throughput/latency and owned-allocation profiles')
    report.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n', encoding='utf-8', newline='\n')
    report.with_suffix('.md').write_text(render(summary), encoding='utf-8', newline='\n')
    if parsed.retain:
        promote(summary, ROOT / 'benchmarks/results/rolling_summary.json')
    print(render(summary))
    print(f'Aggregate report: {report}\nRaw samples: {raw}')
