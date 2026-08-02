#!/usr/bin/env python3
"""Extract first-party C/C++ structure from libclang's real AST."""

from __future__ import annotations

import ctypes
import ctypes.util
import json
import shlex
from dataclasses import dataclass
from pathlib import Path
from typing import Any


class CXString(ctypes.Structure):
    _fields_ = [("data", ctypes.c_void_p), ("private_flags", ctypes.c_uint)]


class CXCursor(ctypes.Structure):
    _fields_ = [
        ("kind", ctypes.c_uint),
        ("xdata", ctypes.c_int),
        ("data", ctypes.c_void_p * 3),
    ]


class CXType(ctypes.Structure):
    _fields_ = [("kind", ctypes.c_int), ("data", ctypes.c_void_p * 2)]


class CXSourceLocation(ctypes.Structure):
    _fields_ = [("ptr_data", ctypes.c_void_p * 2), ("int_data", ctypes.c_uint)]


class CXSourceRange(ctypes.Structure):
    _fields_ = [
        ("ptr_data", ctypes.c_void_p * 2),
        ("begin_int_data", ctypes.c_uint),
        ("end_int_data", ctypes.c_uint),
    ]


@dataclass(frozen=True)
class CompileCommand:
    file: str
    directory: str
    arguments: tuple[str, ...]


DECLARATION_KINDS = {
    "Namespace": "namespace",
    "StructDecl": "struct",
    "ClassDecl": "class",
    "UnionDecl": "union",
    "EnumDecl": "enum",
    "EnumConstantDecl": "enumerator",
    "FunctionDecl": "function",
    "FunctionTemplate": "function",
    "CXXMethod": "method",
    "Constructor": "method",
    "Destructor": "method",
    "CXXConversion": "method",
    "ConversionFunction": "method",
    "ClassTemplate": "class",
    "ClassTemplatePartialSpecialization": "class",
    "FieldDecl": "member",
    "VarDecl": "variable",
    "TypedefDecl": "typedef",
    "TypeAliasDecl": "typedef",
}
CALLABLE_KINDS = {
    "FunctionDecl",
    "FunctionTemplate",
    "CXXMethod",
    "Constructor",
    "Destructor",
    "CXXConversion",
    "ConversionFunction",
}
CALL_KINDS = {"CallExpr", "CUDAKernelCallExpr"}
CONTAINER_KINDS = {
    "Namespace",
    "StructDecl",
    "ClassDecl",
    "UnionDecl",
    "EnumDecl",
    "ClassTemplate",
    "ClassTemplatePartialSpecialization",
}


def find_libclang() -> str:
    for name in ("clang-18", "clang-17", "clang-16", "clang-15", "clang"):
        found = ctypes.util.find_library(name)
        if found:
            return found
    for candidate in (
        "/usr/lib/llvm-18/lib/libclang.so",
        "/usr/lib/llvm-17/lib/libclang.so",
        "/usr/lib/llvm-16/lib/libclang.so",
        "/usr/lib/llvm-15/lib/libclang.so",
    ):
        if Path(candidate).is_file():
            return candidate
    raise RuntimeError("libclang was not found")


