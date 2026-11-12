"""Embedded resource trimming must preserve selectors that can match native UI."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from pack_frontend import trim_screen_styles


class NativeStyles(unittest.TestCase):
    def test_owned_classes_removed_but_shared_groups_and_negations_survive(self):
        css = '.screen-editor,.card{color:red}.panel .screen-card:hover{color:blue}' \
              '.co2screen-mode{color:green}:not(.screen-editor){display:block}' \
              ':is(.screen-editor,.card){margin:0}[class="screen-editor"]{padding:0}'
        result = trim_screen_styles(css)
        self.assertTrue(result.startswith('.card{color:red}'))
        self.assertIn('.card{color:red}', result)
        self.assertNotIn('color:blue', result)
        for selector in ('.co2screen-mode', ':not(.screen-editor)', ':is(.screen-editor,.card)', '[class="screen-editor"]'):
            self.assertIn(selector, result)

    def test_nested_responsive_rules_preserve_native_declarations_and_keyframes(self):
        css = '@media(max-width:600px){.screen-preview{width:100%}.card{width:90%}}' \
              '@supports(display:grid){.screen-grid{display:grid}.grid{display:grid}}' \
              '@keyframes pulse{from{opacity:0}to{opacity:1}}'
        result = trim_screen_styles(css)
        self.assertNotIn('screen-', result)
        self.assertIn('.card{width:90%}', result)
        self.assertIn('.grid{display:grid}', result)
        self.assertIn('@keyframes pulse{from{opacity:0}to{opacity:1}}', result)


if __name__ == '__main__':
    unittest.main()
