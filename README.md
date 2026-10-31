# Vela

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Standard: C++23](https://img.shields.io/badge/C%2B%2B-23-purple.svg)](https://en.cppreference.com/w/cpp/23)

[简体中文](README.zh-CN.md)

Vela is a statically typed embedded scripting language designed specifically for native C++ applications.

> [!WARNING]
> Vela is currently in early active development and has not yet reached a usable state.

## Goals

- **Native C++ Embedding**: Clean, type-safe host function bindings and independent `Isolate` execution environments with zero global state.
- **Fast Register VM**: Predictable, efficient interpretation using a fixed-width register bytecode VM.
- **Native AOT Compilation**: A native AOT compiler backend planned for later stages, built on the shared Typed Intermediate Representation (Typed IR).
- **Low-Latency Runtime**: An incremental, non-moving mark-and-sweep garbage collector designed to minimize pause times.

## Example

```typescript
interface Entity {
  render(): string;
}

class Player implements Entity {
  name: string;
  health: int = 100;
  shield: int? = null;

  get isAlive(): bool {
    return this.health > 0;
  }

  render(): string {
    const effectiveShield = this.shield ?? 0;
    return '${this.name} (HP: ${this.health}, Shield: ${effectiveShield})';
  }
}

async func fetchBonus(): Future<int> {
  await sleep(10);
  return 25;
}

async func main(): Future<void> {
  const player = Player(name: 'Vela');
  player.health += await fetchBonus();

  print('Status: ${player.render()}');
  print('State: ${player.isAlive ? "Alive" : "Defeated"}');

  const skills = ['Dash', 'Parry', 'Focus'];
  for (const skill in skills) {
    print(' - ${skill}');
  }
}
```

## References & Influences

Vela draws design and implementation insights from:

- **Luau**: Register bytecode VM architecture and lightweight native embedding.
- **Dart**: Sound static type system, class parameter syntax, and isolate-based concurrency model.
- **JavaScript / TypeScript**: Syntax readability, modern async model, and collection conventions.
- **Wren / Lua**: Minimalist embedding boundaries and self-contained runtime philosophies.

## License

Vela is distributed under the [MIT License](LICENSE).
