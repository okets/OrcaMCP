"""The offline catalog preserves the native analysis section and paging contract."""
import unittest
from tests.bridge_test_support import load_manifest


class PlateAnalysisCatalogTests(unittest.TestCase):
    def test_analysis_catalog_has_bounded_detail_and_single_page_summary(self):
        tools = {tool["name"]: tool for tool in load_manifest()["server_tools"]}
        tool = tools["get_plate_analysis"]
        schema = tool["inputSchema"]
        self.assertFalse(schema["additionalProperties"])
        self.assertEqual(schema["required"], ["plate_index"])
        props = schema["properties"]
        self.assertEqual(props["section"]["default"], "summary")
        self.assertEqual(props["max_moves"]["minimum"], 5)
        self.assertEqual(props["max_moves"]["maximum"], 10000)
        self.assertFalse(props["cursor"]["additionalProperties"])
        self.assertEqual(set(props["cursor"]["required"]), {
            "result_id", "section", "total_moves", "tail_gcode_id", "next_move"})
        self.assertIn("64 KiB", tool["description"])
        self.assertIn("slice_busy", tool["description"])
        self.assertIn("not proven slice-time binding", tool["description"])
