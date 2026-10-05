"""Saved responses must not silently become a successful empty image."""
import pathlib
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="hvc-converter-") as directory:
    root = pathlib.Path(directory)
    subprocess.run([sys.argv[2], "--write-responses", directory], check=True)
    def convert(names, success):
        output = root / "out.j2k"
        output.write_bytes(b"preserve on failure")
        result = subprocess.run([sys.argv[1], "-o", str(output),
                                 *[str(root / name) for name in names]], capture_output=True)
        assert result.returncode == (0 if success else 1), result.stderr
        if success:
            assert output.read_bytes().startswith(b"\xffO")
            return output.read_bytes()
        assert result.stderr and output.read_bytes() == b"preserve on failure"
    convert(["headers-header.jpp"], False)
    convert(["limited-header.jpp", "limited.jpp"], False)
    convert(["layers-header.jpp", "layers.jpp"], False)
    whole = convert(["whole-header.jpp", "whole.jpp"], True)
    assert convert(["limited-header.jpp", "limited.jpp", "continuation.jpp"], True) == whole
    assert convert(["whole-header.jpp", "whole.jpp", "whole.jpp"], True) == whole
    convert(["reduced-header.jpp", "reduced.jpp"], True)
