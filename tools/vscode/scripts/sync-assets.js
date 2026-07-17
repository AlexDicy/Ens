// Copies the canonical shared editor assets into this extension.
// VS Code requires these paths inside the extension folder for portability when
// packaging, so we keep one source of truth and produce build artifacts here.
// Run via `npm install` or `npm run sync-assets`.

const fs = require('fs');
const path = require('path');

const toolsDirectory = path.resolve(__dirname, '..', '..');
const extensionRoot = path.resolve(__dirname, '..');

const files = [
    {
        source: path.join('grammar', 'ens.tmLanguage.json'),
        destination: path.join('syntaxes', 'ens.tmLanguage.json'),
    },
    {
        source: path.join('grammar', 'language-configuration.json'),
        destination: 'language-configuration.json',
    },
    {
        source: path.join('assets', 'ens-file-icon-light.svg'),
        destination: path.join('images', 'ens-file-icon-light.svg'),
    },
    {
        source: path.join('assets', 'ens-file-icon.svg'),
        destination: path.join('images', 'ens-file-icon.svg'),
    },
    {
        source: path.join('assets', 'ens-logo.png'),
        destination: path.join('images', 'ens-logo.png'),
    },
];

for (const file of files) {
    const source = path.join(toolsDirectory, file.source);
    const destination = path.join(extensionRoot, file.destination);
    if (!fs.existsSync(source)) {
        console.error(`sync-assets: source not found at ${source}`);
        process.exit(1);
    }
    fs.mkdirSync(path.dirname(destination), { recursive: true });
    fs.copyFileSync(source, destination);
    console.log(`sync-assets: ${source} -> ${destination}`);
}
