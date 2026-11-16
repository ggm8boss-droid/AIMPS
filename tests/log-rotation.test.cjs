const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const zlib = require('node:zlib');
const { test } = require('node:test');

const { rotateLogs, rollLog } = require('../AIMP.Electron/log-rotation');

function temporaryDirectory() {
  return fs.mkdtempSync(path.join(process.env.AIMP_TEST_TMP || os.tmpdir(), 'aimp-log-rotation-'));
}

test('small logs are left in place', async () => {
  const dir = temporaryDirectory();
  const log = path.join(dir, 'aimp-electron.log');
  fs.writeFileSync(log, 'small\n');
  assert.equal(rollLog(log, { maxBytes: 1024 }), null);
  await rotateLogs([log], { maxBytes: 1024 });
  assert.deepEqual(fs.readdirSync(dir), ['aimp-electron.log']);
  fs.rmSync(dir, { recursive: true, force: true });
});

test('large logs are rolled, gzipped losslessly and capped per log', async () => {
  const dir = temporaryDirectory();
  const log = path.join(dir, 'aimp-launcher-api.log');
  const contents = [];
  for (let run = 0; run < 13; run += 1) {
    const text = `run ${run} ünïcødé\n`.repeat(200);
    contents.push(Buffer.from(text, 'utf8'));
    fs.writeFileSync(log, text);
    await rotateLogs([log], { maxBytes: 1024, maxArchives: 10, now: new Date(Date.UTC(2026, 10, 1, 0, run)) });
  }
  const archives = fs.readdirSync(dir).filter((name) => name.endsWith('.gz')).sort();
  assert.equal(archives.length, 10);
  assert.equal(fs.readdirSync(dir).filter((name) => name.endsWith('.tmp')).length, 0);
  assert.equal(fs.existsSync(log), false, 'next session starts a fresh file');
  archives.forEach((name, index) => {
    const restored = zlib.gunzipSync(fs.readFileSync(path.join(dir, name)));
    assert.ok(restored.equals(contents[index + 3]), `${name} restores exactly`);
  });
  fs.rmSync(dir, { recursive: true, force: true });
});

test('rotation errors are reported, never thrown', async () => {
  const messages = [];
  await rotateLogs([path.join(temporaryDirectory(), 'missing', 'x.log')], { log: (m) => messages.push(m) });
  assert.equal(messages.length, 1);
});