def configure_libclang() -> ctypes.CDLL:
    lib = ctypes.CDLL(find_libclang())
    lib.clang_getCString.argtypes = [CXString]
    lib.clang_getCString.restype = ctypes.c_char_p
    lib.clang_disposeString.argtypes = [CXString]
    lib.clang_createIndex.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.clang_createIndex.restype = ctypes.c_void_p
    lib.clang_disposeIndex.argtypes = [ctypes.c_void_p]
    lib.clang_parseTranslationUnit2.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_char_p),
        ctypes.c_int,
        ctypes.c_void_p,
        ctypes.c_uint,
        ctypes.c_uint,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.clang_parseTranslationUnit2.restype = ctypes.c_int
    lib.clang_disposeTranslationUnit.argtypes = [ctypes.c_void_p]
    lib.clang_getTranslationUnitCursor.argtypes = [ctypes.c_void_p]
    lib.clang_getTranslationUnitCursor.restype = CXCursor
    lib.clang_getCursorKindSpelling.argtypes = [ctypes.c_uint]
    lib.clang_getCursorKindSpelling.restype = CXString
    lib.clang_getCursorSpelling.argtypes = [CXCursor]
    lib.clang_getCursorSpelling.restype = CXString
    lib.clang_getCursorDisplayName.argtypes = [CXCursor]
    lib.clang_getCursorDisplayName.restype = CXString
    lib.clang_getCursorUSR.argtypes = [CXCursor]
    lib.clang_getCursorUSR.restype = CXString
    lib.clang_getCursorSemanticParent.argtypes = [CXCursor]
    lib.clang_getCursorSemanticParent.restype = CXCursor
    lib.clang_getCursorReferenced.argtypes = [CXCursor]
    lib.clang_getCursorReferenced.restype = CXCursor
    lib.clang_Cursor_isNull.argtypes = [CXCursor]
    lib.clang_Cursor_isNull.restype = ctypes.c_int
    lib.clang_hashCursor.argtypes = [CXCursor]
    lib.clang_hashCursor.restype = ctypes.c_uint
    lib.clang_getCursorLocation.argtypes = [CXCursor]
    lib.clang_getCursorLocation.restype = CXSourceLocation
    lib.clang_getExpansionLocation.argtypes = [
        CXSourceLocation,
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_uint),
        ctypes.POINTER(ctypes.c_uint),
        ctypes.POINTER(ctypes.c_uint),
    ]
    lib.clang_getFileName.argtypes = [ctypes.c_void_p]
    lib.clang_getFileName.restype = CXString
    lib.clang_getCursorExtent.argtypes = [CXCursor]
    lib.clang_getCursorExtent.restype = CXSourceRange
    lib.clang_getRangeStart.argtypes = [CXSourceRange]
    lib.clang_getRangeStart.restype = CXSourceLocation
    lib.clang_getRangeEnd.argtypes = [CXSourceRange]
    lib.clang_getRangeEnd.restype = CXSourceLocation
    lib.clang_getCursorType.argtypes = [CXCursor]
    lib.clang_getCursorType.restype = CXType
    lib.clang_getTypeSpelling.argtypes = [CXType]
    lib.clang_getTypeSpelling.restype = CXString
    lib.clang_isCursorDefinition.argtypes = [CXCursor]
    lib.clang_isCursorDefinition.restype = ctypes.c_uint
    lib.clang_getCXXAccessSpecifier.argtypes = [CXCursor]
    lib.clang_getCXXAccessSpecifier.restype = ctypes.c_uint
    lib.clang_getNumDiagnostics.argtypes = [ctypes.c_void_p]
    lib.clang_getNumDiagnostics.restype = ctypes.c_uint
    lib.clang_getDiagnostic.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    lib.clang_getDiagnostic.restype = ctypes.c_void_p
    lib.clang_getDiagnosticSeverity.argtypes = [ctypes.c_void_p]
    lib.clang_getDiagnosticSeverity.restype = ctypes.c_uint
    lib.clang_formatDiagnostic.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    lib.clang_formatDiagnostic.restype = CXString
    lib.clang_disposeDiagnostic.argtypes = [ctypes.c_void_p]
    return lib


def cx_text(lib: ctypes.CDLL, value: CXString) -> str:
    raw = lib.clang_getCString(value)
    result = raw.decode("utf-8", "replace") if raw else ""
    lib.clang_disposeString(value)
    return result


def cursor_kind(lib: ctypes.CDLL, cursor: CXCursor) -> str:
    return cx_text(lib, lib.clang_getCursorKindSpelling(cursor.kind))


def cursor_location(lib: ctypes.CDLL, location: CXSourceLocation) -> tuple[str, int, int] | None:
    source_file = ctypes.c_void_p()
    line = ctypes.c_uint()
    column = ctypes.c_uint()
    offset = ctypes.c_uint()
    lib.clang_getExpansionLocation(
        location,
        ctypes.byref(source_file),
        ctypes.byref(line),
        ctypes.byref(column),
        ctypes.byref(offset),
    )
    if not source_file.value:
        return None
    return cx_text(lib, lib.clang_getFileName(source_file)), line.value, column.value


def relative_location(
    lib: ctypes.CDLL,
    root: Path,
    location: CXSourceLocation,
) -> tuple[str, int, int] | None:
    resolved = cursor_location(lib, location)
    if resolved is None:
        return None
    filename, line, column = resolved
    try:
        relative = Path(filename).resolve().relative_to(root).as_posix()
    except ValueError:
        return None
    return relative, line, column


