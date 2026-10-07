import {test} from 'node:test';
import assert from 'node:assert/strict';
import {realpathSync} from 'node:fs';
import {EventEmitter} from 'node:events';
import type {Socket} from 'node:dgram';
import {createHash} from 'node:crypto';
import {mkdtemp, writeFile, readFile, rm, mkdir, symlink} from 'node:fs/promises';
import {join} from 'node:path';
import {tmpdir} from 'node:os';
import {loadBundle, findBundle} from '../src/bundle.js';
import {decodeDns, lightsFromRecords, discoveryQuery, discoverLights} from '../src/discovery.js';

const dnsName = (value: string) => Buffer.concat([...value.split('.').filter(Boolean).map(label => Buffer.concat([Buffer.from([Buffer.byteLength(label)]), Buffer.from(label)])), Buffer.from([0])]);
const rr = (name: string, type: number, data: Buffer, ttl = 120) => {const header = Buffer.alloc(10); header.writeUInt16BE(type); header.writeUInt16BE(1, 2); header.writeUInt32BE(ttl, 4); header.writeUInt16BE(data.length, 8); return Buffer.concat([dnsName(name), header, data]);};
function packet(native = false) {
  const service = native ? '_openkeylight._tcp.local.' : '_wled._tcp.local.', instance = `Studio light.${service}`, host = native ? 'keylight-aabbcc.local.' : 'Studio light.local.';
  const srv = Buffer.alloc(6); srv.writeUInt16BE(native ? 80 : 10003, 4);
  const strings = native ? ['api=1', 'model=keylight-chroma'] : ['mac=112233AABBCC', 'exData=ProductType_1234&VID_1532&PID_030C&EID_0000&NAME_Studio light\\MAC_112233AABBCC'];
  const entries = [rr(service, 12, dnsName(instance)), rr(instance, 33, Buffer.concat([srv, dnsName(host)])), rr(instance, 16, Buffer.concat(strings.map(value => Buffer.concat([Buffer.from([Buffer.byteLength(value)]), Buffer.from(value)])))), rr(host, 1, Buffer.from([192, 168, 1, 25]))];
  const header = Buffer.alloc(12); header.writeUInt16BE(0x8400, 2); header.writeUInt16BE(4, 6); return Buffer.concat([header, ...entries]);
}
test('mDNS joins only matching service, product, MAC, address and port hints', () => {
  assert.deepEqual(lightsFromRecords(decodeDns(packet()), '192.168.1.25'), [{ip: '192.168.1.25', name: 'Studio light', mac: '112233aabbcc', deviceId: 'keylight-aabbcc', installed: false}]);
  assert.equal(lightsFromRecords(decodeDns(packet(true)), '192.168.1.25')[0]?.installed, true);
  assert.deepEqual(lightsFromRecords(decodeDns(packet()), '192.168.1.26'), []);
  const unrelated = Buffer.from(packet().toString('latin1').replace('VID_1532', 'VID_9999'), 'latin1');
  assert.deepEqual(lightsFromRecords(decodeDns(unrelated), '192.168.1.25'), []);
  const query = discoveryQuery('_wled._tcp.local.'); assert.equal(query.readUInt16BE(4), 1); assert.equal(query.readUInt16BE(query.length - 2), 0x8001);
});
test('DNS parser rejects every truncation, compression cycle, oversized count and invalid UTF-8', () => {
  const full = packet(); for (let i = 0; i < full.length; i++) assert.throws(() => decodeDns(full.subarray(0, i)));
  const cycle = Buffer.alloc(24); cycle.writeUInt16BE(0x8000, 2); cycle.writeUInt16BE(1, 6); cycle[12] = 0xc0; cycle[13] = 12; assert.throws(() => decodeDns(cycle));
  const count = Buffer.from(full); count.writeUInt16BE(129, 6); assert.throws(() => decodeDns(count));
  const utf = Buffer.from(full); utf[13] = 0xff; assert.throws(() => decodeDns(utf));
});
test('discovery sends exactly two read-only service queries, bounds time, and closes on cancellation', async () => {
  const sockets: any[] = [];
  const socket = () => {
    const s = Object.assign(new EventEmitter(), {sent: [] as Buffer[], closed: false,
      bind: (_port: number, _address: string, ready: () => void) => {queueMicrotask(ready);}, setMulticastInterface: () => {}, setMulticastTTL: () => {},
      send: (bytes: Buffer, port: number, address: string, cb: (e?: Error) => void) => {assert.equal(port, 5353); assert.equal(address, '224.0.0.251'); s.sent.push(bytes); s.emit('message', packet(), {address: '192.168.1.25', port: 5353}); cb();},
      close: () => {s.closed = true;},
    }); sockets.push(s); return s as unknown as Socket;
  };
  const lights = await discoverLights({interfaces: ['192.168.1.2'], seconds: 0.01, socket});
  assert.equal(lights.length, 1); assert.equal(sockets[0].sent.length, 2); assert.equal(sockets[0].closed, true);
  const abort = new AbortController(), pending = discoverLights({interfaces: ['192.168.1.2'], socket, signal: abort.signal}); abort.abort();
  await assert.rejects(pending, /cancelled/); assert.equal(sockets[1].closed, true); assert.equal(sockets[1].sent.length, 0);
});

