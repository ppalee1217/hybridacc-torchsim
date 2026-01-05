hybridacc-trace-parser
======================

Install locally (editable):

```bash
pip install -e python/trace_parser
```

Or install from workspace root:

```bash
cd python
pip install -e ./trace_parser
```

Usage examples:

```bash
# CLI
python -m trace_parser.cli /path/to/trace.json --stats all
# or installed entry-point
trace-parser /path/to/trace.json --stats all

# Python API
from trace_parser import TraceParser
tp = TraceParser.load("output/trace-conv_k3c4h1.json")
print(tp.summary())
```
