IntelliJ IDEA, CLion, GoLand, PyCharm and the other JetBrains IDEs (Linux and Windows)

JetBrains IDEs take a language server through the free LSP4IJ plugin.

1. Settings > Plugins > Marketplace: install "LSP4IJ".
2. Settings > Languages & Frameworks > Language Servers > + (New Language Server).
3. Name: Solis
   Command: sl lsp          (sl.exe lsp, or the full path, if sl is not on PATH)
4. Mappings tab > File name patterns: add   *.sl   with language id   solis
5. Apply. Open a .sl file; diagnostics, hover, go to definition, find usages
   and completion come from the server.

For highlighting, Settings > Editor > File Types > TextMate Bundles can load
tooling/editors/vscode (it contains a TextMate grammar).
