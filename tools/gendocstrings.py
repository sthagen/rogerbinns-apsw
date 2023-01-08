#!/usr/bin/env python3
"""
This implements functionality similar to Argument Clinic.

* Docstrings are available as C symbols, and then used
* The syntax for __text_signature is used to give a partial type
  signature
* Argument parsing is automated
* Type stubs are generated

Argument parsing is done differently.  Generated code is placed in the
original source so normal tools can see it, but argument clinic then
uses checksums to detect modifications.  We use the simpler approach
of replacing the generated section if it differs, so git will tell if
it was modified.

Beware that this code evolved and was not intelligently designed
"""

import sys
import os
import io
import textwrap
import glob
import tempfile
import apsw
import urllib.request
import collections
import copy

from typing import Union, List

# symbols to skip because we can't apply docstrings (PyModule_AddObject doesn't take docstring)
docstrings_skip = {
    "apsw.compile_options",
    "apsw.connection_hooks",
    "apsw.keywords",
    "apsw.using_amalgamation",
    "apsw.SQLITE_VERSION_NUMBER",
}

virtual_table_classes = {"VTCursor", "VTModule", "VTTable"}


def sqlite_links():
    global funclist, consts

    basesqurl = "https://sqlite.org/"
    with tempfile.NamedTemporaryFile() as f:
        f.write(urllib.request.urlopen(basesqurl + "toc.db").read())
        f.flush()

        db = apsw.Connection(f.name)

        funclist = {}
        consts = collections.defaultdict(lambda: copy.deepcopy({"vars": []}))
        const2page = {}

        for name, type, title, uri in db.execute("select name, type, title, uri from toc"):
            if type == "function":
                funclist[name] = basesqurl + uri
            elif type == "constant":
                const2page[name] = basesqurl + uri
                consts[title]["vars"].append(name)
                consts[title]["page"] = basesqurl + uri.split("#")[0]


def get_sqlite_constant_info(name: str) -> dict:
    for title, details in consts.items():
        if name in details["vars"]:
            return {"title": title, "url": details["page"], "value": getattr(apsw, name)}
    raise ValueError(f"constant { name } not found")


def get_mapping_info(name: str) -> dict:
    # work out which mapping in consts this corresponds to
    symbols = set(k for k in getattr(apsw, name) if isinstance(k, str))
    found_in = []
    for title, details in consts.items():
        i = symbols.intersection(details["vars"])
        if i:
            found_in.append((len(i), title, details))
    found_in.sort()
    if not found_in:
        raise ValueError(f"Couldn't figure out { name }")
    f = found_in[-1]
    return {"title": f[1], "url": f[2]["page"], "members": f[2]["vars"]}


all_exc_doc = {}


def get_all_exc_doc() -> None:
    capture = None

    def proc():
        nonlocal capture
        if capture is None:
            return
        while not capture[0].strip():
            capture.pop(0)
        while not capture[-1].strip():
            capture.pop()
        doc = [f"{ line }\n" for line in textwrap.dedent("\n".join(capture)).split("\n")]
        all_exc_doc[cur_name] = doc
        capture = None

    for line in open("doc/exceptions.rst", "rt"):
        if line.startswith(f".. exception::"):
            proc()
            capture = []
            cur_name = line.split()[-1]
            continue
        if capture is not None:
            # look for non-indented line
            if line.strip() and line.lstrip() == line and not line.startswith(".. attribute::"):
                proc()
                continue
            capture.append(line.rstrip())
    proc()


def get_exc_doc(name: str) -> list[str]:
    return all_exc_doc[name]


def process_docdb(data: dict) -> list:
    res = []
    for klass, members in data.items():
        for name, docstring in members.items():
            assert docstring[0].startswith(".. ")
            if name.endswith(".<class>"):
                pass
            else:
                assert name in docstring[0]
                docstring[0] = docstring[0].replace(name, f"{ klass }.{ name }", 1)

            c = classify([f"{ line }\n" for line in docstring])
            if c:
                res.append(c)
    return res


