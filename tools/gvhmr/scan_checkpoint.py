"""Checks a PyTorch checkpoint for code it would run when loaded, without loading it.

    python scan_checkpoint.py file.ckpt [more files...]

A checkpoint is a zip holding pickle data. A pickle can call any Python function while it is loaded, which is how a
tampered model file takes over a machine. Every function it can call is named in the data, so this lists those names
(without running anything) and refuses the file if any is outside a short list of harmless ones: PyTorch's tensor
rebuilders, NumPy arrays, ordered dicts and config containers. Exit code 0 means every file passed.
"""

import pickletools
import sys
import zipfile

SAFE_PREFIXES = (
    "torch._utils._rebuild", "torch.FloatStorage", "torch.HalfStorage", "torch.LongStorage", "torch.IntStorage",
    "torch.BoolStorage", "torch.ByteStorage", "torch.DoubleStorage", "torch.BFloat16Storage", "torch.storage._load_from_bytes",
    "torch.float", "torch.half", "torch.int", "torch.long", "torch.bool", "torch.bfloat16", "torch.Size",
    "torch.device", "torch._tensor._rebuild_from_type_v2", "torch.nn.parameter.Parameter",
    "collections.OrderedDict", "collections.defaultdict", "builtins.set", "builtins.frozenset", "builtins.slice",
    "builtins.dict", "builtins.list", "builtins.tuple", "builtins.int", "builtins.float", "builtins.str", "builtins.bool",
    "numpy.core.multiarray._reconstruct", "numpy.core.multiarray.scalar", "numpy.ndarray", "numpy.dtype",
    "numpy._core.multiarray._reconstruct", "numpy._core.multiarray.scalar",
    "omegaconf.", "typing.Any", "pathlib.PosixPath", "_codecs.encode",
)


def globals_in(data):
    """Every module.name the pickle refers to, found by reading its opcodes, never by running them."""
    found, strings = set(), []
    for op, arg, _ in pickletools.genops(data):
        if op.name in ("SHORT_BINUNICODE", "BINUNICODE", "UNICODE", "BINUNICODE8"):
            strings.append(arg)
        elif op.name == "GLOBAL":
            found.add(arg.replace(" ", "."))
        elif op.name == "STACK_GLOBAL" and len(strings) >= 2:
            found.add(f"{strings[-2]}.{strings[-1]}")
    return found


def scan(path):
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            pickles = [z.read(n) for n in z.namelist() if n.endswith(".pkl")]
    else:
        with open(path, "rb") as f:
            pickles = [f.read()]
    names = set().union(*(globals_in(p) for p in pickles)) if pickles else set()
    unsafe = sorted(n for n in names if not n.startswith(SAFE_PREFIXES))
    print(f"{'UNSAFE' if unsafe else 'safe  '}  {path}  ({len(names)} kinds of object)")
    for name in unsafe:
        print(f"        would call: {name}")
    return not unsafe


if __name__ == "__main__":
    results = [scan(p) for p in sys.argv[1:]]
    sys.exit(0 if results and all(results) else 1)
