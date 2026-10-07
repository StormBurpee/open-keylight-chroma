import {StringDecoder} from 'node:string_decoder';
import {stagesFor, type InstallationMode, type Progress} from './model.js';

type RecordValue = Record<string, unknown>;
const record = (v: unknown): v is RecordValue => !!v && typeof v === 'object' && !Array.isArray(v);
const integer = (v: unknown, min: number, max: number): v is number => Number.isSafeInteger(v) && Number(v) >= min && Number(v) <= max;
const text = (v: unknown, max = 1000): v is string => typeof v === 'string' && v.length > 0 && v.length <= max;
function requireValue(test: unknown): asserts test {if (!test) throw new Error('The installer backend returned an invalid or out-of-order event.');}

/** Strict presentation state. Success comes from verified backend events, never a timer or process exit. */
export class EventState {
  progress: Progress = {stage: 0, state: 'waiting', label: 'Starting the reviewed installer…', finishedStages: []};
  terminal: 'installed' | 'stopped' | undefined;
  lastSequence = -1;
  private active = -1;
  private finished = new Set<number>();
  private promptIds = new Set<string>();
  constructor(readonly target: {ip: string; deviceId: string; version: string; elf: string; manifest: string; controllerVersion?: string; workflow?: InstallationMode}) {
    this.progress.workflow = target.workflow ?? 'install';
  }

  accept(value: unknown): void {
    requireValue(record(value) && value.v === 1 && integer(value.seq, 0, 100000) && value.seq === this.lastSequence + 1 && !this.terminal);
    const e = value;
    const stages = stagesFor(this.target.workflow);
    requireValue(text(e.event, 40));
    switch (e.event) {
      case 'stage': {
        requireValue(integer(e.index, 1, stages.length) && e.total === stages.length && e.id === stages[e.index - 1]![0] && text(e.message));
        const index = e.index - 1;
        if (e.phase === 'started') {
          requireValue(index === this.finished.size && (this.active === -1 || this.finished.has(this.active)) && !this.progress.prompt);
          this.active = index;
          this.progress = {workflow: this.target.workflow ?? 'install', stage: index, state: 'running', label: e.message, finishedStages: [...this.finished], cancelRequested: this.progress.cancelRequested};
        } else {
          requireValue(e.phase === 'completed' && index === this.active && !this.finished.has(index) && !this.progress.prompt);
          this.finished.add(index);
          this.progress = {...this.progress, state: 'waiting', label: e.message, completed: undefined, total: undefined, finishedStages: [...this.finished]};
        }
        break;
      }
      case 'progress': {
        requireValue(this.active >= 0 && e.stage_id === stages[this.active]![0] && !this.finished.has(this.active)
          && ['controller_program', 'controller_verify', 'esp_upload', 'esp_processing', 'quiet', 'startup_wait', 'trial'].includes(String(e.scope))
          && ['blocks', 'bytes', 'seconds', 'percent'].includes(String(e.unit))
          && typeof e.total === 'number' && Number.isFinite(e.total) && e.total > 0
          && typeof e.completed === 'number' && Number.isFinite(e.completed) && e.completed >= 0 && e.completed <= e.total);
        if (e.remaining_ms !== undefined) requireValue(integer(e.remaining_ms, 0, 180000));
        const labels: Record<string, string> = {controller_program: 'Writing the controller bank', controller_verify: 'Reading back every byte', esp_upload: 'Sending the dashboard application', esp_processing: 'Device processing the application', quiet: 'Allowing the controller to finish safely', startup_wait: 'Waiting for the new dashboard', trial: 'Completing automatic setup and checking acceptance'};
        this.progress = {...this.progress, state: e.scope === 'quiet' ? 'quiet' : 'running', label: labels[String(e.scope)]!, completed: e.completed, total: e.total,
          remainingSeconds: e.remaining_ms === undefined ? undefined : Math.ceil(Number(e.remaining_ms) / 1000)};
        break;
      }
      case 'prompt':
        requireValue(this.target.workflow !== 'finish' && this.active >= 0 && !this.progress.prompt && text(e.id, 80) && !this.promptIds.has(e.id)
          && ['off1_observation', 'low1_observation'].includes(String(e.kind)) && text(e.message)
          && Array.isArray(e.choices) && e.choices.length === 2 && e.choices[0] === 'yes' && e.choices[1] === 'no');
        this.promptIds.add(e.id);
        this.progress = {...this.progress, state: 'prompt', prompt: {id: e.id, question: e.message}};
        break;
      case 'action': {
        requireValue(this.active === stages.length - 1 && e.kind === 'native_acceptance' && e.device_id === this.target.deviceId
          && e.firmware === this.target.version && e.elf_sha256 === this.target.elf && typeof e.pairing_open === 'boolean'
          && e.manifest_sha256 === this.target.manifest && e.controller_version === this.target.controllerVersion && text(e.controller_version, 20)
          && integer(e.remaining_ms, 0, 180000) && text(e.url, 256));
        const url = new URL(e.url);
        requireValue(url.protocol === 'http:' && url.hostname === this.target.ip && (!url.port || url.port === '80')
          && !url.username && !url.password && !url.search && !url.hash && url.pathname === '/');
        this.progress = {...this.progress, dashboardUrl: url.href, action: {url: url.href, pairingOpen: e.pairing_open, remainingSeconds: Math.floor(e.remaining_ms / 1000), manifest: e.manifest_sha256 as string, controllerVersion: e.controller_version}};
        break;
      }
      case 'status':
        requireValue(text(e.code, 80) && text(e.message));
        if (e.code === 'installation_workflow') requireValue(this.active === -1 && e.workflow === (this.target.workflow ?? 'install'));
        this.progress = {...this.progress, label: e.message};
        break;
      case 'completed':
        requireValue(e.outcome === 'installed' && this.finished.size === stages.length && !this.progress.prompt);
        this.terminal = 'installed'; this.progress = {...this.progress, state: 'complete', label: 'Installation verified and dashboard confirmed.', action: undefined};
        break;
      case 'stopped':
        requireValue(text(e.message) && text(e.error_type, 100) && e.automatic_retry === false && e.automatic_restore === false);
        this.terminal = 'stopped'; this.progress = {...this.progress, state: 'stopped', label: e.message, prompt: undefined};
        break;
      default: requireValue(false);
    }
    this.lastSequence = e.seq as number;
  }

