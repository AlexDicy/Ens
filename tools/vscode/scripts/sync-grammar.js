// Copies the canonical shared editor assets from tools/grammar/ into this extension.
// VS Code requires these paths inside the extension folder for portability when
// packaging, so we keep the source of truth shared and produce build artifacts here.
// Run via `npm install` (prepare lifecycle) or `npm run sync-grammar`.

const fs = require('fs');
const path = require('path');

const sourceDirectory = path.resolve(__dirname, '..', '..', 'grammar');
const extensionRoot = path.resolve(__dirname, '..');

const files = [
    { source: 'ens.tmLanguage.json', destination: path.join('syntaxes', 'ens.tmLanguage.json') },
    { source: 'language-configuration.json', destination: 'language-configuration.json' },
];

for (const file of files) {
    const src = path.join(sourceDirectory, file.source);
    const dst = path.join(extensionRoot, file.destination);
    if (!fs.existsSync(src)) {
        console.error(`sync-grammar: source not found at ${src}`);
        process.exit(1);
    }
    fs.mkdirSync(path.dirname(dst), { recursive: true });
    fs.copyFileSync(src, dst);
    console.log(`sync-grammar: ${src} -> ${dst}`);
}
