"""Signed release verification binds the DAIKIN image to full or hub."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest

from cryptography.hazmat.primitives.asymmetric import ec

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/release'))
import verify


class SignedVariantTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.key = ec.generate_private_key(ec.SECP256R1())
        public = self.key.public_key().public_numbers()
        raw = public.x.to_bytes(32, 'big') + public.y.to_bytes(32, 'big')
        self.keyring = Path(self.tmp.name) / 'keyring.h'
        self.keyring.write_text('{1, {' + ','.join(f'0x{x:02x}' for x in raw) + '}}')
        self.commit = 'a' * 40

    def payload(self, variant):
        image = bytearray(384)
        image[0] = 0xe9
        struct.pack_into('<I', image, 32, 0xabcd5432)
        image[288:320] = b'BRY88AB151K'.ljust(32, b'\0')
        image[320:352] = ('PHICOMM-DAIKIN-1:' + variant).encode().ljust(32, b'\0')
        return image

    def signed(self, image):
        return verify.package.sign(bytes(image), 'daikin', 'v1.0.0', self.commit, self.key)

    def check(self, signed, variant):
        return verify.verify(signed, 'daikin', 'v1.0.0', self.commit, variant, self.keyring)

    def test_both_variants_are_signed_and_reject_each_other(self):
        for variant, other in [('full', 'hub'), ('hub', 'full')]:
            signed = self.signed(self.payload(variant))
            self.assertEqual(self.check(signed, variant)['payload_size'], 384)
            with self.assertRaisesRegex(ValueError, 'variant identity'):
                self.check(signed, other)

    def test_marker_must_be_at_the_fixed_offset(self):
        for offset in (0, 32, 288, 320, 351):
            image = self.payload('hub')
            image[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, 'identity'):
                self.check(self.signed(image), 'hub')
        image = self.payload('hub')
        image.extend(image[320:352])
        image[320:352] = bytes(32)
        with self.assertRaisesRegex(ValueError, 'identity'):
            self.check(self.signed(image), 'hub')

    def test_signature_digest_and_version_are_checked_before_identity(self):
        signed = bytearray(self.signed(self.payload('hub')))
        signed[-1] ^= 1
        with self.assertRaisesRegex(ValueError, 'digest'):
            self.check(bytes(signed), 'hub')
        signed = self.signed(self.payload('hub'))
        with self.assertRaisesRegex(ValueError, 'version or commit'):
            verify.verify(signed, 'daikin', 'v1.0.1', self.commit, 'hub', self.keyring)

    def test_historical_verification_does_not_require_new_variant_marker(self):
        image = self.payload('full')
        image[320:352] = bytes(32)
        verify.verify(self.signed(image), 'daikin', 'v1.0.0', self.commit, keyring_path=self.keyring)
        with self.assertRaisesRegex(ValueError, 'variant identity'):
            self.check(self.signed(image), 'full')


if __name__ == '__main__':
    unittest.main()
