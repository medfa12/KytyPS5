"""Read-only bounded summary of retained Apple media traces (no URL/key dumps)."""
import argparse
from collections import Counter
import hashlib
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('guest_log')
    parser.add_argument('--timeline-limit', type=int, default=32,
                        help='maximum numeric lifecycle records to print (0..256; default 32)')
    args = parser.parse_args()
    if not 0 <= args.timeline_limit <= 256:
        parser.error('--timeline-limit must be between 0 and 256')
    digest = hashlib.sha256()
    counts = Counter()
    results = Counter()
    timeline = []
    timeline_seen = set()
    milestones = {'source-admitted', 'decoder-admitted-1360', 'startup-flush-complete',
                  'flush-complete', 'source-ready-delivered', 'video-mailbox-activated',
                  'audio-lease-opened', 'audio-pcm-accepted', 'decoder-removed'}
    record = re.compile(rb'MediaFramework source: operation=([a-z0-9-]+) '
                        rb'pid=([0-9]+) handle=([0-9]+) bytes=([0-9]+) '
                        rb'result=([0-9a-fA-F]{8}) detail=(-?[0-9]+)')
    size = lines = 0
    with open(args.guest_log, 'rb') as stream:
        for line in stream:
            digest.update(line)
            size += len(line)
            lines += 1
            match = re.search(rb'MediaFramework source: operation=([a-z0-9-]+) ', line)
            if match:
                counts[match[1].decode('ascii')] += 1
            match = record.search(line)
            if match:
                operation, pid, handle, size_value, result, detail = (
                    value.decode('ascii') for value in match.groups())
                results[(operation, result.lower())] += 1
                # Only operation labels and numeric fields leave this reader.
                # Never print an entire guest line (which may contain tokens/URLs).
                identity = (operation, pid, handle, result)
                if (len(timeline) < args.timeline_limit and identity not in timeline_seen and
                        (operation in milestones or int(result, 16))):
                    timeline_seen.add(identity)
                    timeline.append(f'line={lines} {operation} pid={pid} handle={handle} '
                                    f'bytes={size_value} result={result} detail={detail}')
    print(f'bytes={size} lines={lines} sha256={digest.hexdigest()}')
    for operation, count in sorted(counts.items()):
        print(f'{operation}: {count}')
    for operation in ('startup-flush-complete', 'source-ready-delivered', 'flush-complete', 'audio-pcm-accepted'):
        if operation not in counts:
            print(f'{operation}: 0 (absence in retained trace, not proof of no native calls)')
    for (operation, result), count in sorted(results.items()):
        if int(result, 16):
            print(f'nonzero-result {operation}: {result} count={count}')
    if timeline:
        print(f'first lifecycle/error records (limit={args.timeline_limit}; not playback proof):')
        for entry in timeline:
            print(entry)


if __name__ == '__main__':
    main()
