# Vela

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Standard: C++23](https://img.shields.io/badge/C%2B%2B-23-purple.svg)](https://en.cppreference.com/w/cpp/23)

[English](README.md)

Vela 是一个专为原生 C++ 应用设计的静态类型嵌入式脚本语言。

> [!WARNING]
> Vela 目前处于早期活跃开发阶段，尚未到达可用状态。

## 项目目标

- **原生 C++ 嵌入**：提供类型安全的宿主绑定机制，基于独立的隔离区（`Isolate`）执行环境，无全局共享状态。
- **原生 JIT 执行**：基于共享 Typed IR 提供即时编译、交互执行与热重载。
- **原生 AOT 编译**：通过共享原生代码生成器构建独立可执行文件和可嵌入的原生产物。
- **低延迟运行时**：采用非移动式增量标记清除垃圾回收器（GC），最大程度消除全停顿延迟。

## 示例代码

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

## 参考项目

Vela 的语言和运行时设计汲取了以下项目的优秀思想：

- **Luau**：定宽寄存器字节码虚拟机架构与轻量宿主嵌入设计。
- **Dart**：严格静态类型系统、类构造形参语法与基于 Isolate 的隔离并发模型。
- **JavaScript / TypeScript**：表达力丰富的现代语法结构与内置集合约定。
- **Wren / Lua**：清晰简约的宿主交互边界与纯粹自包含的运行时理念。

## 许可证

Vela 基于 [MIT License](LICENSE) 协议发布。
