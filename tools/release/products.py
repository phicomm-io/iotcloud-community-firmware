#!/usr/bin/env python3
"""Asset names shared by firmware packaging, CI and release metadata."""
import argparse

STEMS = {'tc1': 'phicomm_tc1_a1', 'm1': 'phicomm_m1', 'a1': 'phicomm_a1',
         'daikin': 'daikin_BRY88AX151K'}
VARIANTS = ('full', 'no-frontend-resources', 'fresh-air', 'fresh-air-no-frontend-resources')


def asset_stem(model, variant='full'):
    if variant not in VARIANTS or (model == 'daikin' and variant != 'full') or (variant.startswith('fresh-air') and model != 'a1'):
        raise ValueError(f'Unsupported firmware variant: {model}/{variant}')
    return STEMS[model] + ('' if variant == 'full' else '-' + variant)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('model', choices=STEMS)
    parser.add_argument('variant', choices=VARIANTS, nargs='?', default='full')
    args = parser.parse_args()
    print(asset_stem(args.model, args.variant))