def semantic_scope(lib: ctypes.CDLL, cursor: CXCursor) -> str:
    names = []
    parent = lib.clang_getCursorSemanticParent(cursor)
    while not lib.clang_Cursor_isNull(parent):
        kind = cursor_kind(lib, parent)
        if kind == "TranslationUnit":
            break
        if kind in CONTAINER_KINDS:
            name = cx_text(lib, lib.clang_getCursorSpelling(parent))
            if name:
                names.append(name)
        parent = lib.clang_getCursorSemanticParent(parent)
    return "::".join(reversed(names))


def qualified_cursor_name(lib: ctypes.CDLL, cursor: CXCursor) -> str:
    if lib.clang_Cursor_isNull(cursor):
        return ""
    name = cx_text(lib, lib.clang_getCursorSpelling(cursor))
    scope = semantic_scope(lib, cursor)
    return f"{scope}::{name}" if scope and name else name


def compile_arguments(entry: dict[str, Any]) -> tuple[str, ...]:
    raw = entry.get("arguments")
    arguments = list(raw) if isinstance(raw, list) else shlex.split(entry["command"])
    if arguments:
        arguments.pop(0)
    source = Path(entry["file"]).resolve()
    directory = Path(entry["directory"]).resolve()
    result = [f"-working-directory={directory}"]
    skip_next = False
    options_with_value = {"-o", "-MF", "-MT", "-MQ"}
    ignored = {"-c", "-MD", "-MMD", "-MP"}
    for argument in arguments:
        if skip_next:
            skip_next = False
            continue
        if argument in options_with_value:
            skip_next = True
            continue
        if argument in ignored:
            continue
        candidate = Path(argument)
        if candidate.is_absolute() and candidate.resolve() == source:
            continue
        if not candidate.is_absolute() and (directory / candidate).resolve() == source:
            continue
        result.append(argument)
    return tuple(result)


def load_compile_commands(path: Path, root: Path, indexed_paths: set[str]) -> list[CompileCommand]:
    if not path.is_file():
        return []
    database = json.loads(path.read_text(encoding="utf-8"))
    selected: dict[str, CompileCommand] = {}
    for entry in database:
        source = Path(entry["file"])
        if not source.is_absolute():
            source = Path(entry["directory"]) / source
        try:
            relative = source.resolve().relative_to(root).as_posix()
        except ValueError:
            continue
        if relative not in indexed_paths or relative in selected:
            continue
        selected[relative] = CompileCommand(
            file=str(source.resolve()),
            directory=str(Path(entry["directory"]).resolve()),
            arguments=compile_arguments(entry),
        )
    return [selected[path] for path in sorted(selected)]


