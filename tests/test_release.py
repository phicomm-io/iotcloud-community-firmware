import hashlib,importlib.util,json,sys,tempfile,unittest
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric import ec
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/release'))
from notes import extract
from catalog import make
from products import asset_stem, targets
import verify
class ReleaseTests(unittest.TestCase):
 def current_assets(self,root):
  key=ec.generate_private_key(ec.SECP256R1())
  public=key.public_key().public_numbers()
  raw=public.x.to_bytes(32,'big')+public.y.to_bytes(32,'big')
  keyring=root/'test-keyring.h'
  keyring.write_text('{1, {'+','.join(f'0x{x:02x}' for x in raw)+'}}')
  for model,variant in targets('v1.0.0'):
   stem=asset_stem(model,variant)
   for suffix in ['ota.bin.signed','ota.bin.raw','all.bin']:
    (root/f'{stem}.{suffix}').write_bytes((model+variant+suffix).encode())
   if model=='daikin':
    payload=bytearray(384)
    payload[0]=0xe9
    payload[32:36]=(0xabcd5432).to_bytes(4,'little')
    payload[288:320]=b'BRY88AB151K'.ljust(32,b'\0')
    payload[320:352]=('PHICOMM-DAIKIN-1:'+variant).encode().ljust(32,b'\0')
    (root/f'{stem}.ota.bin.raw').write_bytes(payload)
    (root/f'{stem}.ota.bin.signed').write_bytes(verify.package.sign(bytes(payload),'daikin','v1.0.0','a'*40,key))
  return key,keyring
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
    self.assertNotIn('firmware_variant',model)
   (root/'phicomm_a1.ota.bin.signed').write_bytes(b'')
   with self.assertRaises(ValueError):make('v0.9.5','a'*40,root,'notes')
 def test_nine_historical_variants_are_required_and_daikin_has_only_full(self):
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
   self.assertNotIn('firmware_variant',r['models']['daikin'])
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
 def test_ten_current_variants_require_independent_complete_assets(self):
  current=list(targets('v1.0.0'))
  self.assertEqual(len(current),10)
  self.assertEqual(set(current)-set(targets('v0.9.24')),{('daikin','hub')})
  self.assertNotIn(('daikin','no-frontend-resources'),current)
  with tempfile.TemporaryDirectory() as tmp:
   root=Path(tmp)
   _,keyring=self.current_assets(root)
   release=make('v1.0.0','a'*40,root,'v1 release notes',require_variants=True,keyring_path=keyring)
   hub=release['models']['daikin']['variants']['hub']
   self.assertEqual(release['models']['daikin']['firmware_variant'],'full')
   self.assertEqual(hub['firmware_variant'],'hub')
   self.assertIn('/daikin_BRY88AX151K-hub.',hub['path'])
   self.assertNotEqual(hub['sha256'],release['models']['daikin']['sha256'])
   for suffix in ['ota.bin.signed','ota.bin.raw','all.bin']:
    with self.subTest(missing=suffix):
     asset=root/f'daikin_BRY88AX151K-hub.{suffix}'
     body=asset.read_bytes()
     asset.unlink()
     with self.assertRaises(FileNotFoundError):make('v1.0.0','a'*40,root,'notes',require_variants=True,keyring_path=keyring)
     asset.write_bytes(body)
   for model,variant in [('tc1','hub'),('m1','fresh-air'),('daikin','no-frontend-resources'),('unknown','full')]:
    with self.subTest(model=model,variant=variant),self.assertRaises(ValueError):asset_stem(model,variant)
 def test_current_catalog_authenticates_actual_daikin_labels_and_raw_payload(self):
  with tempfile.TemporaryDirectory() as tmp:
   root=Path(tmp)
   key,keyring=self.current_assets(root)
   full=root/f'{asset_stem("daikin","full")}.ota.bin.signed'
   raw=root/f'{asset_stem("daikin","full")}.ota.bin.raw'
   signed=full.read_bytes();payload=raw.read_bytes()
   hub=(root/f'{asset_stem("daikin","hub")}.ota.bin.signed').read_bytes()
   cases=[(hub,payload,'variant identity'),
          (signed,hub[192:],'raw image differs'),
          (signed[:128]+bytes(64)+signed[192:],payload,'signature')]
   old=bytearray(payload);old[320:352]=bytes(32)
   cases.append((verify.package.sign(bytes(old),'daikin','v1.0.0','a'*40,key),bytes(old),'variant identity'))
   for package,image,error in cases:
    with self.subTest(error=error):
     full.write_bytes(package);raw.write_bytes(image)
     with self.assertRaisesRegex(ValueError,error):
      make('v1.0.0','a'*40,root,'notes',require_variants=True,keyring_path=keyring)
   full.write_bytes(signed);raw.write_bytes(payload)
   self.assertEqual(make('v1.0.0','a'*40,root,'notes',require_variants=True,keyring_path=keyring)['tag'],'v1.0.0')
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
