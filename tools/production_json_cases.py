"""Typed JSON fixtures shared by generated AOT and independent C++ models."""

import json


DECLARATIONS = """
let jsonCalls: int = 0;
class JsonItem { value: int; text: string; }
class ConstructItem {
  value: int; text: string;
  constructor { jsonCalls++; this.value += this.text.length; }
}
class JsonBox<T> { value: T; }
"""


def json_cases() -> list[dict]:
    records = []
    for name in ('scalar', 'object', 'array', 'nested', 'constructor', 'early_failure',
                 'late_failure', 'utf8_failure', 'missing_field', 'unknown_field', 'text_type'):
        constructed = name in ('constructor', 'early_failure', 'late_failure')
        item_type = 'ConstructItem' if constructed else 'JsonItem'
        kind = {'scalar': 'JsonScalar', 'object': 'JsonObject', 'nested': 'JsonNested'}.get(
            name, 'JsonConstruct' if constructed else 'JsonArray')
        count = 1 if name in ('scalar', 'object', 'missing_field', 'unknown_field', 'text_type') else 1024
        items = [dict(value=42, text='Vela') for _ in range(count)]
        error = dict(code='', path='', expected='', actual='', offset=-1)
        if name in ('early_failure', 'late_failure', 'utf8_failure', 'text_type'):
            index = 0 if name in ('early_failure', 'text_type') else count - 1
            field = 'text' if name == 'text_type' else 'value'
            items[index][field] = False
            if name == 'utf8_failure':
                items[0]['text'] = '播放器🎮'
            error.update(code='type_mismatch', path=f'$[{index}].{field}',
                         expected='string' if field == 'text' else 'int', actual='bool')
        elif name == 'missing_field':
            del items[0]['text']
            error.update(code='missing_field', path='$[0].text', expected='string', actual='missing')
        elif name == 'unknown_field':
            items[0]['extra'] = True
            error.update(code='unknown_field', path='$[0].extra', actual='bool')
        value = 42 if name == 'scalar' else items[0] if name == 'object' else items
        if name == 'nested':
            value = dict(value=dict(value=value))
        source = json.dumps(value, ensure_ascii=False, separators=(',', ':'))
        if error['actual'] == 'bool':
            token = b'"extra"' if name == 'unknown_field' else b'false'
            error['offset'] = source.encode().index(token)
        target = 'int' if name == 'scalar' else item_type if name == 'object' else item_type + '[]'
        if name == 'nested':
            target = 'JsonBox<JsonBox<JsonItem[]>>'
        body = f'jsonCalls = 0; const result = JSON.safeParse<{target}>(jsonSource); '
        if error['code']:
            checks = [f"error.{key} == {json.dumps(val) if val else 'null'}"
                      for key, val in error.items() if key != 'offset']
            checks += [f"error.offset == {error['offset'] if error['offset'] >= 0 else 'null'}"]
            body += ('if (result.success || jsonCalls != 0) { return 0; } '
                     'const error = result.error; return ' + ' && '.join(checks) + ' ? 42 : 0;')
            expected = 42
        else:
            body += 'if (!result.success) { return 0; } const value = result.value; '
            if name == 'scalar':
                body += 'return value;'
                expected = 42
            else:
                array = 'value.value.value' if name == 'nested' else 'value'
                number = 46 if constructed else 42
                if name == 'object':
                    body += "return value.text == 'Vela' ? value.value + value.text.length : 0;"
                    expected = 46
                else:
                    body += f'if ({array}.length != {count} || jsonCalls != {count if constructed else 0}) {{ return 0; }} '
                    body += 'let total = 0; let rank = 0; '
                    body += (f'for (const item in {array}) {{ if (item.text != \'Vela\') {{ return 0; }} '
                             'rank++; total += rank * (item.value + item.text.length); } return total;')
                    expected = (number + 4) * count * (count + 1) // 2
        records.append(dict(name='json/' + name, kind=kind, size=count, expected=expected,
                            setup='jsonSource = ' + json.dumps(source, ensure_ascii=False) + ';',
                            body=body, json=dict(input=source, **error)))
    return records
