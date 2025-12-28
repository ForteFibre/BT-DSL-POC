import { formatBtDslText } from '../src/index.js';
import { readFileSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// When running from dist/test/, shared is at ../../shared
const soldierAiPath = join(__dirname, '../../../shared/examples/soldier-ai.bt');
const input = readFileSync(soldierAiPath, 'utf8');

console.log('=== Formatting soldier-ai.bt ===\n');

try {
  const output = await formatBtDslText(input, { filepath: soldierAiPath });
  console.log(output);
  console.log('\n=== Formatting successful ===');
} catch (error) {
  console.error('Error formatting:', error);
  process.exit(1);
}
