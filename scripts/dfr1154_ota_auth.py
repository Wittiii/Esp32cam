Import("env")

import re
from pathlib import Path


def decode_cpp_string(value):
    return value.replace(r"\"", '"').replace(r"\\", "\\")


if env.GetProjectOption("upload_protocol", "") == "espota":
    secrets_path = Path(env.subst("$PROJECT_DIR")) / "include" / "dfr1154_secrets.h"
    contents = secrets_path.read_text(encoding="utf-8")
    match = re.search(r'kOtaPassword\[\]\s*=\s*"((?:\\.|[^"])*)";', contents)
    if match is None:
        raise RuntimeError("kOtaPassword is missing from include/dfr1154_secrets.h")
    env.Append(UPLOADERFLAGS=["--auth=" + decode_cpp_string(match.group(1))])
    print("[DFR1154] OTA authentication configured from local secrets")
