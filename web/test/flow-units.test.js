import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

// Owner's unit rule: every visible flow rate is written in ml/s. A rate in g/s is the scale-derived mass rate,
// which is a different quantity and is not shown anywhere in the interface. These checks read the sources as
// text, so they also cover strings that only reach the screen through a chart canvas.
const srcDir = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'src');

function sourceFiles(dir) {
  return fs.readdirSync(dir, { withFileTypes: true }).flatMap(entry => {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) return entry.name === 'locales' ? [] : sourceFiles(full);
    return /\.(js|jsx)$/.test(entry.name) ? [full] : [];
  });
}

const stripComments = code => code.replace(/\/\*[\s\S]*?\*\//g, '').replace(/(^|[^:'"`])\/\/.*$/gm, '$1');
const MASS_RATE = /\bg\s*\/\s*s\b/;

test('no visible string in the web sources uses g/s', () => {
  const offenders = [];
  for (const file of sourceFiles(srcDir)) {
    const lines = stripComments(fs.readFileSync(file, 'utf8')).split(/\r?\n/);
    lines.forEach((line, index) => {
      const literals = line.match(/(['"`])(?:\\.|(?!\1).)*\1/g) || [];
      if (literals.some(literal => MASS_RATE.test(literal))) {
        offenders.push(`${path.relative(srcDir, file)}:${index + 1}: ${line.trim()}`);
      }
    });
  }
  assert.deepEqual(offenders, []);
});

test('no English message shown to the user mentions g/s', () => {
  const catalog = fs.readFileSync(path.join(srcDir, 'locales', 'en', 'messages.po'), 'utf8');
  const offenders = catalog.split(/\r?\n/).filter(line => /^msg(id|str) /.test(line) && MASS_RATE.test(line));
  assert.deepEqual(offenders, []);
});

test('the dashboard flow metric and the live trace state their unit as ml/s', () => {
  const metrics = fs.readFileSync(path.join(srcDir, 'utils', 'metricDefinitions.js'), 'utf8');
  const flowBlock = metrics.slice(metrics.indexOf("id: 'flow'"), metrics.indexOf("id: 'temp'"));
  assert.match(flowBlock, /unit: ' ml\/s'/);
  const trace = fs.readFileSync(path.join(srcDir, 'components', 'OverviewChart.jsx'), 'utf8');
  assert.match(trace, /Pump Flow/);
  assert.match(trace, /'ml\/s'/);
});

test('a pressure reading without a pressure target is not shown against a 0.0 bar target', () => {
  const metrics = fs.readFileSync(path.join(srcDir, 'utils', 'metricDefinitions.js'), 'utf8');
  const pressureBlock = metrics.slice(metrics.indexOf("id: 'pressure'"), metrics.indexOf("id: 'flow'"));
  assert.match(pressureBlock, /getTarget: ds => \(ds\.targetPressure > 0 \? ds\.targetPressure\.toFixed\(1\) : null\)/);
});
