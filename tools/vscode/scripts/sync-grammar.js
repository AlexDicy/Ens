// Copies the canonical TextMate grammar from tools/grammar/ into this extension's
// syntaxes/ folder. VS Code requires grammar paths inside the extension folder for
// portability when packaging, so we keep the source of truth shared and produce a
// build artifact here. Run via `npm install` (prepare lifecycle) or `npm run sync-grammar`.

const fs = require('fs');
const path = require('path');

const src = path.resolve(__dirname, '..', '..', 'grammar', 'ens.tmLanguage.json');
const dstDir = path.resolve(__dirname, '..', 'syntaxes');
const dst = path.join(dstDir, 'ens.tmLanguage.json');

if (!fs.existsSync(src)) {
    console.error(`sync-grammar: source not found at ${src}`);
    process.exit(1);
}

fs.mkdirSync(dstDir, { recursive: true });
fs.copyFileSync(src, dst);
console.log(`sync-grammar: ${src} -> ${dst}`);
