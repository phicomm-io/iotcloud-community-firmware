#!/usr/bin/env python3
"""Asset names shared by firmware packaging, CI and release metadata."""
import argparse

STEMS = {'tc1': 'phicomm_tc1_a1', 'm1': 'phicomm_m1', 'a1': 'phicomm_a1',
         'daikin': 'daikin_BRY88AX151K'}
MODEL_VARIANTS = {
    'tc1': ('full', 'no-frontend-resources'),
    'm1': ('full', 'no-frontend-resources'),
    'a1': ('full', 'no-frontend-resources', 'fresh-air', 'fresh-air-no-frontend-resources'),
    'daikin': ('full', 'hub'),
}
VARIANTS = tuple(dict.fromkeys(v for variants in MODEL_VARIANTS.values() for v in variants))


def variants_for(model, tag=None):
    """The frozen 0.x archive predates the independent DAIKIN hub image."""
    variants = MODEL_VARIANTS[model]
    return ('full',) if model == 'daikin' and tag and tag.startswith('v0.') else variants


def targets(tag=None):
    for model in STEMS:
        for variant in variants_for(model, tag):
            yield model, variant


def asset_stem(model, variant='full'):
    if model not in MODEL_VARIANTS or variant not in MODEL_VARIANTS[model]:
        raise ValueError(f'Unsupported firmware variant: {model}/{variant}')
    return STEMS[model] + ('' if variant == 'full' else '-' + variant)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('model', choices=STEMS, nargs='?')
    parser.add_argument('variant', choices=VARIANTS, nargs='?', default='full')
    parser.add_argument('--targets', action='store_true', help='Print each supported model and variant for CI/signing')
    parser.add_argument('--tag', help='Select the historical target set for an archive tag')
    args = parser.parse_args()
    if args.targets:
        for model, variant in targets(args.tag):
            print(model, variant)
    elif args.model:
        print(asset_stem(args.model, args.variant))
    else:
        parser.error('specify a model or --targets')
