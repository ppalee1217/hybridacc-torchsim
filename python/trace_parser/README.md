hybridacc-trace-parser
======================

文件樹： [../../doc/index.md](../../doc/index.md) -> [../../doc/user-manual/python-cli-reference.md](../../doc/user-manual/python-cli-reference.md) -> 本頁。

Repo 內的 Python workflow 以 `uv` 為主。一般情況下不需要單獨安裝這個 package，只要在 repo root `uv sync` 後直接用 `uv run` 執行即可。

從 repo root 使用：

```bash
cd "$(git rev-parse --show-toplevel)"
uv sync
uv run python -m trace_parser.cli /path/to/trace.json --stats all
```

若你真的需要 editable install，也請透過 `uv`：

```bash
cd "$(git rev-parse --show-toplevel)"
uv run pip install -e python/trace_parser
```

Usage examples:

```bash
# CLI
uv run python -m trace_parser.cli /path/to/trace.json --stats all

# Python API
from trace_parser import TraceParser
tp = TraceParser.load("output/trace-conv_k3c4h1.json")
print(tp.summary())
```
