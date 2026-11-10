import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('route', ROOT / 'tools/check_retail_rnps_route.py')
route = importlib.util.module_from_spec(spec)
spec.loader.exec_module(route)


class RouteTest(unittest.TestCase):
    def test_route_and_negative_controls(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            app = root / 'apps/NPXS40008'
            app.mkdir(parents=True)
            (app / 'manifest.json').write_text(json.dumps({'titleId': 'NPXS40008', 'applicationName': 'rnps-settings'}))
            (app / 'application.ps.bundle').write_bytes(b'RNPSHEDR')
            cache = root / 'cache.json'
            data = {'response': {'scheme': {'default': {'pssettings': 'app:rnps-settings'}},
                                 'app': {'rnps-settings': {'default': {'rnps': {'titleId': 'NPXS40008', 'component': 'Main'}}}}}}
            cache.write_text(json.dumps(data))
            result = route.inspect(cache, root / 'apps')
            self.assertEqual(result['title_id'], 'NPXS40008')
            self.assertTrue(result['bundle_is_rnps_container'])
            self.assertFalse(result['initialsetup_route_present'])
            data['response']['scheme']['default'].clear()
            cache.write_text(json.dumps(data))
            with self.assertRaises(ValueError):
                route.inspect(cache, root / 'apps')
            data['response']['scheme']['default']['pssettings'] = 'app:rnps-settings'
            cache.write_text(json.dumps(data))
            (app / 'manifest.json').write_text(json.dumps({'titleId': 'NPXS40008', 'applicationName': 'unrelated'}))
            with self.assertRaises(ValueError):
                route.inspect(cache, root / 'apps')


if __name__ == '__main__':
    unittest.main()