def classify(doc: list[str]) -> Union[dict, None]:
    "Process docstring and ignore or update details"
    line = doc[0]
    assert line.startswith(".. ")
    kind = line.split()[1]
    assert kind.endswith("::")
    kind = kind.rstrip(":")

    assert kind in ("method", "attribute", "class"), f"unknown kind { kind } in { line }"
    rest = line.split("::", 1)[1].strip()
    if "(" in rest:
        name, signature = rest.split("(", 1)
        signature = "(" + signature
    else:
        name, signature = rest, ""

    name = name.strip()
    signature = signature.strip()

    # strip leading and trailing blank lines
    doc = doc[1:]
    while doc and not doc[0].strip():
        doc = doc[1:]
    while not doc[-1].strip():
        doc = doc[:-1]

    if not doc:
        return None

    doc = [f"{ line }\n" for line in textwrap.dedent("".join(doc) + "\n").strip().split("\n")]

    n = 0
    while n < len(doc):
        if doc[n].strip().startswith("-* "):
            calls = doc[n].split()[1:]
            indent = " " * doc[n].find("-*")

            if len(calls) > 1:
                lines = [f"{ indent }Calls:\n"]
                for call in calls:
                    lines.append(f"{ indent }  * `{ call } <{ funclist[call] }>`__\n")
            else:
                lines = [f"{ indent }Calls: `{ calls[0] } <{ funclist[calls[0]] }>`__\n"]
            doc[n:n + len(lines)] = lines
        n += 1

    symbol = make_symbol(f"{ name }.class" if kind == "class" else name)
    return {
        "kind": kind,
        "name": name,
        "symbol": symbol,
        "signature_original": signature,
        "signature": analyze_signature(signature) if signature else [],
        "doc": doc,
        "skip_docstring": name in docstrings_skip or name.split(".")[0] in virtual_table_classes
    }


def make_symbol(n: str) -> str:
    "Returns C symbol name"
    n = n[0].upper() + n[1:]
    n = n.replace(".", "_").replace("__", "_").replace("__", "_")
    return n.rstrip("_")


def cppsafe(lines: List[str], eol: str) -> str:

    def backslash(l: str) -> str:
        return l.replace('"', '\\"').replace("\n", "\\n")

    res = "\n".join(f'''"{ backslash(line) }"{ eol }''' for line in lines)
    res = res.strip().strip("\\").strip()
    return res


def fixup(item: dict, eol: str) -> str:
    "Return docstring lines after making safe for C"
    lines = item["doc"]
    if item["signature"]:
        # cpython can't handle the arg or return type info
        sig = simple_signature(item["signature"])
        func = item["name"].split(".")[1]
        lines = [f'''{ func }{ sig }\n--\n\n{ item["name"] }{ item["signature_original"] }\n\n'''] + lines

    return cppsafe(lines, eol)


def simple_signature(signature: List[dict]) -> str:
    "Return signature simple enough to be accepted for __text_signature__"
    res = ["$self"]
    for param in signature:
        if param["name"] == "return":
            continue
        p = param["name"]
        if param["default"]:
            p += (f"={ param['default'] }")
        res.append(p)
    return "(" + ",".join(res) + ")"


def analyze_signature(s: str) -> List[dict]:
    "parse signature returning info about each item"
    res = []
    if "->" in s:
        s, rettype = [ss.strip() for ss in s.split("->", 1)]
    else:
        rettype = "None"
    res.append({"name": "return", "type": rettype})

    assert s[0] == "(" and s[-1] == ")"

    # we want to split on commas, but a param could be:  Union[Dict[A,B],X]
    nest_start = "[("
    nest_end = "])"

    pos = 1
    nesting = 0
    name = ""
    after_name = ""
    skip_to_next = False

    def add_param():
        nonlocal name, after_name
        p = {"name": name}
        after_name = [a.strip() for a in after_name.strip().lstrip(":").split("=", 1)]
        if len(after_name) > 1:
            after_name, default = after_name
        else:
            after_name, default = after_name[0], None
        p["type"] = after_name
        p["default"] = default
        res.append(p)
        name = after_name = ""

    for pos in range(1, len(s) - 1):
        c = s[pos]
        if c in nest_start or nesting:
            after_name += c
            if c in nest_start:
                nesting += 1
                continue
            if c not in nest_end:
                continue
            nesting -= 1
            continue

        if c == ',':
            assert name
            add_param()
            skip_to_next = False
            continue

        if skip_to_next:
            after_name += c
            continue

        if name and not (name + c).isidentifier() and not (name[0] == "*" and (name[1:] + c).isidentifier()):
            skip_to_next = True
            after_name = ""
            continue

        if c.isidentifier() or (name and c.isdigit()) or (not name and c in "/*"):
            name += c

    if name:
        add_param()

    return res


