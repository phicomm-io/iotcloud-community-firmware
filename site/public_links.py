"""Canonical source links, separate from immutable release provenance."""
FIRMWARE_PUBLIC_REPOSITORY = 'phicomm-io/iotcloud-community-firmware'
PUBLIC_LINKS = {
    'FIRMWARE_URL': 'https://github.com/' + FIRMWARE_PUBLIC_REPOSITORY,
    'PROTOCOLS_URL': 'https://phicomm-io.github.io/iotcloud-community-protocols/',
    'FRONTEND_URL': 'https://github.com/phicomm-io/iotcloud-community-frontend',
    'PLATFORM_URL': 'https://github.com/phicomm-io/iotcloud-community-mqtt',
    'MCU_URL': 'https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20',
    'COMMUNITY_URL': 'https://github.com/phicomm-io/.github',
}


def expand_public_links(text):
    for name, url in PUBLIC_LINKS.items():
        text = text.replace('{{' + name + '}}', url)
    return text
