# Gang tag count in saves

Group `gang-tags-savegame` (byte_patch at 0x5d3da8).

**Bug.** The tag count is loaded with a different routine than the tags, so extra tags added by mods
don't load.

**Port.** The count is loaded with the same routine as the tags (0x5d1300), as SilentPatch.

**Reproduction.** Not scripted yet (needs a save with extra tags).

**Result.** Lifted and converted.
