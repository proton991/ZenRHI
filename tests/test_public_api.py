"""Regression for Foundation type discovery and public declaration scope."""
from pathlib import Path
import re
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from check_public_api import foundation_types, violations


class PublicAPITest(unittest.TestCase):
    def test_new_foundation_types_in_public_declarations(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "Foundation.h").write_text("""
                namespace zen::storage {
                    template<class T> class FutureContainer {};
                    using Queue = FutureContainer<int>;
                }
            """)
            names = foundation_types(root)
            types = re.compile(r"\b(?:" + "|".join(sorted(names)) + r")\b")
            header = root / "RHI.h"
            header.write_text("""
                namespace zen::rhi {
                    struct Descriptor {
                        bool Queue();
                        explicit operator Queue() const;
                        FutureContainer<int> attachments{};
                        void Submit(int count = 1, storage::Queue* queue = nullptr);
                        using Alias = storage::Queue;
                        void Reset() { FutureContainer<int> temporary; }
                    private:
                        storage::Queue scratch;
                    };
                }
            """)
            leaks = list(violations(header, types))
        self.assertEqual([leak.split(" exposes ")[1].split(" in ")[0] for leak in leaks],
                         ["Queue", "FutureContainer", "Queue", "Queue"])


if __name__ == "__main__":
    unittest.main()
