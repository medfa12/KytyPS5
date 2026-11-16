"""Check focused ESVM source fixture logs with tracing disabled and enabled."""
import argparse
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disabled', type=Path)
    parser.add_argument('enabled', type=Path)
    parser.add_argument('--repeated-hevc', action='store_true', help='require ordered repeated-HEVC regressions')
    args = parser.parse_args()
    def read_log(path):
        data = path.read_bytes()
        return data.decode('utf-16' if data.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8', errors='replace')
    logs = [re.sub(r'\x1b\[[0-9;]*m', '', read_log(path)) for path in (args.disabled, args.enabled)]
    rows = [[line for line in text.splitlines() if line.startswith('MediaFramework state:')]
            for text in logs]
    assert not rows[0], 'disabled trace emitted state records'
    capped = int(re.search(r'capped-source=(\d+)', logs[1])[1])
    caprows = [line for line in rows[1] if f'source={capped} ' in line]
    assert len(caprows) == 65 and sum('trace-cap=64' in line for line in caprows) == 1
    blocked = re.findall(r'blocked-source=(\d+) child=(\d+)', logs[1])
    assert len(blocked) == 1, 'blocked-source fixture must run'
    for source, child in blocked:
        records = [dict(re.findall(r'([a-z-]+)=([^ ]+)', line)) for line in rows[1]
                   if f'source={source} ' in line and f'child={child} ' in line]
        def has(**values):
            return any(all(row.get(key.replace('_', '-')) == str(value)
                           for key, value in values.items()) for row in records)
        assert has(flags=3, decoded=4, payload_aus=1, packets=1, held=1,
                   eos=0, sufficient=0, refill_capacity=1, batch_units=6)
        assert has(decoded=4, payload_aus=1, packets=2, held=1, eos=1, sufficient=1)
        assert has(reason='ready-claimed', pending=0)
        assert any(row.get('entry') == 'pause' and int(row['caller'], 16) != 0 for row in records)
        assert all(int(row['host-us']) > 0 for row in records)
    assert not any('global-trace-cap' in line for line in rows[1]), 'fixture lost observations'
    if args.repeated_hevc:
        fixtures = re.findall(r'repeated-hevc-source=(\d+) child=(\d+) variant=(\d+)', logs[1])
        assert {variant for _, _, variant in fixtures} == {'0', '1', '2', '3'} and len(fixtures) == 4
        for source, child, variant in fixtures:
            records = [dict(re.findall(r'([a-z-]+)=([^ ]+)', line)) for line in rows[1]
                       if f'source={source} ' in line and f'child={child} ' in line]
            assert has(reason='observe-enter', selected_gen=3, generation=3, waiting_sync=1, timestamp=0)
            claims = [row for row in records if row.get('reason') == 'ready-claimed']
            assert len(claims) == (0 if variant == '3' else 1)
            if variant != '3':
                assert claims[0]['timestamp'] == '0' and claims[0]['eos'] == '0'
                assert claims[0]['sufficient'] == '1' and claims[0]['selected-gen'] == '3'
            if variant == '0':
                assert has(reason='observe-exit', selected_gen=3, generation=3, waiting_sync=0,
                           timestamp=0, eligible=1, batch_units=23, subtype=2, decoded=4)
            elif variant == '1':
                assert has(reason='observe-enter', selected_gen=3, waiting_sync=1, decoded=0, pending=0, batch=2)
            elif variant == '2':
                assert has(reason='observe-exit', selected_gen=3, flags=3, sufficient=0, pending=0, batch_units=6)
            else:
                assert has(reason='observe-exit', selected_gen=3, flags=2, sufficient=1, pending=0, playing=1)
        print('PASS: repeated HEVC preserves generation, real output Sync, no timestamp/EOS shortcut, config-only/short/Play controls')
    print(f'PASS: disabled=0 enabled={len(rows[1])}; source cap=64+1; '
          'blocked predicate, EOS, readiness claim, caller and timestamp')


if __name__ == '__main__':
    main()
