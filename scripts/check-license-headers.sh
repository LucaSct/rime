#!/usr/bin/env bash
# Fail if any C++/Rust source file is missing the SPDX license header required by
# CLAUDE.md. Used by CI's license gate and runnable locally. We check the first few lines
# of every source file under engine/, tests/, and tools/.
set -euo pipefail
cd "$(cd "$(dirname "$0")/.." && pwd)"

needle="SPDX-License-Identifier: Apache-2.0"
missing=0
while IFS= read -r f; do
    if ! head -n 3 "$f" | grep -q "$needle"; then
        echo "missing SPDX header: $f"
        missing=1
    fi
done < <(find engine tests tools -type d -name target -prune -o \
    -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.mm' -o -name '*.rs' \) -print)

if [ "$missing" -ne 0 ]; then
    echo ""
    echo "License-header check failed. Add to the top of each flagged file:"
    echo "  // SPDX-License-Identifier: Apache-2.0"
    echo "  // Copyright (c) 2026 The Rime Engine Authors."
    exit 1
fi
echo "License headers OK."
