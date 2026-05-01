# BitLang VS Code Extension

Adds syntax highlighting for `.bitlang` files in Visual Studio Code.

## Features

- Comments (`#`)
- String literals (`!"..."`) with escape sequences
- Hex and char literals (`!#FF`, `!'A'`)
- All commands: `!O`, `!I`, `!E`, `!X`, `!L`, `!W`, `!$VAR`, `!~`, `!^`
- Selection syntax (`![n:m]`, `![M:@]`, etc.)
- Control flow (`!!`, `[`, `]`, `$...$!`)
- Navigation and bit operators

## Installation

### From source (manual)

1. Copy the `vscode-bitlang` folder into your VS Code extensions directory:
   - **Linux/macOS**: `~/.vscode/extensions/`
   - **Windows**: `%USERPROFILE%\.vscode\extensions\`
2. Restart VS Code.

### Via VSIX (if packaged)

```
vsce package
code --install-extension vscode-bitlang-0.1.0.vsix
```
