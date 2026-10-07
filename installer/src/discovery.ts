import {createSocket, type Socket} from 'node:dgram';
import {networkInterfaces} from 'node:os';
import {privateIPv4} from './model.js';
export type Light = {ip: string; name: string; deviceId: string; mac?: string; installed: boolean};
type RecordValue = {name: string; type: number; ttl: number; value: string | string[] | {port: number; target: string}};
const stock = '_wled._tcp.local.', native = '_openkeylight._tcp.local.';
const decoder = new TextDecoder('utf-8', {fatal: true});
function need(v: unknown): asserts v {if (!v) throw new Error('Malformed discovery response.');}

/** Only service PTR questions, with a unicast-response request (RFC 6762). */
export function discoveryQuery(service: typeof stock | typeof native): Buffer {
  need(service === stock || service === native);
  const header = Buffer.alloc(12); header.writeUInt16BE(1, 4);
  return Buffer.concat([header, ...service.slice(0, -1).split('.').map(label => Buffer.concat([Buffer.from([label.length]), Buffer.from(label)])), Buffer.from([0, 0, 12, 0x80, 1])]);
}

export function decodeDns(packet: Buffer): RecordValue[] {
  need(packet.length >= 12 && packet.length <= 9000 && (packet.readUInt16BE(2) & 0x800f) === 0x8000);
  const name = (start: number): {value: string; next: number} => {
    let at = start, next = -1, bytes = 1; const labels: string[] = [], visited = new Set<number>();
    while (true) {
      need(at < packet.length && !visited.has(at) && visited.size < 128); visited.add(at);
      const length = packet[at++]!;
      if (!length) return {value: labels.join('.') + '.', next: next < 0 ? at : next};
      if ((length & 0xc0) === 0xc0) {need(at < packet.length); if (next < 0) next = at + 1; at = ((length & 63) << 8) | packet[at]!; continue;}
      need(length < 64 && at + length <= packet.length); bytes += length + 1; need(bytes <= 255);
      labels.push(decoder.decode(packet.subarray(at, at + length))); at += length;
    }
  };
  const questions = packet.readUInt16BE(4), count = packet.readUInt16BE(6) + packet.readUInt16BE(8) + packet.readUInt16BE(10);
  need(questions <= 16 && count <= 128); let at = 12; const records: RecordValue[] = [];
  for (let q = 0; q < questions; q++) {at = name(at).next + 4; need(at <= packet.length);}
  for (let r = 0; r < count; r++) {
    const owner = name(at); at = owner.next; need(at + 10 <= packet.length);
    const type = packet.readUInt16BE(at), cls = packet.readUInt16BE(at + 2), ttl = packet.readUInt32BE(at + 4), length = packet.readUInt16BE(at + 8);
    at += 10; const end = at + length; need(end <= packet.length); let value: RecordValue['value'] | undefined;
    if (type === 1) {need(length === 4); value = [...packet.subarray(at, end)].join('.');}
    else if (type === 12) {const result = name(at); need(result.next === end); value = result.value;}
    else if (type === 33) {need(length >= 7); const result = name(at + 6); need(result.next === end); value = {port: packet.readUInt16BE(at + 4), target: result.value};}
    else if (type === 16) {const items: string[] = []; let position = at; while (position < end) {const size = packet[position++]!; need(position + size <= end); items.push(decoder.decode(packet.subarray(position, position + size))); position += size;} value = items;}
    if ((cls & 0x7fff) === 1 && value !== undefined) records.push({name: owner.value, type, ttl, value}); at = end;
  }
  need(at === packet.length); return records;
}

