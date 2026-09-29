"""Summarise capture/replay diagnostic JSON without opening or modifying an RDC."""
import argparse
import json


def compare(capture, replay=None):
    events = [row['event'] for row in capture['observations']]
    created = {e['resource']: e['input_md5'] for e in events if e['phase'] == 'create_input'}
    decoded = {} if replay is None else {
        row['event']['resource']: row['event']['input_md5']
        for row in replay['observations'] if row['event']['phase'] == 'deserialize'}
    results = []
    for resource, digest in created.items():
        blob = capture['blobs'][digest]
        stripped_from = [e['input_md5'] for e in events
                         if e['phase'] == 'D3DStripShader' and e['output_md5'] == digest and
                         capture['blobs'][e['input_md5']]['has_rdef']]
        if blob['has_rdef']:
            finding = 'reflection_available_at_create'
        elif stripped_from:
            finding = 'reflection_removed_by_observed_runtime_strip'
        else:
            finding = 'no_reflection_observed_at_create_origin_unknown'
        results.append(dict(resource=resource, bytecode_md5=digest, finding=finding,
                            cbuffer_names=[b['name'] for b in blob['cbuffers']],
                            strip_input_hashes=sorted(set(stripped_from)),
                            present_in_replay=resource in decoded if replay else None,
                            captured_bytes_preserved=(decoded[resource] == digest) if resource in decoded else None))
    return dict(shaders=results,
                successful_compiler_calls=sum(row['count'] for row in capture['observations']
                                              if row['event']['phase'].startswith('D3DCompile')),
                successful_strip_calls=sum(row['count'] for row in capture['observations']
                                           if row['event']['phase'] == 'D3DStripShader'),
                limitations=['Observations start when hooks are installed; absent compiler calls do not prove offline compilation.',
                             'A created shader missing from replay may simply be unreferenced by this frame.',
                             'Resource debug names and shader-local cbuffer names are independent.'],
                dropped_records=capture['dropped_records'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture_report')
    parser.add_argument('--replay-report')
    parser.add_argument('--output')
    args = parser.parse_args()
    with open(args.capture_report, encoding='utf-8') as f:
        capture = json.load(f)
    replay = None
    if args.replay_report:
        with open(args.replay_report, encoding='utf-8') as f:
            replay = json.load(f)
    result = json.dumps(compare(capture, replay), indent=2, ensure_ascii=True)
    if args.output:
        with open(args.output, 'w', encoding='utf-8') as f:
            f.write(result + '\n')
    print(result)
