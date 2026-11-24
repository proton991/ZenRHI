"""Regression for Foundation type discovery and public declaration scope."""
from pathlib import Path
import re
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from check_public_api import foundation_types, incompatible_changes, signatures, violations


class PublicAPITest(unittest.TestCase):
    def test_tag_diff_preserves_member_ownership_and_enum_values(self):
        old = "struct A { void f(); }; struct B {}; enum class E { First, Second };"
        moved = "struct A {}; struct B { void f(); }; enum class E { Inserted, First, Second };"
        self.assertIn(("::A", "voidf()"), signatures(old) - signatures(moved))
        self.assertIn(("::E", "First=0"), signatures(old) - signatures(moved))
        self.assertFalse(signatures(old) - signatures(old.replace("Second", "Second, Third")))

    def test_renamed_parameters_and_new_implementation_requirements(self):
        old = "struct Device { virtual void poll(unsigned int frames, const ns::Options& opts) = 0; };"
        renamed = old.replace("frames", "count").replace("opts)", "options)")
        self.assertFalse(incompatible_changes(old, renamed))
        self.assertTrue(incompatible_changes(old, old.replace("unsigned int", "float")))
        self.assertTrue(incompatible_changes(old, old.replace("};", "private: virtual void reset() = 0; };")))
        self.assertFalse(incompatible_changes(old, old + "struct New { virtual void reset() = 0; };"))
        self.assertFalse(incompatible_changes(old, old.replace("};", "void (*callback)(int) = 0; };")))
        self.assertTrue(incompatible_changes(old, old.replace("};", "virtual void bind(void (*callback)(int)) = 0; };")))

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
