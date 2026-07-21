#!/usr/bin/env python3
import contextlib
import io
import types
import unittest

import source_audit as audit


class SourceAuditRedTests(unittest.TestCase):
    def test_overaligned_member_red_fixture_is_rejected(self):
        args = types.SimpleNamespace(path=str(audit.HERE / "fixtures/red/overaligned_member.c"))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(audit.audit_overaligned(args), 1)

    def test_raw_savestate_without_full_stamp_is_rejected(self):
        raw = "bool SaveState(){fwrite(&state,sizeof state,1,f);} bool LoadState(){fread(&state,sizeof state,1,f);}" 
        stamped = raw + " enum { STATE_MAGIC=1, STATE_VERSION=2 }; size_t payload_len;"
        self.assertFalse(audit.savestate_stamped(raw))
        self.assertTrue(audit.savestate_stamped(stamped))

    def test_positional_digest_changes_on_reorder(self):
        fields = ["s_a", "s_b", "s_c"]
        self.assertNotEqual(audit.digest(fields), audit.digest(["s_a", "s_c", "s_b"]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
