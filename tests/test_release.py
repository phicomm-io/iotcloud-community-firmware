import hashlib,importlib.util,json,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/release'))
from notes import extract
from catalog import make
class ReleaseTests(unittest.TestCase):
 def test_notes_boundaries(self):
  text='# Log\n## v0.9.5\n\n- A meaningful version-specific change.\n## v0.9.4\n\n- Previous version remains archived.\n'
  self.assertNotIn('Previous',extract(text,'v0.9.5'))
  for value in ['## v0.9.5\nTODO',text+text,'## v0.9.4\nold']:
   with self.assertRaises(ValueError):extract(value,'v0.9.5')
 def test_catalog_integrity_and_relative_paths(self):
  with tempfile.TemporaryDirectory() as tmp:
   root=Path(tmp)
   for product in ['tc1_a1','m1','a1']:
    for suffix in ['ota.bin.signed','ota.bin.raw','all.bin']:(root/f'phicomm_{product}.{suffix}').write_bytes((product+suffix).encode())
   r=make('v0.9.5','a'*40,root,'Release notes')
   self.assertEqual(set(r['models']),{'tc1','m1','a1'})
   for model in r['models'].values():
    self.assertTrue(model['path'].startswith('/releases/v0.9.5/'))
    self.assertEqual(len(model['sha256']),64)
   (root/'phicomm_a1.ota.bin.signed').write_bytes(b'')
   with self.assertRaises(ValueError):make('v0.9.5','a'*40,root,'notes')
 def test_nine_variants_are_required_and_daikin_has_only_full(self):
  with tempfile.TemporaryDirectory() as tmp:
   root=Path(tmp)
   for product in ['tc1_a1','m1','a1','daikin']:
    for variant in ([''] if product=='daikin' else ['', '-no-frontend-resources']) + (['-fresh-air','-fresh-air-no-frontend-resources'] if product=='a1' else []):
     for suffix in ['ota.bin.signed','ota.bin.raw','all.bin']:
      (root/f'phicomm_{product}{variant}.{suffix}').write_bytes((product+variant+suffix).encode())
   r=make('v0.9.5','a'*40,root,'Notes',require_variants=True)
   self.assertEqual(set(r['models']),{'tc1','m1','a1','daikin'})
   self.assertNotIn('variants',r['models']['daikin'])
   for key,model in r['models'].items():
    if key=='daikin':continue
    slim=model['variants']['no-frontend-resources']
    self.assertEqual(slim['firmware_variant'],'no-frontend-resources')
    self.assertIn('-no-frontend-resources.',slim['path'])
    self.assertNotEqual(slim['sha256'],model['sha256'])
   (root/'phicomm_a1-no-frontend-resources.ota.bin.raw').unlink()
   with self.assertRaises(FileNotFoundError):make('v0.9.5','a'*40,root,'Notes',require_variants=True)
   (root/'phicomm_a1-no-frontend-resources.ota.bin.raw').write_bytes(b'fixture')
   (root/'phicomm_daikin.all.bin').unlink()
   with self.assertRaises(FileNotFoundError):make('v0.9.5','a'*40,root,'Notes',require_variants=True)
if __name__=='__main__':unittest.main()