def check_and_update(symbol: str, code: str):
    for fn in glob.glob("src/*.c"):
        orig = open(fn, "r").read()
        if symbol not in orig:
            continue
        return check_and_update_file(fn, symbol, code)
    else:
        raise ValueError(f"Failed to find code with { symbol }")


def check_and_update_file(filename: str, symbol: str, code: str):
    lines = open(filename, "r").read().split("\n")
    insection = False
    for lineno, line in enumerate(lines):
        if line.strip() == "{":
            lineopen = lineno
        elif line.strip() == "}" and insection:
            lineclose = lineno
            break
        elif symbol in line:
            insection = True
    else:
        raise ValueError(f"{ symbol } not found in { filename }")

    lines = lines[:lineopen] + code.split("\n") + lines[lineclose + 1:]

    new = "\n".join(lines)
    if not new.endswith("\n"):
        new += "\n"
    replace_if_different(filename, new)


def replace_if_different(filename: str, contents: str) -> None:
    if not os.path.exists(filename) or open(filename).read() != contents:
        print(f"{ 'Creating' if not os.path.exists(filename) else 'Updating' } { filename }")
        open(filename, "w").write(contents)


# Python 'int' can be different C sizes (int32, int64 etc) so we override with more
# specific types here
type_overrides = {
    "apsw.softheaplimit": {
        "limit": "int64"
    },
    "apsw.hard_heap_limit": {
        "limit": "int64"
    },
    "Blob.readinto": {
        "buffer": "PyObject",
        "offset": "int64",
        "length": "int64"
    },
    "Blob.reopen": {
        "rowid": "int64"
    },
    "Connection.blobopen": {
        "rowid": "int64"
    },
    "Connection.drop_modules": {
        "keep": "PyObject"
    },
    "Connection.filecontrol": {
        "pointer": "pointer"
    },
    "Connection.set_last_insert_rowid": {
        "rowid": "int64"
    },
    "Cursor.execute": {
        "statements": "strtype"
    },
    "Cursor.executemany": {
        "statements": "strtype",
        "sequenceofbindings": "Sequence"
    },
    "URIFilename.uri_int": {
        "default": "int64",
    },
    "VFSFile.__init__": {
        "filename": "PyObject",
        "flags": "List[int,int]"
    },
    "VFSFile.xFileControl": {
        "ptr": "pointer"
    },
    "VFSFile.xRead": {
        "offset": "int64"
    },
    "VFSFile.xTruncate": {
        "newsize": "int64"
    },
    "VFSFile.xWrite": {
        "offset": "int64"
    },
    "VFS.xDlClose": {
        "handle": "pointer"
    },
    "VFS.xDlSym": {
        "handle": "pointer"
    },
    "VFS.xSetSystemCall": {
        "pointer": "pointer"
    },
    "VFS.xOpen": {
        "flags": "List[int,int]"
    },
    "zeroblob.__init__": {
        "size": "int64"
    },
}


def callable_erasure(f, token="Callable"):
    "Removes nested square brackets after token"
    if token not in f:
        return f
    res = ""
    rest = f
    while token in rest:
        idx = rest.index(token)
        res += rest[:idx + len(token)]
        rest = rest[idx + len(token):]

        c = rest[0]
        if c == ']':  # no type to erase
            continue
        assert c == '[', f"expected [ at '{ rest }' processing '{ f }'"
        nesting = 1
        rest = rest[1:]
        while nesting:
            c = rest[0]
            rest = rest[1:]
            if c == '[':
                nesting += 1
            elif c == ']':
                nesting -= 1
                if not nesting:
                    break

    res += rest
    return res


