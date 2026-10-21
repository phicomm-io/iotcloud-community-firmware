import unittest
from unittest.mock import patch

from tests.host_models import DEFAULT_MODELS, selected_models


class SelectedModelsTest(unittest.TestCase):
    def test_default_preserves_full_ci_order(self):
        with patch.dict("os.environ", {}, clear=True):
            self.assertEqual(selected_models(), DEFAULT_MODELS)

    def test_explicit_selection_preserves_requested_order(self):
        self.assertEqual(selected_models("A1,TC1"), ("A1", "TC1"))
        self.assertEqual(selected_models("TC1,A1"), ("TC1", "A1"))

    def test_a1_tc1_selection_excludes_m1_sentinel(self):
        m1_sentinel = "M1"
        self.assertNotIn(m1_sentinel, selected_models("A1,TC1"))

    def test_rejects_empty_unknown_duplicate_and_whitespace(self):
        for value in ("", ",A1", "A1,", "A1,,TC1", "a1", "A1,M2", "A1,A1", "A1, TC1"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                selected_models(value)


if __name__ == "__main__":
    unittest.main()
