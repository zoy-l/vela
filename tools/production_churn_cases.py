"""Bounded retained object graphs with repeated replacement and complete validation."""

DECLARATIONS = """
class ChurnRecord { generation: int; text: string; values: int[]; }
let churnRing: ChurnRecord[] = [];
let churnSequence: int = 0;
"""


def churn_cases() -> list[dict]:
    records = []
    for width in (64, 1024):
        body = f"""
const generation = churnSequence % 257;
const values: int[] = [];
for (let i = 0; i < {width}; i++) {{ values.push(generation + i); }}
const record = ChurnRecord(generation: generation,
  text: 'production-record-' + generation.toString(), values: values);
const slot = churnSequence % 64;
if (churnRing.length < 64) {{ churnRing.push(record); }} else {{ churnRing[slot] = record; }}
churnSequence++;
for (let i = 0; i < churnRing.length; i++) {{
  const item = churnRing[i];
  const last = churnSequence - 1;
  const expected = last - ((last - i) % 64);
  if (item.generation != expected % 257) {{ return 0; }}
  if (item.text != 'production-record-' + item.generation.toString() || item.values.length != {width}) {{ return 0; }}
  for (let j = 0; j < item.values.length; j++) {{
    if (item.values[j] != item.generation + j) {{ return 0; }}
  }}
}}
return 42;
"""
        records.append(dict(name=f'longrun/ring/{width}', kind='Churn', size=width, expected=42,
                            setup='churnRing = []; churnSequence = 0;', body=body))
    return records