def do_argparse(item):
    for param in item["signature"]:
        try:
            param["type"] = type_overrides[item['name']][param["name"]]
        except KeyError:
            pass
        if param["name"] != "*" and not param["type"]:
            sys.exit(f"{ item['name'] } param { param } has no type from { item['signature_original'] }")
    res = [f"#define { item['symbol'] }_CHECK do {{ \\"]
    param_structs = []
    param_usages = []
    fstr = ""
    optional = False
    # names of python level keywords
    kwlist = []
    # what is passed at C level
    parse_args = []

    seen_star = False
    for param in item["signature"]:
        if param["name"] == "return":
            continue
        if param["name"] == "*":
            seen_star = True
            optional = True
            if "|" not in fstr:
                fstr += "|"
            fstr += "$"
            continue
        pname = param["name"]
        if pname in {"default"}:
            pname += "_"
        args = ["&" + pname]
        default_check = None
        if seen_star and not param["default"]:
            sys.exit(
                f'param { param } comes after * and must have default value in { item["name"] } { item["signature_original"] }'
            )
        if param["type"] == "str":
            type = "const char *"
            kind = "s"
            if param["default"]:
                if param["default"] == "None":
                    default_check = f"{ pname } == 0"
                else:
                    breakpoint()
                    pass
        elif param["type"] == "Optional[str]":
            type = "const char *"
            kind = "z"
            if param["default"]:
                if param["default"] == "None":
                    default_check = f"{ pname } == 0"
                else:
                    breakpoint()
                    pass
        elif param["type"] == "bool":
            type = "int"
            kind = "O&"
            args = ["argcheck_bool"] + args
            if param["default"]:
                assert param["default"] in {"True", "False"}
                dval = int(param["default"] == "True")
                default_check = f"{ pname } == { dval }"
        elif param["type"] == "int":
            type = "int"
            kind = "i"
            if param["default"]:
                try:
                    val = int(param['default'])
                except ValueError:
                    val = param['default'].replace("apsw.", "")
                default_check = f"{ pname } == ({ val })"
        elif param["type"] == "int64":
            type = "long long"
            kind = "L"
            if param["default"]:
                default_check = f"{ pname } == { int(param['default']) }L"
        elif param["type"] == "pointer":
            type = "void *"
            kind = "O&"
            args = ["argcheck_pointer"] + args
            if param["default"]:
                breakpoint()
                pass
        elif param["type"] in {
                "PyObject", "Any", "Optional[type[BaseException]]", "Optional[BaseException]",
                "Optional[types.TracebackType]", "Optional[VTModule]"
        }:
            type = "PyObject *"
            kind = "O"
            if param["default"]:
                breakpoint()
                pass
        elif param["type"] == "bytes":
            type = "Py_buffer"
            kind = "y*"
            if param["default"]:
                breakpoint()
                pass
        elif callable_erasure(param["type"]) in {
                "Optional[Callable]",
                "Optional[RowTracer]",
                "Optional[ExecTracer]",
                "Optional[ScalarProtocol]",
                "Optional[AggregateFactory]",
                "Optional[Authorizer]",
                "Optional[CommitHook]",
                "Optional[WindowFactory]",
        }:
            # the above are all callables and we don't check beyond that
            type = "PyObject *"
            kind = "O&"
            args = ["argcheck_Optional_Callable"] + args
            if param["default"]:
                if param["default"] == "None":
                    default_check = f"{ pname } == NULL"
                else:
                    breakpoint()
        elif param["type"] == "Optional[Union[str,URIFilename]]":
            type = "PyObject *"
            kind = "O&"
            args = ["argcheck_Optional_str_URIFilename"] + args
            if param["default"]:
                breakpoint()
                pass
        elif param["type"] == "Optional[Bindings]":
            type = "PyObject *"
            kind = "O&"
            args = ["argcheck_Optional_Bindings"] + args
            if param["default"]:
                if param["default"] == "None":
                    default_check = f"{ pname } == NULL"
                else:
                    breakpoint()
                pass
        elif callable_erasure(param["type"]) == "Callable":
            type = "PyObject *"
            kind = "O&"
            args = ["argcheck_Callable"] + args
            if param["default"]:
                breakpoint()
                pass
        elif param["type"] == "Sequence":
            # note that we can't check for sequence because anything
            # that PySequence_Fast accepts is ok which includes sets,
            # iterators, generators etc and I can't test for all of
            # them
            type = "PyObject *"
            kind = "O"
            if param["default"]:
                breakpoint()
                pass
        elif param["type"] == "Connection":
            type = "Connection *"
            kind = "O!"
            args = ["&ConnectionType"] + args
            if param["default"]:
                breakpoint()
                pass
        elif param["type"] == "strtype":
            type = "PyObject *"
            kind = "O!"
            args = ["&PyUnicode_Type"] + args
            if param["default"]:
                breakpoint()
                pass
        elif param["type"] == "List[int,int]":
            type = "PyObject *"
            kind = "O&"
            args = ["argcheck_List_int_int"] + args
            if param["default"]:
                breakpoint()
                pass
        else:
            assert False, f"Don't know how to handle type for { item ['name'] } param { param }"

        kwlist.append(pname)
        res.append(f"  assert(__builtin_types_compatible_p(typeof({ pname }), { type })); \\")
        if default_check:
            res.append(f"  assert({ default_check }); \\")

        if not optional and param["default"]:
            fstr += "|"
            optional = True

        fstr += kind
        assert len(args) in {1, 2}
        if len(args) == 2 and args[0].startswith("argcheck_"):
            param_usages.append(pname)
            param_structs.append(
                f"{ args[0] }_param { pname }_param = {{{ args[1] }, { item['symbol'] }_{ pname }_MSG}};")
            args[1] += "_param"

        parse_args.extend(args)

    res.append("} while(0)\n")

    param_structs = "\n    ".join(param_structs)

    code = f"""\
  {{
    static char *kwlist[] = {{{ ", ".join(f'"{ a }"' for a in kwlist) }, NULL}};
    { item['symbol'] }_CHECK;
    { param_structs }
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "{ fstr }:" { item['symbol'] }_USAGE, kwlist, { ", ".join(parse_args) }))
      return { "NULL" if not item['symbol'].endswith("_init") else -1 };
  }}"""

    code = "\n".join(line for line in code.split("\n") if line.strip())

    usage = f"{ item['name'] }{ item['signature_original'] }".replace('"', '\\"')
    res.insert(0, f"""#define { item['symbol'] }_USAGE "{ usage }"\n""")

    if param_usages:
        u = []
        for p in param_usages:
            u.append(f"""#define { item['symbol'] }_{ p }_MSG  "argument '{ p }' of { usage }" """)
        res.insert(0, "\n".join(u) + "\n")

    check_and_update(f"{ item['symbol'] }_CHECK", code)

    return "\n".join(res) + "\n"


