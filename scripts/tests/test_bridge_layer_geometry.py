"""The offline bridge publishes the native bounded geometry query contract."""
import unittest
from tests.bridge_test_support import load_manifest


class LayerGeometryCatalogTests(unittest.TestCase):
    def test_geometry_catalog_has_explicit_selectors_and_bounded_cursor(self):
        tools = {tool["name"]: tool for tool in load_manifest()["server_tools"]}
        tool = tools["get_layer_geometry"]
        schema = tool["inputSchema"]
        self.assertFalse(schema["additionalProperties"])
        self.assertEqual(schema["required"], ["plate_index"])
        props = schema["properties"]
        self.assertEqual(props["max_points"]["maximum"], 8192)
        self.assertEqual(props["max_points"]["default"], 512)
        self.assertFalse(props["cursor"]["additionalProperties"])
        self.assertEqual(set(props["cursor"]["required"]), {
            "result_id", "layer_digest", "layer", "features", "filaments", "next_move"})
        self.assertEqual(set(props["features"]["items"]["enum"]), {
            "perimeters", "infill", "support", "support_interface", "brim", "skirt", "prime_tower"})
        self.assertIn("slice_busy", tool["description"])
        self.assertIn("64 KiB", tool["description"])