def parse_translation_unit(command: CompileCommand, root_string: str) -> dict[str, Any]:
    root = Path(root_string).resolve()
    try:
        lib = configure_libclang()
    except (OSError, RuntimeError) as error:
        return {"source": command.file, "error": str(error)}
    index = lib.clang_createIndex(0, 0)
    translation_unit = ctypes.c_void_p()
    encoded_arguments = [argument.encode("utf-8") for argument in command.arguments]
    argument_array = (ctypes.c_char_p * len(encoded_arguments))(*encoded_arguments)
    parse_error = lib.clang_parseTranslationUnit2(
        index,
        command.file.encode("utf-8"),
        argument_array,
        len(encoded_arguments),
        None,
        0,
        0x200,
        ctypes.byref(translation_unit),
    )
    if parse_error or not translation_unit.value:
        lib.clang_disposeIndex(index)
        return {"source": command.file, "error": f"libclang parse error {parse_error}"}

    diagnostics = []
    has_errors = False
    for diagnostic_index in range(lib.clang_getNumDiagnostics(translation_unit)):
        diagnostic = lib.clang_getDiagnostic(translation_unit, diagnostic_index)
        severity = lib.clang_getDiagnosticSeverity(diagnostic)
        if severity >= 3:
            has_errors = True
        if severity >= 2 and len(diagnostics) < 10:
            diagnostics.append(cx_text(lib, lib.clang_formatDiagnostic(diagnostic, 0)))
        lib.clang_disposeDiagnostic(diagnostic)
    if has_errors:
        lib.clang_disposeTranslationUnit(translation_unit)
        lib.clang_disposeIndex(index)
        return {
            "source": command.file,
            "error": "; ".join(diagnostics) or "Clang reported compilation errors",
        }

    symbols: dict[str, list[dict[str, Any]]] = {}
    calls: dict[str, list[dict[str, Any]]] = {}
    covered_paths: set[str] = set()
    caller_by_parent: dict[int, dict[str, str] | None] = {}
    access_names = {2: "public", 3: "protected", 4: "private"}

    visitor_type = ctypes.CFUNCTYPE(ctypes.c_uint, CXCursor, CXCursor, ctypes.c_void_p)

    def visit(cursor: CXCursor, parent: CXCursor, _data: ctypes.c_void_p) -> int:
        kind = cursor_kind(lib, cursor)
        location = relative_location(lib, root, lib.clang_getCursorLocation(cursor))
        parent_caller = caller_by_parent.get(lib.clang_hashCursor(parent))
        caller = parent_caller
        if location is None:
            return 2 if parent_caller is not None else 1
        path, name_line, name_column = location
        covered_paths.add(path)
        extent = lib.clang_getCursorExtent(cursor)
        start = relative_location(lib, root, lib.clang_getRangeStart(extent)) or location
        end = relative_location(lib, root, lib.clang_getRangeEnd(extent)) or location
        name = cx_text(lib, lib.clang_getCursorSpelling(cursor))
        display_name = cx_text(lib, lib.clang_getCursorDisplayName(cursor))
        usr = cx_text(lib, lib.clang_getCursorUSR(cursor))
        scope = semantic_scope(lib, cursor)
        qualified_name = f"{scope}::{name}" if scope else name

        if kind in DECLARATION_KINDS and name:
            is_definition = bool(lib.clang_isCursorDefinition(cursor))
            output_kind = DECLARATION_KINDS[kind]
            if kind in CALLABLE_KINDS and not is_definition:
                output_kind = "prototype"
            parent_kind = cursor_kind(lib, lib.clang_getCursorSemanticParent(cursor))
            include_declaration = not (kind == "VarDecl" and parent_kind in CALLABLE_KINDS)
            signature = display_name[len(name):] if display_name.startswith(name) else display_name
            item = {
                "name": name,
                "qualified_name": qualified_name,
                "kind": output_kind,
                "line": start[1],
                "column": start[2],
                "name_line": name_line,
                "name_column": name_column,
                "end_line": end[1],
                "end_column": end[2],
                "span_lines": max(1, end[1] - start[1] + 1),
                "scope": scope,
                "signature": signature,
                "type": cx_text(lib, lib.clang_getTypeSpelling(lib.clang_getCursorType(cursor))),
                "usr": usr,
                "is_definition": is_definition,
                "access": access_names.get(lib.clang_getCXXAccessSpecifier(cursor), ""),
                "parser": "clang-ast",
            }
            if include_declaration:
                symbols.setdefault(path, []).append(item)
            if kind in CALLABLE_KINDS and is_definition:
                caller = {"name": qualified_name, "usr": usr}

        if kind == "LambdaExpr":
            caller = {
                "name": f"{parent_caller['name']}::<lambda@{name_line}:{name_column}>"
                if parent_caller else f"<lambda@{path}:{name_line}:{name_column}>",
                "usr": "",
            }

        if kind in CALL_KINDS:
            referenced = lib.clang_getCursorReferenced(cursor)
            callee = qualified_cursor_name(lib, referenced)
            if not callee:
                callee = display_name or name or "<unresolved>"
            calls.setdefault(path, []).append({
                "caller": parent_caller["name"] if parent_caller else "<file-scope>",
                "caller_usr": parent_caller["usr"] if parent_caller else "",
                "callee": callee,
                "callee_usr": cx_text(lib, lib.clang_getCursorUSR(referenced))
                if not lib.clang_Cursor_isNull(referenced) else "",
                "line": name_line,
                "column": name_column,
                "type": cx_text(lib, lib.clang_getTypeSpelling(lib.clang_getCursorType(cursor))),
                "parser": "clang-ast",
            })

        caller_by_parent[lib.clang_hashCursor(cursor)] = caller
        return 2

    visitor = visitor_type(visit)
    lib.clang_visitChildren.argtypes = [CXCursor, visitor_type, ctypes.c_void_p]
    lib.clang_visitChildren.restype = ctypes.c_uint
    root_cursor = lib.clang_getTranslationUnitCursor(translation_unit)
    caller_by_parent[lib.clang_hashCursor(root_cursor)] = None
    lib.clang_visitChildren(root_cursor, visitor, None)
    lib.clang_disposeTranslationUnit(translation_unit)
    lib.clang_disposeIndex(index)
    return {
        "source": command.file,
        "symbols": symbols,
        "calls": calls,
        "covered_paths": sorted(covered_paths),
        "diagnostics": diagnostics,
    }
