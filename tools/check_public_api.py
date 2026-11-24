"""Check public RHI declarations against types declared by the selected Foundation SDK."""
import argparse
from pathlib import Path
import re
import sys
import subprocess
from collections import Counter

ROOT = Path(__file__).resolve().parents[1]

TOKENS = re.compile(r"//[^\n]*|/\*.*?\*/|^[ \t]*\#(?:[^\n\\]|\\[^\n]|\\\n)*|"
                    r'"(?:\\.|[^"\\])*"|(?:public|protected|private)\s*:|[(){};]', re.S | re.M)
TYPE = re.compile(r"(enum(?:\s+(?:class|struct))?|class|struct|union)\s+(\w+)")
NAMESPACE = re.compile(r"(?:inline\s+)?namespace(?:\s+[\w:]+)?$")
HANDLE = re.compile(r"using\s+RHI\w*Ptr\s*=\s*(?:RefCountPtr|SharedPtr)\s*<\s*(?:T|RHI\w+)"
                    r"(?:\s*,\s*(?:RHIResourceRefCountPolicy|MultiThreadCounter))?\s*>$")


def without_templates(text):
    while re.match(r"template\s*<", text):
        depth = 1
        for i in range(text.index("<") + 1, len(text)):
            depth += (text[i] == "<") - (text[i] == ">")
            if depth == 0:
                text = text[i + 1:].strip()
                break
        else:
            break
    return text


def pure_virtual(decl):
    # A pointer-to-function data member can also have a zero initializer.
    return (re.search(r"\)\s*[^{};=]*=\s*0$", decl) is not None
            and not re.match(r"\s*(?:[\w:]+::)?[*&]", decl.partition("(")[2]))


def declarations(source, qualified=False):
    """Yield namespace and public member declarations, skipping comments and inline bodies."""
    scopes = [("namespace", True, "")]
    declaration = ""
    end = parentheses = 0
    for token in TOKENS.finditer(source):
        declaration += source[end:token.start()]
        end = token.end()
        value = token[0]
        if value.startswith(("//", "/*")) or value.lstrip().startswith("#"):
            declaration += " "
        elif value.startswith('"'):
            declaration += '""'
        elif value in ("(", ")"):
            parentheses += 1 if value == "(" else -1
            declaration += value
        elif parentheses:
            declaration += value
        elif value.endswith(":"):
            scopes[-1] = (scopes[-1][0], value.startswith("public"), scopes[-1][2])
            declaration = ""
        else:
            kind, public, owner = scopes[-1]
            text = without_templates(declaration.strip())
            type_decl = TYPE.match(text)
            pure = qualified and kind == "type" and pure_virtual(text)
            if kind in ("type", "namespace") and (public or pure) and value in (";", "{") and text:
                yield owner if qualified else kind, declaration.strip(), source.count("\n", 0, token.start()) + 1
            if qualified and kind == "enum" and value == "}":
                # An implicitly valued member depends on its predecessor. Appending a member is compatible.
                previous = ""
                for member in declaration.split(","):
                    member = member.strip()
                    if member:
                        name, explicit, _ = member.partition("=")
                        yield owner, member if explicit else f"{name}={previous}+1" if previous else f"{name}=0", 0
                        previous = name.strip()
            if value == "{":
                if kind == "body" or not public:
                    scopes.append(("body", False, owner))
                elif NAMESPACE.fullmatch(text):
                    scopes.append(("namespace", True, owner + "::" + text.split("namespace", 1)[1].strip()))
                elif type_decl and not type_decl[1].startswith("enum"):
                    scopes.append(("type", type_decl[1] != "class", owner + "::" + type_decl[2]))
                elif qualified and type_decl:
                    scopes.append(("enum", True, owner + "::" + type_decl[2]))
                else:
                    scopes.append(("body", False, owner))
            elif value == "}" and len(scopes) > 1:
                scopes.pop()
            declaration = ""


def headers(include):
    return sorted(path for path in include.rglob("*") if path.suffix in {".h", ".hpp", ".inl"})


def foundation_types(include):
    names = set()
    for path in headers(include):
        for scope, text, _ in declarations(path.read_text(encoding="utf-8")):
            if scope == "namespace":
                text = without_templates(text)
                decl = TYPE.match(text) or re.match(r"(using|concept)\s+(\w+)\s*=", text)
                if decl and not text[decl.end():].startswith("::"):
                    names.add(decl[2])
                elif text.startswith("typedef "):
                    alias = re.search(r"\(\s*\*\s*(\w+)\s*\)|(\w+)$", text)
                    if alias:
                        names.add(alias[1] or alias[2])
    return names


