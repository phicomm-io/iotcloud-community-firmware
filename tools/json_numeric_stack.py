"""Actual private numeric frames, with explicit conservative helper closures.

These sums retain existing SDK/RTOS reserves. They are static upper bounds;
helper alternatives, allocator observation, and lifetime cleanup are siblings.
"""
from pathlib import Path

CORE_FILES = ('context', 'mprec', 'dtoa', 'strtod', 'match', 'scan', 'format')
NUMERIC_SYMBOLS = '''json_number_scan json_num_scan_token json_num_strtod_r
json_number_format json_number_format_fixed json_number_format_float32 json_num_dtoa_r json_num_Balloc
json_num_Bfree json_num_multadd json_num_s2b json_num_i2b json_num_multiply
json_num_pow5mult json_num_lshift json_num_mdiff json_num_d2b json_num_ratio
json_num_b2d json_num_init_mp json_num_calloc json_num_cleanup
json_num_resource_error json_heap_reject'''.split()
JSON_SYMBOLS = ('json_object_double_to_json_string', 'json_tokener_parse_ex',
                'json_tokener_parse', 'json_tokener_parse_verbose', 'json_object_get_double')


def read_numeric_frames(modules, frames):
    heaps = list(Path(modules).rglob('json_heap.su'))
    if len(heaps) != 1:
        raise ValueError('Expected one actual SDK json_heap.su for numeric closure')
    directory = heaps[0].parent
    for stem in (*CORE_FILES, 'json_object', 'json_tokener'):
        paths = list(directory.rglob(stem+'.su'))
        if len(paths) != 1:
            raise ValueError('Expected one actual SDK numeric '+stem+'.su')
        for line in paths[0].read_text().splitlines():
            location, size, kind = line.split('\t')
            name = location.rsplit(':', 1)[-1].split('.')[0]
            if name in ('quorem', 'sulp'):
                name = 'json_numeric.'+name
            if name.startswith(('json_num_', 'json_number_', 'json_numeric.')) or name in JSON_SYMBOLS:
                if kind != 'static':
                    raise ValueError('Unbounded numeric frame: '+name)
                frames[name] = max(frames.get(name, 0), int(size))
    missing = set(NUMERIC_SYMBOLS + list(JSON_SYMBOLS)) - frames.keys()
    if missing:
        raise ValueError('Missing actual numeric .su frames: '+', '.join(sorted(missing)))


def numeric_tails(frames, operation):
    """Return independently live branches, never sum sequential cleanup/format.

    Include optional emitted integer exponent helpers. For allocation branches
    conservatively retain init_mp even when its initialization already happened;
    that overcounts at most its one emitted frame, without omitting any callee.
    Standard allocation-free memcpy/AEABI/ctype and RTOS interiors remain in
    the unchanged external reserve.
    """
    if operation == 'scan':
        entry = ['json_number_scan', 'json_num_scan_token', 'json_num_strtod_r']
        helpers = (['json_num_s2b', 'json_num_multadd'], ['json_num_d2b'],
                   ['json_num_pow5mult', 'json_num_multadd'],
                   ['json_num_pow5mult', 'json_num_multiply'],
                   ['json_num_lshift'], ['json_num_mdiff'], ['json_num_i2b'], [])
    else:
        entry = [{'fixed': 'json_number_format_fixed', 'float32': 'json_number_format_float32'}.get(operation, 'json_number_format'),
                 'json_num_dtoa_r']
        helpers = (['json_num_d2b'], ['json_num_pow5mult', 'json_num_multadd'],
                   ['json_num_pow5mult', 'json_num_multiply'],
                   ['json_num_lshift'], ['json_num_mdiff'], ['json_num_i2b'], [])
    tails = {}
    for index, chain in enumerate(helpers):
        allocating = entry + chain + ['json_num_Balloc', 'json_num_init_mp', 'json_num_calloc']
        tails[f'headroom{index}'] = allocating + ['json_heap_calloc', 'json_heap.permit', 'json_heap_permit']
        for leaf in ('permit', 'created'):
            tails[f'allocate{index} '+leaf] = allocating + ['json_heap_calloc', 'json_heap.'+leaf, 'json_heap_failure']
        tails[f'policy{index}'] = allocating + ['json_num_resource_error', 'json_heap_reject', 'json_heap_failure']
        for leaf in ('json_heap_lock', 'json_heap_unlock'):
            tails[f'allocator{index} '+leaf] = allocating + ['json_heap_calloc', leaf]
            tails[f'policy{index} '+leaf] = allocating + ['json_num_resource_error', 'json_heap_reject', leaf]
    # Nonallocating arithmetic can be deeper than an allocation helper.
    arithmetic = entry + (['json_num_ratio', 'json_num_b2d'] if operation == 'scan' else ['json_numeric.quorem'])
    tails['arithmetic'] = arithmetic
    if operation == 'scan' and 'json_numeric.sulp' in frames:
        tails['underflow arithmetic'] = entry + ['json_numeric.sulp']
    for helper in ('json_num_scan_exp', 'json_num_write_exp', 'json_num_exp_length'):
        if operation == 'scan' and helper in frames:
            tails[helper] = entry[:2] + [helper]
    for leaf in ('json_heap_lock', 'json_heap_unlock'):
        tails['cleanup '+leaf] = entry[:1] + ['json_num_cleanup', 'json_heap_free', leaf]
    return tails


def add_numeric_paths(paths, frames, prefix, reserve, label, operation):
    for branch, tail in numeric_tails(frames, operation).items():
        paths[label+' '+branch] = (reserve, prefix + tail)
