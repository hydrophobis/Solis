const fs = require('fs');
const path = require('path');
const oniguruma = require('vscode-oniguruma');
const vsctm = require('vscode-textmate');

const wasmBin = fs.readFileSync(path.join(__dirname, 'node_modules/vscode-oniguruma/release/onig.wasm')).buffer;

function findSlFiles(dir) {
  let out = [];
  for (const name of fs.readdirSync(dir)) {
    const p = path.join(dir, name);
    const st = fs.statSync(p);
    if (st.isDirectory()) out = out.concat(findSlFiles(p));
    else if (name.endsWith('.sl')) out.push(p);
  }
  return out;
}

async function main() {
  await oniguruma.loadWASM(wasmBin);
  const onigLib = Promise.resolve({
    createOnigScanner(patterns) { return new oniguruma.OnigScanner(patterns); },
    createOnigString(s) { return new oniguruma.OnigString(s); }
  });

  const grammarJson = JSON.parse(fs.readFileSync(path.join(__dirname, 'syntaxes/solis.tmLanguage.json'), 'utf8'));
  const registry = new vsctm.Registry({
    onigLib,
    loadGrammar: async (scopeName) => (scopeName === 'source.solis' ? grammarJson : null)
  });
  const grammar = await registry.loadGrammar('source.solis');

  const root = path.join(__dirname, '..', '..');
  const files = [
    ...findSlFiles(path.join(root, 'examples')),
    ...findSlFiles(path.join(root, 'src', 'std'))
  ];

  let totalProblems = 0;
  let totalTokens = 0;

  for (const file of files) {
    const lines = fs.readFileSync(file, 'utf8').split('\n');
    let ruleStack = vsctm.INITIAL;
    let fileProblems = 0;
    for (let i = 0; i < lines.length; i++) {
      const line = lines[i];
      const r = grammar.tokenizeLine(line, ruleStack);
      ruleStack = r.ruleStack;
      for (const tok of r.tokens) {
        const text = line.substring(tok.startIndex, tok.endIndex);
        totalTokens++;
        if (text.trim() !== '' && tok.scopes.length <= 1) {
          console.log(`${path.relative(root, file)}:${i + 1}: UNSCOPED ${JSON.stringify(text)} in ${JSON.stringify(line)}`);
          fileProblems++;
          totalProblems++;
        }
      }
    }
    if (fileProblems === 0) console.log(`ok  ${path.relative(root, file)}`);
    else console.log(`BAD ${path.relative(root, file)}  (${fileProblems} unscoped)`);
  }

  console.log('---');
  console.log(`${files.length} files, ${totalTokens} tokens, ${totalProblems} unscoped`);

  // Targeted checks for constructs worth naming explicitly.
  const checks = [
    { line: 'let name: str = "hello ${x + 1}!";', must: ['meta.embedded.expression.solis', 'string.quoted.double.solis'] },
    { line: 'func draw_all[T: Drawable](items: [T], c: Canvas) {', must: ['punctuation.definition.generic.begin.solis'] },
    { line: 'weak parent: Node;', must: ['storage.modifier.solis'] },
    { line: '    case Shape.Circle(r):', must: ['keyword.control.solis'] },
    { line: '/* nested /* comment */ still going */', must: ['comment.block.solis'] }
  ];
  let checkFail = 0;
  for (const c of checks) {
    const r = grammar.tokenizeLine(c.line, vsctm.INITIAL);
    const scopes = new Set(r.tokens.flatMap(t => t.scopes));
    for (const m of c.must) {
      if (!scopes.has(m)) {
        console.log(`CHECK FAIL: ${JSON.stringify(c.line)} missing scope ${m}`);
        checkFail++;
      }
    }
  }
  console.log(checkFail === 0 ? 'targeted checks: all passed' : `targeted checks: ${checkFail} failed`);

  if (totalProblems > 0 || checkFail > 0) process.exit(1);
}

main().catch(e => { console.error(e); process.exit(1); });