def is_sequence(s):
    return isinstance(s, (list, tuple))


def get_class_doc(klass: str, items: List[dict]) -> str:
    for item in items:
        if item["name"] == klass:
            return item["doc"]
    raise ValueError(f"{ klass } doc not found")


def fmt_docstring(doc: list[str], indent: str) -> str:
    res = indent + '"""'
    for i, line in enumerate(doc):
        if i == 0:
            res += line
        elif line.strip():
            res += indent + line
        else:
            res += "\n"
    res = res.rstrip()
    res += '"""'
    return res


def attr_docstring(doc: list[str]) -> list[str]:
    ds = doc[:]
    if ds[0].startswith(":type:"):
        ds.pop(0)
    try:
        while not ds[0].strip():
            ds.pop(0)
    except:
        breakpoint()
    return ds


def generate_typestubs(items: list[dict]):
    try:
        import apsw
    except ImportError:
        print("Skipping type stub updates because can't import apsw")
        return

    out = io.StringIO()
    print("""# This file is generated by rst2docstring""", file=out)
    with open("src/apswtypes.py", "rt") as f:
        print(f.read(), file=out)

    lastclass = ""

    baseindent = ""

    for item in sorted(items, key=lambda x: x["symbol"]):
        if item["kind"] == "class":
            # these end up in an unhelpful place in the sort order
            continue

        klass, name = item['name'].split(".", 1)
        signature = item["signature_original"]
        if klass == "apsw":
            name = item["name"][len("apsw."):]
            if item["kind"] == "method":
                assert signature.startswith("(")
                print(f"{ baseindent }def { name }{ signature }:", file=out)
                print(fmt_docstring(item["doc"], indent=f"{ baseindent }    "), file=out)
                print(f"{ baseindent }    ...\n", file=out)
            else:
                assert item["kind"] == "attribute"
                print(f"{ baseindent }{ name }: { attribute_type(item) }", file=out)
                print(fmt_docstring(attr_docstring(item["doc"]), indent=baseindent), file=out)
                print("", file=out)
        else:
            if klass != lastclass:
                lastclass = klass
                doc = get_class_doc(klass, items)

                if klass in virtual_table_classes:
                    baseindent = "    "
                    print("\nif sys.version_info >= (3, 8):", file=out)
                    print(f"\n{ baseindent }class { klass }(Protocol):", file=out)
                else:
                    baseindent = ""
                    print(f"\n{ baseindent }class { klass }:", file=out)
                print(fmt_docstring(doc, indent=f"{ baseindent }    "), file=out)
                print("", file=out)

            if item["kind"] == "method":
                for find, replace in (
                    ("apsw.", ""),  # some constants
                    ("List[int,int]", "List[int]"),  # can't see how to type a 2 item list
                ):
                    signature = signature.replace(find, replace)
                if not signature.startswith("(self"):
                    signature = "(self" + (", " if signature[1] != ")" else "") + signature[1:]
                print(f"{ baseindent }    def { name }{ signature }:", file=out)
                print(fmt_docstring(item["doc"], indent=f"{ baseindent }        "), file=out)
                print(f"{ baseindent }        ...\n", file=out)

            else:
                assert item["kind"] == "attribute"
                print(f"{ baseindent }    { name }: { attribute_type(item) }", file=out)
                print(fmt_docstring(attr_docstring(item["doc"]), indent=f"{ baseindent }    "), file=out)
                print("", file=out)

    # constants
    print("\n", file=out)
    for n in dir(apsw):
        if not n.startswith("SQLITE_") or n == "SQLITE_VERSION_NUMBER":
            continue
        assert isinstance(getattr(apsw, n), int)
        ci = get_sqlite_constant_info(n)
        print(f"""{ n }: int = { ci["value"] }""", file=out)
        print(f'''"""For `{ ci["title"] } <{ ci["url"] }>'__"""''', file=out)

    # mappings
    def wrapvals(vals):
        return "\n".join(textwrap.wrap(" ".join(sorted(vals))))

    print("\n", file=out)
    for n in dir(apsw):
        if not n.startswith("mapping_"):
            continue
        mi = get_mapping_info(n)
        print(f"{ n }: Dict[Union[str,int],Union[int,str]]", file=out)
        print(f'''"""{ mi["title"] } mapping names to int and int to names.
Doc at { mi["url"] }

{ wrapvals(mi["members"]) }"""''',
              file=out)
        print("", file=out)

    # exceptions
    print("\n", file=out)
    print("class Error(Exception):", file=out)
    print(fmt_docstring(get_exc_doc("Error"), indent="    "), file=out)
    print("", file=out)
    for n in dir(apsw):
        if n != "ExecTraceAbort" and (not n.endswith("Error") or n == "Error"):
            continue
        print(f"class { n }(Error):", file=out)
        print(fmt_docstring(get_exc_doc(n), indent="    "), file=out)
        print("", file=out)

    replace_if_different("apsw/__init__.pyi", out.getvalue())


