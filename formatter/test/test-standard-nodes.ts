import { formatBtDslText } from '../src/index.js';
import { readFileSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// When running from dist/test/, shared is at ../../../shared
const standardNodesPath = join(__dirname, '../../../shared/examples/StandardNodes.bt');
const input = readFileSync(standardNodesPath, 'utf8');

console.log('=== Formatting StandardNodes.bt ===\n');

try {
  const output = await formatBtDslText(input, { filepath: standardNodesPath });
  console.log(output);
  console.log('\n=== Formatting successful ===');
} catch (error) {
  console.error('Error formatting:', error);
  process.exit(1);
}
