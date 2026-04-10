# Patch to replace expand_tables in scene.py
# Just change the hierarchy count - no data insertion
import struct

def expand_tables_simple(self, new_count):
    old_count = self._hier_count
    if new_count <= old_count:
        return
    struct.pack_into("<I", self._data, self._hier_count_offset, new_count)
    self._hier_count = new_count
