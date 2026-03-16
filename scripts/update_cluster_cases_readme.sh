#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
README="$ROOT_DIR/design/hybridacc-ESL/README.md"
CLUSTER_DIR="$ROOT_DIR/testbench/cluster"
BEGIN_MARKER="<!-- BEGIN_CLUSTER_CASES_AUTO -->"
END_MARKER="<!-- END_CLUSTER_CASES_AUTO -->"

if [ ! -f "$README" ]; then
    echo "[ERROR] README not found: $README" >&2
    exit 1
fi

if [ ! -d "$CLUSTER_DIR" ]; then
    echo "[ERROR] cluster testbench dir not found: $CLUSTER_DIR" >&2
    exit 1
fi

if ! grep -q "$BEGIN_MARKER" "$README" || ! grep -q "$END_MARKER" "$README"; then
    echo "[ERROR] Auto block markers not found in README." >&2
    exit 1
fi

mapfile -t CASES < <(find "$CLUSTER_DIR" -mindepth 2 -maxdepth 2 -type f -name config.json \
    | sed -E 's#^.*/testbench/cluster/([^/]+)/config\.json$#\1#' \
    | sort)

if [ ${#CASES[@]} -eq 0 ]; then
    echo "[ERROR] No cluster cases found (expected */config.json under $CLUSTER_DIR)." >&2
    exit 1
fi

TMP_FILE=$(mktemp)
{
    for case_name in "${CASES[@]}"; do
        echo "- \`$case_name\`"
    done
} > "$TMP_FILE"

NEW_README=$(mktemp)
awk -v begin="$BEGIN_MARKER" -v end="$END_MARKER" -v listFile="$TMP_FILE" '
    $0 == begin {
        print $0
        while ((getline line < listFile) > 0) {
            print line
        }
        in_block = 1
        next
    }
    $0 == end {
        in_block = 0
        print $0
        next
    }
    in_block != 1 {
        print $0
    }
' "$README" > "$NEW_README"

mv "$NEW_README" "$README"
rm -f "$TMP_FILE"

echo "[OK] Updated cluster case list in $README"
