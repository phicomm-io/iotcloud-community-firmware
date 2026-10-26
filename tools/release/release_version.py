#!/usr/bin/env python3
"""The release grammar shared by firmware packaging and publication tools.

VERSION remains a numeric core. Published tags add an optional release-candidate
suffix; development commit suffixes deliberately do not belong to this grammar.
"""
import argparse
import re
from typing import NamedTuple

NUMBER = r'(?:0|[1-9][0-9]*)'
VERSION_PATTERN = re.compile(rf'({NUMBER})\.({NUMBER})\.({NUMBER})(?:-(rc)(?:\.({NUMBER}))?)?\Z')
TAG_PATTERN = re.compile('v' + VERSION_PATTERN.pattern)


class ReleaseVersion(NamedTuple):
    core: tuple
    candidate: object
    version: str

    @property
    def tag(self):
        return 'v' + self.version

    @property
    def prerelease(self):
        return self.candidate is not None

    @property
    def channel(self):
        return 'candidate' if self.prerelease else 'stable'

    @property
    def key(self):
        # SemVer: rc < rc.0 < rc.1 < the stable release of the same core.
        return self.core + ((1, 0, 0) if not self.prerelease else
                            (0, int(self.candidate != ''), int(self.candidate or 0)))


def parse_version(value):
    if not isinstance(value, str):
        raise ValueError('Release version must be MAJOR.MINOR.PATCH[-rc[.N]]')
    text = value.removeprefix('v')
    match = VERSION_PATTERN.fullmatch(text)
    if not match:
        raise ValueError('Release version must be MAJOR.MINOR.PATCH[-rc[.N]] without leading zeros')
    candidate = (match[5] if match[5] is not None else '') if match[4] else None
    return ReleaseVersion(tuple(int(part) for part in match.group(1, 2, 3)), candidate, text)


def parse_tag(value):
    if not isinstance(value, str) or not value.startswith('v'):
        raise ValueError('Release tag must be vMAJOR.MINOR.PATCH[-rc[.N]]')
    return parse_version(value)


def version_key(value):
    return parse_version(value).key


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tag')
    args = parser.parse_args()
    try:
        print(parse_tag(args.tag).channel)
    except ValueError as error:
        parser.error(str(error))
