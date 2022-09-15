import json
import os
from pathlib import Path


SCRIPT_DIR = Path(os.path.dirname(os.path.realpath(__file__)))
REPO_DIR = SCRIPT_DIR.parent.parent

with (REPO_DIR / ".config" / "tsaoptions.json").open() as f:
    data = json.load(f)

buildNumber = os.getenv("BUILD_BUILDNUMBER")
if buildNumber is not None:
    data["buildNumber"] = buildNumber

with (REPO_DIR / ".config" / "tsaoptions.json").open(model="w") as f:
    json.dump(data, f)
