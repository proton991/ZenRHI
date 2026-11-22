"""Check public RHI declarations against types declared by the selected Foundation SDK."""
import argparse
from pathlib import Path
import re
import sys

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


def declarations(source):
    """Yield namespace and public member declarations, skipping comments and inline bodies."""
    scopes = [("namespace", True)]
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
            scopes[-1] = (scopes[-1][0], value.startswith("public"))
            declaration = ""
        else:
            kind, public = scopes[-1]
            text = without_templates(declaration.strip())
            type_decl = TYPE.match(text)
            if kind in ("type", "namespace") and public and value in (";", "{") and text:
                yield kind, declaration.strip(), source.count("\n", 0, token.start()) + 1
            if value == "{":
                if kind == "body" or not public:
                    scopes.append(("body", False))
                elif NAMESPACE.fullmatch(text):
                    scopes.append(("namespace", True))
                elif type_decl and not type_decl[1].startswith("enum"):
                    scopes.append(("type", type_decl[1] != "class"))
                else:
                    scopes.append(("body", False))
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--foundation-include", type=Path, nargs="+", default=[ROOT.parent / "ZenFoundation/include"])
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
    for problem in problems:
        print(problem)
    return bool(problems)


if __name__ == "__main__":
    sys.exit(main())