def attribute_type(item: dict) -> str:
    # docstring will start with :type: type
    doc = "\n".join(item["doc"]).strip().split("\n")[0]
    assert doc.startswith(":type:"), f"Expected :type: for doc in { item }"
    return doc[len(":type:"):].strip()


if __name__ == '__main__':
    import json
    docdb = json.load(open(sys.argv[1]))

    sqlite_links()

    items = process_docdb(docdb)
    get_all_exc_doc()

    allcode = "\n".join(open(fn).read() for fn in glob.glob("src/*.c"))

    missing = []

    out = io.StringIO()
    print("""/* This file is generated by rst2docstring */

#ifndef __GNUC__
#define __builtin_types_compatible_p(x,y) (1)
#endif
""",
          file=out)
    method, mid, eol, end = "#define ", " ", " \\", ""
    for item in sorted(items, key=lambda x: x["symbol"]):
        if item["skip_docstring"]:
            continue
        print(f"""{ method } { item["symbol"] }_DOC{ mid }{ fixup( item, eol) } { end }\n""", file=out)
        if f"{ item['symbol'] }_CHECK" in allcode:
            print(do_argparse(item), file=out)
        else:
            if any(param["name"] != "return"
                   for param in item["signature"]) and not any(param["name"].startswith("*")
                                                               for param in item["signature"]):
                if item["name"] not in {
                        "apsw.format_sql_value",
                        "VFSFile.excepthook",
                        "Cursor.__next__",
                        "Cursor.__iter__",
                        "VFS.excepthook",
                        "Connection.execute",
                        "Connection.executemany",
                        "Blob.__exit__",
                }:
                    missing.append(item["name"])
    for name, doc in sorted(all_exc_doc.items()):
        print(f"""{ method } { name }_exc_DOC{ mid }{ cppsafe(doc, eol) } { end }\n""", file=out)

    outval = out.getvalue()
    replace_if_different(sys.argv[2], outval)

    symbols = sorted([item["symbol"] for item in items if not item["skip_docstring"]])

    if any(s not in allcode for s in symbols):
        print("Unreferenced doc\n")
        for s in symbols:
            if s not in allcode:
                print("  ", s)
        sys.exit(2)

    if missing:
        print("Not argparse checked", missing)

    generate_typestubs(items)