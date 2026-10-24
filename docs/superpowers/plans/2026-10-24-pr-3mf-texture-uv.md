TITLE: Keep a part's UV channel in the 3MF, so model-UV texture layers survive a save

### Contributing back

Hi! We maintain [OrcaMCP](https://github.com/okets/OrcaMCP), a fork of OrcaSlicer that follows your releases closely. What we do is take this amazing software and open it up for smooth use by AI agents: an MCP server that gives them proper tools for everything in the slicer, along with rich metadata about the project and rendered views, so they can see what they are doing.

While giving agents tools for the new texture displacement, we found that one of its projections does not survive saving a project, and wanted to pass back the change we made. It touches only the 3MF reader and writer and one line of the texture layers' reader, plus tests. We'd love to hear what you think, and whether it fits the way you'd want to solve it.

### What a user sees

**What happens.** Load a textured OBJ (with its MTL), add a texture displacement layer and choose the "Model UV" projection: the texture follows the model's own UV map. Save the project and open it again: the layer is still there, but the part has no UV map any more, so the layer falls back to triplanar projection and the texture lands differently. Bake it then, and the print is not what you set up.

**Who it affects.** Anyone using the Model UV projection on a saved project: the UV map lives only in memory, from the OBJ load until the app closes.

**Why it happens.** Two things:

- The 3MF writer stores each part's mesh, paint and texture masks, but not `ModelVolume::texture_uv`, so the reader has nothing to restore.
- The layer stack's reader clamps `projection_method` to 0..4, and Model UV is 5. So a saved Model UV layer comes back as a "From view" layer, even before the UVs matter.

Where: `_BBS_3MF_Exporter::_add_mesh_to_object_stream` and `_BBS_3MF_Importer::_handle_start_triangle` in `src/libslic3r/Format/bbs_3mf.cpp`; `texture_displacement_layers_from_json` in `src/libslic3r/TextureDisplacement.cpp`.

### What we changed

The writer adds one attribute per triangle, `texture_uv="u1 v1 u2 v2 u3 v3"`, next to `paint_color` and the texture masks, only for a part that has a UV channel. The reader takes the channel only when every triangle of the part has a valid one, swaps the corners with the mesh when it flips an inside-out mesh, and gives a volume that shares a mesh the source volume's channel (the writer shares a mesh only while the channels are equal too). Files without the attribute load exactly as before, and older builds, Bambu Studio and PrusaSlicer ignore it, as they ignore `paint_color`.

We looked at the 3MF Materials extension's `texture2dgroup` first. It needs a `texture2d` image in the package, which a part with a UV map but no texture layer does not have; other programs would show that image as the part's surface colour, which is wrong for a height map; and a triangle can carry only one `pid`. So we followed the `paint_*` convention instead. If you would rather have the standard form, we're happy to rework it.

The layer reader now clamps to `TextureProjectionMethod::ModelUV`, the last projection, so a Model UV layer keeps its projection.

Our fork's commit: https://github.com/okets/OrcaMCP/commit/4d05bd48364c7a783aefeb971ac8d8ad17fe7b84 (branch `feat/3mf-texture-uv`, one commit on your current main)

Feel free to take it as is or adapt it. If you see a better way, or something we missed, we're happy to talk it through.

### Tests

New cases in `tests/libslic3r/test_3mf.cpp` (`[3mf]`): a part's channel round trip in both save strategies, a two-part object where one part has UVs and one does not, a file with no UVs, an inside-out mesh flipped on load, shared meshes with equal and with different channels, and partial or malformed values (all refused, the part loads without UVs). And a Model UV layer that keeps its projection through a save and reload.

Built on your current main (37115565ff), Release, `libslic3r_tests`: `[3mf]` 53 test cases (2220 assertions) and `[TextureDisplacement]` 79 test cases (8265 assertions) all pass. In our fork the same code also passed a live check: a textured OBJ with a Model UV layer saved, reopened and baked as before the save.