  answer(id: string, answer: 'yes' | 'no'): string {
    requireValue(!this.terminal && this.progress.prompt?.id === id && ['yes', 'no'].includes(answer));
    this.progress = {...this.progress, prompt: undefined, state: 'waiting', label: 'Observation sent. Waiting for the backend.'};
    return JSON.stringify({v: 1, id, answer}) + '\n';
  }
  cancel(): string | undefined {
    if (this.terminal || this.progress.cancelRequested) return;
    this.progress = {...this.progress, cancelRequested: true};
    return JSON.stringify({v: 1, command: 'cancel'}) + '\n';
  }
}

/** Streaming UTF-8 JSONL, bounded per event. Partial EOF is a failure. */
export class EventLines {
  private buffer = '';
  private decoder = new StringDecoder('utf8');
  constructor(private consume: (value: unknown) => void) {}
  write(chunk: Buffer): void {
    this.buffer += this.decoder.write(chunk);
    let end: number;
    while ((end = this.buffer.indexOf('\n')) >= 0) {
      const line = this.buffer.slice(0, end); this.buffer = this.buffer.slice(end + 1);
      requireValue(Buffer.byteLength(line) <= 65536 && line.length > 0);
      let value: unknown;
      try {value = JSON.parse(line);} catch {throw new Error('The installer backend returned malformed JSON.');}
      this.consume(value);
    }
    requireValue(Buffer.byteLength(this.buffer) <= 65536);
  }
  end(): void {this.buffer += this.decoder.end(); requireValue(this.buffer.length === 0);}
}
