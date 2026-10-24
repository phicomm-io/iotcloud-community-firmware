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
    stem = 'daikin_BRY88AX151K' if product == 'daikin' else 'phicomm_' + product
    for variant in ([''] if product=='daikin' else ['', '-no-frontend-resources']) + (['-fresh-air','-fresh-air-no-frontend-resources'] if product=='a1' else []):
     for suffix in ['ota.bin.signed','ota.bin.raw','all.bin']:
      (root/f'{stem}{variant}.{suffix}').write_bytes((product+variant+suffix).encode())
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
   (root/'daikin_BRY88AX151K.all.bin').unlink()
   with self.assertRaises(FileNotFoundError):make('v0.9.5','a'*40,root,'Notes',require_variants=True)
 def test_daikin_asset_names_reach_the_catalog_and_device_manifest(self):
  import subprocess
  from archive_pages import write_latest
  tool=Path(__file__).resolve().parents[1]/'tools/release/products.py'
  stem=subprocess.check_output([sys.executable,str(tool),'daikin'],text=True).strip()
  self.assertEqual(stem,'daikin_BRY88AX151K')
  with tempfile.TemporaryDirectory() as tmp:
   root=Path(tmp)
   for prefix in ['phicomm_tc1_a1','phicomm_m1','phicomm_a1',stem]:
    for suffix in ['ota.bin.signed','ota.bin.raw','all.bin']:
     (root/f'{prefix}.{suffix}').write_bytes((prefix+suffix).encode())
   release=make('v0.9.5','a'*40,root,'Release notes')
   self.assertEqual(set(release['models']),{'tc1','m1','a1','daikin'})
   downloads=release['models']['daikin']['downloads']
   for kind,suffix in [('signed','ota.bin.signed'),('raw','ota.bin.raw'),('full','all.bin')]:
    asset=downloads[kind]
    self.assertEqual(asset['path'],f'/releases/v0.9.5/daikin_BRY88AX151K.{suffix}')
    self.assertEqual(asset['sha256'],hashlib.sha256((root/Path(asset['path']).name).read_bytes()).hexdigest())
   write_latest({'stable':'v0.9.5','releases':[release]},root)
   latest=json.loads((root/'ota/daikin/latest.json').read_text())
   self.assertEqual(latest['model'],'daikin')
   self.assertEqual(latest['path'],downloads['signed']['path'])
   self.assertEqual(latest['sha256'],downloads['signed']['sha256'])
   for suffix in ['ota.bin.signed','ota.bin.raw','all.bin']:
    with self.subTest(missing=suffix):
     asset=root/f'{stem}.{suffix}'
     body=asset.read_bytes()
     asset.unlink()
     with self.assertRaises(FileNotFoundError):make('v0.9.5','a'*40,root,'Release notes')
     asset.write_bytes(body)
   (root/'phicomm_daikin.ota.bin.signed').write_bytes(b'old name')
   with self.assertRaisesRegex(ValueError,'daikin_BRY88AX151K'):
    make('v0.9.5','a'*40,root,'Release notes')
 def test_historical_daikin_assets_retain_their_original_names(self):
  with tempfile.TemporaryDirectory() as tmp:
   root=Path(tmp)
   for product in ['tc1_a1','m1','a1','daikin']:
    for suffix in ['ota.bin','all.bin']:
     (root/f'phicomm_{product}.{suffix}').write_bytes(b'Historical unsigned image')
   release=make('v0.9.5','a'*40,root,'Notes',legacy=True)
   self.assertEqual(release['models']['daikin']['path'],'/releases/v0.9.5/phicomm_daikin.ota.bin')

if __name__=='__main__':unittest.main()
