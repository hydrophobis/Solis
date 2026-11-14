// Starts `sl lsp` for .sl files.
//
// Install from source (Linux, macOS, Windows):
//   cd tooling/editors/vscode && npm install
//   npx @vscode/vsce package          # makes solis-0.1.0.vsix
//   code --install-extension solis-0.1.0.vsix
// or, to try it without packaging, open this folder in VS Code and press F5.
// `sl` must be on PATH, or set "solis.serverPath".

const vscode = require('vscode');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

let client;

function start() {
  const config = vscode.workspace.getConfiguration('solis');
  const command = config.get('serverPath') || 'sl';
  client = new LanguageClient(
    'solis',
    'Solis',
    { command, args: ['lsp'], transport: TransportKind.stdio },
    {
      documentSelector: [{ scheme: 'file', language: 'solis' }, { scheme: 'untitled', language: 'solis' }],
      synchronize: { fileEvents: vscode.workspace.createFileSystemWatcher('**/*.sl') },
    }
  );
  client.start().catch((err) => {
    vscode.window.showErrorMessage(`Solis: could not start "${command} lsp": ${err.message}`);
  });
}

function activate(context) {
  start();
  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration(async (e) => {
      if (!e.affectsConfiguration('solis.serverPath')) return;
      if (client) await client.stop();
      start();
    })
  );
}

async function deactivate() {
  if (client) await client.stop();
}

module.exports = { activate, deactivate };