test('mDNS goodbye replaces an earlier hint instead of leaving a stale selectable light', async () => {
  const header = Buffer.alloc(12); header.writeUInt16BE(0x8400, 2); header.writeUInt16BE(1, 6);
  const goodbye = Buffer.concat([header, rr('_wled._tcp.local.', 12, dnsName('Studio light._wled._tcp.local.'), 0)]);
  const socket = () => {
    let sends = 0;
    const s = Object.assign(new EventEmitter(), {bind: (_p: number, _a: string, ready: () => void) => queueMicrotask(ready), setMulticastInterface: () => {}, setMulticastTTL: () => {}, close: () => {},
      send: (_b: Buffer, _p: number, _a: string, callback: () => void) => {s.emit('message', ++sends === 1 ? packet() : goodbye, {address: '192.168.1.25', port: 5353}); callback();}});
    return s as unknown as Socket;
  };
  assert.deepEqual(await discoverLights({interfaces: ['192.168.1.2'], seconds: 0.01, socket}), []);
});
async function withBundle(fn: (root: string, manifest: any) => Promise<void>) {
  const root = await mkdtemp(join(tmpdir(), 'okl bundle '));
  const entry = async (path: string, size: number) => {const bytes = Buffer.alloc(size, path.length); await writeFile(join(root, path), bytes); return {path, bytes: size, sha256: createHash('sha256').update(bytes).digest('hex')};};
  try {
    const manifest = {format: 1, product: 'open-keylight-chroma', version: '0.2.0-alpha.1', source_commit: 'a'.repeat(40), stock_profile: 'keylight-chroma-1.0.13',
      packages: {identity: await entry('identity.oklnxp', 28736), OFF1: await entry('OFF1.oklnxp', 28736), LOW1: await entry('LOW1.oklnxp', 28736), lighting: await entry('lighting.oklnxp', 28736)}, esp: await entry('app.bin', 400), assets: await entry('assets.json', 100)};
    await writeFile(join(root, 'bundle.json'), JSON.stringify(manifest)); await fn(root, manifest);
  } finally {await rm(root, {recursive: true, force: true});}
}
test('bundle binds all six contents and derives paths; changed artifact or manifest rejects', async () => {
  await withBundle(async (root, v) => {
    const result = await findBundle(undefined, root); assert.equal(result.version, '0.2.0-alpha.1'); assert.equal(realpathSync.native(result.files.low1), realpathSync.native(join(root, 'LOW1.oklnxp')));
    await writeFile(join(root, 'app.bin'), Buffer.alloc(400)); await assert.rejects(loadBundle(join(root, 'bundle.json')));
    for (const path of ['../app.bin', '/app.bin', 'C:/app.bin', 'nested\\app.bin', 'a//b', './app.bin']) {
      v.esp.path = path; await writeFile(join(root, 'bundle.json'), JSON.stringify(v)); await assert.rejects(loadBundle(join(root, 'bundle.json')));
    }
  });
});
test('bundle follows no symlink escape and never skips a malformed primary bundle', async () => {
  await withBundle(async (root, v) => {
    const outside = await mkdtemp(join(tmpdir(), 'okl outside '));
    try {
      await writeFile(join(outside, 'app.bin'), await readFile(join(root, 'app.bin')));
      await symlink(outside, join(root, 'escape'), process.platform === 'win32' ? 'junction' : 'dir');
      v.esp.path = 'escape/app.bin'; await writeFile(join(root, 'bundle.json'), JSON.stringify(v));
      await assert.rejects(loadBundle(join(root, 'bundle.json')));
      await mkdir(join(root, 'release')); await writeFile(join(root, 'release/bundle.json'), '{}');
      await assert.rejects(findBundle(undefined, root));
    } finally {await rm(outside, {recursive: true, force: true});}
  });
});