def violations(path, types):
    for _, declaration, line in declarations(path.read_text(encoding="utf-8")):
        text = without_templates(declaration)
        template = declaration[:len(declaration) - len(text)]
        # Implementation base classes are allowed. Inspect the derived class's own declaration.
        signature = re.split(r"(?<!:):(?!:)", text, maxsplit=1)[0] if TYPE.match(text) else text
        signature = template + ("" if HANDLE.fullmatch(text) else signature)
        for match in types.finditer(signature):
            # A member function can share a name with a Foundation type (e.g. bool Queue()).
            prefix = signature[:match.start()].rstrip()
            if (re.match(r"\s*\(", signature[match.end():]) and prefix and "(" not in prefix
                    and not re.search(r"\boperator$", prefix)):
                continue
            yield f"{path.name}:{line} exposes {match[0]} in {' '.join(text.split())[:140]}"


PARAMETER_WORDS = set("const volatile signed unsigned short long int char wchar_t char8_t char16_t char32_t "
                      "bool float double void auto typename class struct enum".split())


def parameter_type(parameter):
    decl, default, value = parameter.partition("=")
    # A function pointer or reference names its parameter inside the declarator.
    decl = re.sub(r"\(\s*([*&])\s*[A-Za-z_]\w*\s*\)", r"(\1)", decl.strip())
    match = re.search(r"([A-Za-z_]\w*)\s*((?:\[[^\]]*\]\s*)*)$", decl)
    if match and match[1] not in PARAMETER_WORDS:
        prefix = decl[:match.start()].rstrip()
        if (prefix and not prefix.endswith("::") and
                re.sub(r"\b(?:const|volatile|typename|class|struct|enum)\b", "", prefix).strip()):
            decl = prefix + match[2]
    return decl + default + value


def signature(decl):
    # Canonicalize ordinary function parameters while preserving return types, qualifiers,
    # array bounds and default expressions for the source-compatibility review.
    start = decl.find("(")
    if start >= 0 and not re.match(r"\s*(?:[\w:]+::)?[*&]", decl[start + 1:]):
        depth = 0
        begin = start + 1
        params = []
        for i in range(begin, len(decl)):
            ch = decl[i]
            if depth == 0 and ch in ",)":
                params.append(parameter_type(decl[begin:i]))
                begin = i + 1
                if ch == ")":
                    decl = decl[:start + 1] + ",".join(params) + decl[i:]
                    break
            elif ch in "(<[{":
                depth += 1
            elif ch in ")>]}":
                depth = max(0, depth - 1)
    return re.sub(r"\s+", "", decl)


def signatures(source):
    return Counter((owner, signature(decl)) for owner, decl, _ in declarations(source, qualified=True))


def incompatible_changes(old, new):
    before, after = signatures(old), signatures(new)
    changes = [f"changed {scope} declaration {decl}" for scope, decl in (before - after)]
    old_types = {scope + "::" + match[2] for scope, decl, _ in declarations(old, qualified=True)
                 if (match := TYPE.match(without_templates(decl)))}
    added = after - before
    for scope, decl, _ in declarations(new, qualified=True):
        if scope in old_types and pure_virtual(decl) and (scope, signature(decl)) in added:
            changes.append(f"added pure virtual {scope} declaration {decl}")
    return changes


def compare_tag(tag):
    # Keep the original unified header diff in the job log as well. This captures changes
    # to enums, access, layouts and templates outside the declaration scanner's coverage.
    subprocess.run(["git", "diff", tag, "--", "include/ZenRHI"], cwd=ROOT, check=True)
    tracked = subprocess.check_output(["git", "ls-tree", "-r", "--name-only", tag, "include/ZenRHI"],
                                      cwd=ROOT, text=True).splitlines()
    removed = []
    for name in tracked:
        if Path(name).suffix not in {".h", ".hpp", ".inl"}:
            continue
        baseline = subprocess.check_output(["git", "show", f"{tag}:{name}"], cwd=ROOT).decode("utf-8")
        current = (ROOT / name).read_text(encoding="utf-8") if (ROOT / name).exists() else ""
        removed.extend(f"{name} {change}" for change in incompatible_changes(baseline, current))
    return removed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--foundation-include", type=Path, nargs="+", default=[ROOT.parent / "ZenFoundation/include"])
    parser.add_argument("--previous-tag", help="Compare installed declarations to this Git tag")
    args = parser.parse_args()
    include = next((p / "ZenFoundation" for p in args.foundation_include
                    if (p / "ZenFoundation/Templates").is_dir()), None)
    if include is None:
        parser.error("Supply --foundation-include pointing at the ZenFoundation SDK include directory")
    names = foundation_types(include)
    if not names:
        parser.error(f"No Foundation types found in {include}")
    types = re.compile(r"\b(?:" + "|".join(sorted(names)) + r")\b")
    problems = [v for path in headers(ROOT / "include/ZenRHI") for v in violations(path, types)]
    if args.previous_tag:
        changes = compare_tag(args.previous_tag)
        current = re.search(r"project\(ZenRHI VERSION (\d+)", (ROOT / "CMakeLists.txt").read_text())[1]
        if int(current) >= 1 and int(current) <= int(args.previous_tag.lstrip("v").split(".")[0]):
            problems.extend(changes)
        else:
            print("\n".join(changes))
    for problem in problems:
        print(problem)
    return bool(problems)


if __name__ == "__main__":
    sys.exit(main())
