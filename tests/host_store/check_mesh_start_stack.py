#!/usr/bin/env python3
"""Check the configured C3's first HELLO/history-write call-chain budget."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


def frames(binary, objdump):
    dump = subprocess.check_output([str(objdump), '-d', '-C', str(binary)], text=True)
    result = {}
    for block in re.split(r'(?=^[0-9a-f]+ <[^\n]+>:$)', dump, flags=re.M):
        lines = block.splitlines()
        if not lines or not re.match(r'^[0-9a-f]+ <', lines[0]):
            continue
        name = lines[0].split('<', 1)[1].rsplit('>:', 1)[0]
        size = 0
        started = False
        for line in lines[1:35]:
            m = re.search(r'\baddi\s+sp,sp,-(\d+)', line)
            if m:
                size += int(m[1]); started = True
            elif started and re.search(r'\b(?:jal|jalr|ret)\b', line):
                break
        result[name] = size
    return result, dump


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('elf', type=Path)
    p.add_argument('sdkconfig', type=Path)
    p.add_argument('objdump', type=Path)
    p.add_argument('--timer-format-lengths', type=Path,
                   help='Independently bound, string-free static format-length receipt for this ELF')
    a = p.parse_args()
    f, dump = frames(a.elf, a.objdump)
    names = ['main_task', 'app_main', 'katmed_mesh_poll',
             '(anonymous namespace)::send_hello(unsigned long long, unsigned char const*)',
             '(anonymous namespace)::next_event_id(EventId&)', 'store_reserve_event_ids',
             '(anonymous namespace)::write_record(Store*, StoreRecord const&)',
             '(anonymous namespace)::valid_record(StoreRecord const&)', 'app_make_snapshot']
    used = {n: f[n] for n in names}
    assert all(used.values()), 'missing compiler frame'
    budget = int(re.search(r'^CONFIG_ESP_MAIN_TASK_STACK_SIZE=(\d+)$',
                          a.sdkconfig.read_text(), re.M)[1])
    # The final executable must still contain the actual calls that make the
    # history validation reachable; a coincidental sum of unrelated symbols
    # is not a regression of the configured-device failure.
    blocks = re.split(r'(?=^[0-9a-f]+ <[^\n]+>:$)', dump, flags=re.M)
    by_name = {b.splitlines()[0].split('<', 1)[1].rsplit('>:', 1)[0]: b
               for b in blocks if b and re.match(r'^[0-9a-f]+ <', b.splitlines()[0])}
    for caller, callee in zip(names, names[1:]):
        assert re.search(r'\b(?:jal|jalr)\b[^\n]*<' + re.escape(callee) + r'>',
                         by_name[caller]), f'missing call edge {caller} -> {callee}'
    conflict = 'store_record_conflict'
    assert re.search(r'\bjal\b[^\n]*<store_record_conflict>', by_name['katmed_mesh_poll'])
    assert re.search(r'\bjal\b[^\n]*<' + re.escape(names[-3]) + '>', by_name[conflict])
    totals = {'first HELLO/history write': sum(used.values()),
              'received conflict/history write': sum(f[n] for n in
                  [*names[:3], conflict, *names[-3:]]),
              'received event/reducer': sum(f[n] for n in
                  [*names[:3], 'store_apply_event', 'app_apply_event'])}
    print(used)
    for path, total in totals.items():
        print(f'{path}: {total} B, budget {budget} B, reserve 1024 B')
    if max(totals.values()) > budget - 1024:
        raise SystemExit('Mesh/history write exceeds safe main-task stack budget')
    if a.timer_format_lengths:
        receipt = json.loads(a.timer_format_lengths.read_text())
        assert receipt['source_elf_sha256'] == hashlib.sha256(a.elf.read_bytes()).hexdigest()
        assert receipt['all_lengths_bound'] and receipt['sites']
        for site in receipt['sites']:
            assert site['vla_bytes'] == (site['tag_len'] + site['format_len'] + 45) & ~15
        dynamic = max(s['vla_bytes'] for s in receipt['sites'])
        graph = {name: re.findall(r'\b(?:jal|jalr)\b[^\n]*<([^\n]+)>', block)
                 for name, block in by_name.items()}
        paths = []

        def visit(name, active):
            if name in active or len(active) > 16:
                return
            if name == '__ssprint_r':
                paths.append(active + [name])
                return
            for target in graph.get(name, []):
                if target in by_name:
                    visit(target, active + [name])

        for root in receipt['roots']:
            assert root in by_name
            visit(root, [])
        assert paths, 'missing Timer/Mesh snprintf call path'
        worst = max(sum(f[n] for n in path) for path in paths)
        timer = int(re.search(r'^CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=(\d+)$',
                              a.sdkconfig.read_text(), re.M)[1])
        total = f['prvTimerTask'] + f['prvProcessExpiredTimer'] + worst + dynamic
        print(f'Timer/Mesh formatting: {total} B including {dynamic} B dynamic frame, '
              f'budget {timer} B, reserve 1024 B')
        if total > timer - 1024:
            raise SystemExit('Timer/Mesh formatting exceeds safe timer-task stack budget')


if __name__ == '__main__':
    main()