/** Discovery is a selection hint, never installation identity/qualification proof. */
export function lightsFromRecords(records: RecordValue[], source: string): Light[] {
  if (!privateIPv4(source)) return [];
  const results: Light[] = [], equal = (a: string, b: string) => a.toLowerCase() === b.toLowerCase();
  for (const ptr of records.filter(r => r.ttl > 0 && r.type === 12 && (equal(r.name, stock) || equal(r.name, native)))) {
    if (typeof ptr.value !== 'string') continue;
    const instance = ptr.value, suffix = equal(ptr.name, stock) ? stock : native, installed = suffix === native;
    const srvs = records.filter(r => r.ttl > 0 && r.type === 33 && equal(r.name, instance));
    const txts = records.filter(r => r.ttl > 0 && r.type === 16 && equal(r.name, instance));
    if (srvs.length !== 1 || txts.length !== 1 || typeof srvs[0]!.value !== 'object' || Array.isArray(srvs[0]!.value) || !Array.isArray(txts[0]!.value)) continue;
    const srv = srvs[0]!.value, txt = txts[0]!.value;
    if (srv.port !== (installed ? 80 : 10003) || !instance.toLowerCase().endsWith('.' + suffix)) continue;
    if (!records.some(r => r.ttl > 0 && r.type === 1 && equal(r.name, srv.target) && r.value === source)) continue;
    const name = instance.slice(0, -(suffix.length + 1)); if (!name || Buffer.byteLength(name) > 64 || /[\x00-\x1f\x7f]/.test(name)) continue;
    if (installed) {
      const id = srv.target.toLowerCase().match(/^(keylight-[a-f0-9]{6})\.local\.$/)?.[1];
      if (id && txt.includes('model=keylight-chroma') && txt.includes('api=1')) results.push({ip: source, name, deviceId: id, installed: true});
    } else {
      const macs = txt.filter(v => /^mac=/i.test(v)); if (macs.length !== 1) continue;
      const mac = macs[0]!.slice(4).toLowerCase();
      const products = txt.filter(v => v.startsWith('exData='));
      if (!/^[a-f0-9]{12}$/.test(mac) || products.length !== 1 || !products[0]!.includes('VID_1532&PID_030C&') || !products[0]!.toLowerCase().includes(`mac_${mac}`)) continue;
      results.push({ip: source, name, mac, deviceId: `keylight-${mac.slice(-6)}`, installed: false});
    }
  }
  return results;
}

export type DiscoveryOptions = {signal?: AbortSignal; seconds?: number; interfaces?: string[]; socket?: () => Socket};
/** Two read-only multicast queries per IPv4 interface; no TCP or light commands. */
export async function discoverLights(options: DiscoveryOptions = {}): Promise<Light[]> {
  if (options.signal?.aborted) throw new Error('Discovery cancelled.');
  const seconds = options.seconds ?? 4; need(seconds > 0 && seconds <= 6);
  const interfaces = [...new Set(options.interfaces ?? Object.values(networkInterfaces()).flatMap(items => (items ?? []).filter(n => !n.internal && n.family === 'IPv4' && privateIPv4(n.address)).map(n => n.address)))].slice(0, 8);
  if (!interfaces.length) throw new Error('No private IPv4 network interface is available.');
  const all: Light[] = [];
  await Promise.all(interfaces.map(address => new Promise<void>(done => {
    const socket = options.socket?.() ?? createSocket('udp4'); const records = new Map<string, {record: RecordValue; expires: number}[]>(); let packets = 0, closed = false;
    const close = () => {
      if (closed) return; closed = true; clearTimeout(timer); options.signal?.removeEventListener('abort', close);
      for (const [source, cached] of records) all.push(...lightsFromRecords(cached.filter(v => v.expires > performance.now()).map(v => v.record), source));
      try {socket.close();} catch {} done();
    };
    const timer = setTimeout(close, seconds * 1000); options.signal?.addEventListener('abort', close, {once: true});
    socket.on('error', close);
    socket.on('message', (packet, remote) => {
      if (closed || ++packets > 256 || remote.port !== 5353 || !privateIPv4(remote.address)) return;
      try {
        if (records.size >= 64 && !records.has(remote.address)) return;
        const incoming = decodeDns(packet), previous = records.get(remote.address) ?? [], now = performance.now();
        const keys = new Set(incoming.map(r => `${r.name.toLowerCase()}|${r.type}`));
        const merged = [...previous.filter(v => v.expires > now && !keys.has(`${v.record.name.toLowerCase()}|${v.record.type}`)), ...incoming.filter(r => r.ttl > 0).map(record => ({record, expires: now + record.ttl * 1000}))].slice(-128);
        records.set(remote.address, merged);
      } catch { /* Unrelated or malformed LAN replies are never trusted. */ }
    });
    socket.bind(0, address, () => {if (closed) return; try {socket.setMulticastInterface(address); socket.setMulticastTTL(255); for (const service of [stock, native] as const) socket.send(discoveryQuery(service), 5353, '224.0.0.251', error => {if (error) close();});} catch {close();}});
  })));
  if (options.signal?.aborted) throw new Error('Discovery cancelled.');
  const unique = new Map(all.map(light => [`${light.ip}|${light.deviceId}`, light]));
  return [...unique.values()].sort((a, b) => a.name.localeCompare(b.name));
}
