// node --test --experimental-strip-types src/scripts/ (npm test). Cases from tests/test_deeplink.py.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  DEMO_MANIFEST, checkUrl, githubRelease, isDemo, parseInstallParams, rawGithubUrl, readManifest, toFrameportLink,
} from './install-link.ts';

const MANIFEST = 'https://cdn.example.com/game.framedrop.json';

test('a manifest link becomes the frameport:// link the app parses', () => {
  const p = parseInstallParams(`?manifest=${encodeURIComponent(MANIFEST)}`);
  assert.equal(p.ok, true);
  assert.equal(p.link, `frameport://install?manifest=${encodeURIComponent(MANIFEST)}`);
  // FrameDrop's own example leaves the manifest unencoded
  assert.equal(parseInstallParams(`?manifest=${MANIFEST}`).target, MANIFEST);
});

test('direct file links', () => {
  const url = 'https://github.com/x/y/releases/download/v1/Game-arm64.apk';
  const p = parseInstallParams(`?url=${encodeURIComponent(url)}`);
  assert.deepEqual([p.ok, p.kind, p.link], [true, 'url', toFrameportLink('url', url)]);
  assert.equal(parseInstallParams('?url=https%3A%2F%2Fa.com%2Freadme.txt').ok, false);
});

for (const [url, reason] of [
  ['https://user:pw@cdn.example.com/x.apk', 'password'],
  ['https://192.168.1.20/x.apk', 'local network'],
  ['https://10.0.0.1/x.apk', 'local network'],
  ['https://[fe80::1]/x.apk', 'local network'],
  ['https://[::ffff:192.168.0.1]/x.apk', 'local network'],
  ['https://3232235777/x.apk', 'local network'], // 192.168.1.1 written as one number
  ['https://cdn.example.com/builds/', 'file name'],
  ['http://cdn.example.com/x.apk', 'https'],
  ['ftp://cdn.example.com/x.apk', 'https'],
  ['javascript:alert(1)', 'https'],
]) {
  test(`refused: ${url}`, () => assert.match(checkUrl(url) ?? '', new RegExp(reason)));
}

test('http only on this PC; public addresses pass', () => {
  assert.equal(checkUrl('http://localhost:8000/x.apk'), null);
  assert.equal(checkUrl('https://8.8.8.8/x.apk'), null);
  assert.equal(checkUrl('https://cdn.example.com/My%20Game.apk'), null);
});

test('missing parameters', () => {
  assert.equal(parseInstallParams('').ok, false);
  assert.equal(parseInstallParams('?manifest=').ok, false);
});

test('manifest preview', () => {
  const m = readManifest({ schema: 'framedrop.install/v1', name: ' Nice Game ', files: [
    { url: 'https://cdn.example.com/My%20Game-arm64.apk', sha256: 'AB'.repeat(32) },
    { url: 'https://192.168.1.2/evil.obb' },
  ], frameport: { description: 'A game.', icon: 'http://example.com/i.png' } });
  assert.deepEqual(m, { name: 'Nice Game', description: 'A game.', icon: null, files: ['My Game-arm64.apk'] });
  assert.equal(readManifest({ schema: 'framedrop.install/v2', name: 'x', files: [{ url: 'https://a.com/x.apk' }] }), null);
  assert.equal(readManifest({ schema: 'framedrop.install/v1', files: [{ url: 'https://a.com/x.apk' }] }), null);
});

test('the homepage demo link is recognised (and only it)', () => {
  const p = parseInstallParams(`?manifest=${encodeURIComponent(DEMO_MANIFEST)}`);
  assert.deepEqual([p.ok, p.kind, p.demo], [true, 'manifest', true]);
  assert.equal(p.link, `frameport://install?manifest=${encodeURIComponent(DEMO_MANIFEST)}`);
  assert.equal(parseInstallParams(`?manifest=${encodeURIComponent(MANIFEST)}`).demo, false);
  assert.equal(isDemo('https://www.frameport.app/demo/cool-game.json'), true);
  for (const u of ['https://frameport.app/demo/cool-game.json?x=1', 'https://frameport.app.evil.com/demo/cool-game.json',
    'http://frameport.app/demo/cool-game.json', 'https://frameport.app:8443/demo/cool-game.json']) {
    assert.equal(isDemo(u), false, u);
  }
});

test('the published demo manifest previews as Cool Game', () => {
  const data = JSON.parse(readFileSync(new URL('../../public/demo/cool-game.json', import.meta.url), 'utf8'));
  const m = readManifest(data);
  assert.equal(m?.name, 'Cool Game');
  assert.equal(m?.icon, 'https://frameport.app/demo/cool-game.svg');
  assert.deepEqual(m?.files, ['cool-game-arm64.apk']);
});

test('GitHub file pages become raw.githubusercontent.com addresses (also in the frameport:// link)', () => {
  const raw = 'https://raw.githubusercontent.com/o/r/main/dir/game.json';
  for (const page of ['https://github.com/o/r/blob/main/dir/game.json', 'https://github.com/o/r/raw/main/dir/game.json',
    'https://www.github.com/o/r/blob/main/dir/game.json']) {
    assert.equal(rawGithubUrl(page), raw);
  }
  const p = parseInstallParams(`?manifest=${encodeURIComponent('https://github.com/o/r/blob/v1.2/game.json')}`);
  assert.equal(p.target, 'https://raw.githubusercontent.com/o/r/v1.2/game.json');
  assert.equal(p.link, toFrameportLink('manifest', p.target));
  // everything else is left alone: release downloads, raw addresses, other hosts
  for (const other of ['https://github.com/o/r/releases/latest/download/game.json', raw,
    'https://example.com/o/r/blob/main/game.json', 'http://github.com/o/r/blob/main/game.json']) {
    assert.equal(rawGithubUrl(other), other);
  }
});

test('GitHub release downloads are recognised (for the release name from the API)', () => {
  assert.deepEqual(githubRelease('https://github.com/o/r/releases/latest/download/frameport-install.json'),
    { owner: 'o', repo: 'r', tag: null });
  assert.deepEqual(githubRelease('https://github.com/o/r/releases/download/v0.1.0/frameport-install.json'),
    { owner: 'o', repo: 'r', tag: 'v0.1.0' });
  assert.equal(githubRelease('https://github.com/o/r/blob/main/x.json'), null);
  assert.equal(githubRelease('https://raw.githubusercontent.com/o/r/main/x.json'), null);
});
