import React, {useEffect, useRef, useState} from 'react';
import {Box, Text, useApp, useInput, usePaste, useWindowSize} from 'ink';
import {resolve} from 'node:path';
import {homedir} from 'node:os';
import {mkdir} from 'node:fs/promises';
import {randomUUID} from 'node:crypto';
import {discoverArtifacts} from './artifacts.js';
import {acquireRestore, createPlan, validatePlan, repository, type Run} from './backend.js';
import type {Execution, Start} from './execution.js';
import {discoverLights, type Light} from './discovery.js';
import {findBundle, type Bundle} from './bundle.js';
import {readInstalled, type ReadInstalled, type InstalledDetails} from './installed.js';
import {ArtifactList, Button, Field, Frame, Heading, LightChoices, Notice, PlanCard, ProgressView, palette} from './components.js';
import {draftErrors, emptyDraft, previewProgress, type Draft, type InstallationMode, type PlanSummary, type Progress} from './model.js';
import {edit} from './input.js';

type Page = 'home' | 'discover' | 'installed' | 'target' | 'files' | 'restore' | 'review' | 'open' | 'ready' | 'preview' | 'confirm' | 'live';
const groups: Partial<Record<Page, {key: keyof Draft; label: string; hint?: string}[]>> = {
  target: [
    {key: 'ip', label: 'Light IP address', hint: 'From your router. One selected light; no network scanning.'},
    {key: 'name', label: 'Exact stock name'},
    {key: 'deviceId', label: 'Native device ID', hint: 'keylight- plus the last six lowercase ESP MAC digits.'},
  ],
  files: [
    {key: 'folder', label: 'Artifact folder', hint: 'Ctrl+R finds local files here. It never contacts a light.'},
    {key: 'identity', label: 'Identity .oklnxp'}, {key: 'off1', label: 'OFF1 .oklnxp'},
    {key: 'low1', label: 'LOW1 .oklnxp'}, {key: 'lighting', label: 'Lighting .oklnxp'},
    {key: 'esp', label: 'ESP application .bin'}, {key: 'assets', label: 'Dashboard asset-manifest.json'},
    {key: 'commit', label: 'Reviewed source commit', hint: 'The complete 40-character commit for these builds.'},
  ],
  restore: [
    {key: 'restore', label: 'Reviewed restore bank', hint: 'Advanced: supply your reviewed complete bank with its actual provenance.'},
    {key: 'restoreVersion', label: 'Restore version'},
    {key: 'provenance', label: 'Restore provenance', hint: 'Record origin, preserved tail, modifications and evidence. Never call a derived image a backup.'},
    {key: 'output', label: 'New plan path', hint: 'Its parent directory must exist. Existing files are never replaced.'},
  ],
};
const next: Partial<Record<Page, Page>> = {target: 'files', files: 'restore', restore: 'review'};
const previous: Partial<Record<Page, Page>> = {installed: 'discover', target: 'home', files: 'target', restore: 'files', review: 'restore', open: 'home', ready: 'home', preview: 'home'};

export function App({run, start, initialPlan = '', initialFolder = '', initialBundle, root = repository, preview = false, discover = discoverLights, bundleLoader = findBundle, nativeReader = readInstalled}: {run: Run; start?: Start; initialPlan?: string; initialFolder?: string; initialBundle?: string; root?: string; preview?: boolean; discover?: typeof discoverLights; bundleLoader?: typeof findBundle; nativeReader?: ReadInstalled}) {
  const {exit} = useApp(), {columns, rows} = useWindowSize();
  const [page, setPage] = useState<Page>(preview ? 'preview' : initialPlan ? 'open' : 'home');
  const [draft, setDraft] = useState<Draft>(() => ({...emptyDraft(), folder: initialFolder, output: initialFolder ? resolve(initialFolder, 'migration.json') : ''}));
  const [selected, setSelected] = useState(0), [cursor, setCursor] = useState(Array.from(initialPlan).length);
  const [plan, setPlan] = useState(initialPlan), [summary, setSummary] = useState<PlanSummary>();
  const [notice, setNotice] = useState(''), [error, setError] = useState(false), [busy, setBusy] = useState(false);
  const operation = useRef<AbortController | undefined>(undefined);
  const execution = useRef<Execution | undefined>(undefined);
  const promptId = useRef<string | undefined>(undefined);
  const [progress, setProgress] = useState<Progress>({stage: 0, state: 'waiting', label: 'Waiting to start'});
  const [live, setLive] = useState(false), [yes, setYes] = useState(false), [exclusive, setExclusive] = useState(false);
  const [mode, setMode] = useState<InstallationMode>('install');
  const [audit, setAudit] = useState(''), [manualRestore, setManualRestore] = useState(false), [details, setDetails] = useState(false);
  const [lights, setLights] = useState<Light[]>([]);
  const [installedLight, setInstalledLight] = useState<Light>(), [installedDetails, setInstalledDetails] = useState<InstalledDetails>();
  useEffect(() => () => {operation.current?.abort(); execution.current?.cancel();}, []);
  const fields = page === 'restore' && !manualRestore ? [{key: 'output' as const, label: 'New plan path', hint: 'The parent folder must exist. Existing files are preserved.'}] : groups[page] ?? [], field = fields[selected];
  const visibleCount = rows < 32 ? 2 : 3, firstField = Math.max(0, Math.min(selected - 1, fields.length - visibleCount));
  const go = (to: Page) => {setPage(to); setSelected(0); setCursor(to === 'open' ? Array.from(plan).length : 0); setNotice(''); setError(false);};
  const select = (value: number) => {
    const count = page === 'home' ? 4 : page === 'discover' ? lights.length + 2 : page === 'installed' ? 2 : fields.length + 1;
    const index = (value + count) % count;
    setSelected(index); setCursor(Array.from(draft[fields[index]?.key ?? 'name']).length);
  };
  const task = async (body: (signal: AbortSignal) => Promise<void>) => {
    if (operation.current) return;
    const controller = new AbortController(); operation.current = controller;
    setBusy(true); setError(false); setNotice('Checking local files…');
    try {await body(controller.signal);} catch (caught) {setNotice(caught instanceof Error ? caught.message : 'Local check failed.'); setError(true);}
    finally {operation.current = undefined; setBusy(false);}
  };
  const scan = () => void task(async signal => {
    const result = await discoverArtifacts(draft.folder);
    signal.throwIfAborted();
    setDraft(d => ({...d, ...Object.fromEntries(Object.entries(result.suggested).filter(([key]) => !d[key as keyof Draft]))}));
    setNotice(`${Object.keys(result.suggested).length} file suggestions found. ${result.ambiguous.length ? 'Multiple matches: choose each path explicitly.' : 'Names are suggestions; full validation follows.'}`);
  });
  const discoverNearby = () => {go('discover'); setLights([]); void task(async signal => {
    setNotice('Listening for Keylight announcements on your local network…');
    const found = await discover({signal}); signal.throwIfAborted(); setLights(found); setSelected(0);
    setNotice(found.length ? '' : 'No lights found. Check power and use the same Wi-Fi network.');
  });};
  const selectLight = (light: Light) => void task(async signal => {
    if (light.installed) {
      setInstalledLight(light); setInstalledDetails(undefined); go('installed'); setNotice('Reading device details. No settings or firmware will change…');
      const details = await nativeReader(light, signal); signal.throwIfAborted(); setInstalledDetails(details); setNotice(''); return;
    }
    const target = {...draft, ip: light.ip, name: light.name, deviceId: light.deviceId};
    setDraft(target); setNotice('Preparing your installation…');
    const selectedBundle = await bundleLoader(initialBundle, initialFolder || root); signal.throwIfAborted();
    await prepareGuided(selectedBundle, target, signal);
  });
  const prepareGuided = async (selectedBundle: Bundle, target: Draft, signal: AbortSignal) => {
    setNotice('Preparing your installation…');
    const restore = await acquireRestore(resolve(homedir(), '.cache/open-keylight/stock-nxp-1.3.0.bin'), run, signal);
    const plans = resolve(homedir(), '.open-keylight/plans'); await mkdir(plans, {recursive: true, mode: 0o700});
    const output = resolve(plans, `${target.deviceId}-${randomUUID()}.json`);
    const complete = {...target, ...selectedBundle.files, commit: selectedBundle.commit, restore: restore.path, restoreVersion: '1.3.0.0', provenance: restore.provenance, output};
    const result = await createPlan(complete, run, signal); setDraft(complete); setSummary(result); setPlan(output);
    setMode('install'); setAudit(`${output}.install-${randomUUID()}.jsonl`); setExclusive(false); setDetails(false); go('confirm');
  };
  const change = (input: string, key = {}) => {
    const value = page === 'open' ? plan : field ? draft[field.key] : undefined;
    if (value === undefined) return;
    const result = edit(value, cursor, input, key);
    if (page === 'open') setPlan(result.value);
    else if (field) setDraft(d => ({...d, [field.key]: result.value}));
    setCursor(result.cursor);
  };
  const downloadRestore = () => void task(async signal => {
    const result = await acquireRestore(resolve(homedir(), '.cache/open-keylight/stock-nxp-1.3.0.bin'), run, signal);
    setDraft(d => ({...d, restore: result.path, restoreVersion: '1.3.0.0', provenance: result.provenance}));
    setNotice(result.cached ? 'Exact verified recovery image reused from your local cache.' : 'Official download verified; complete recovery bank derived and cached. This is not a backup of your light.');
  });
  const install = () => {
    if (!start || !summary || !exclusive || execution.current) return;
    const stopped = (caught: unknown) => setProgress(p => ({...p, state: 'stopped', label: caught instanceof Error ? caught.message : 'Installer stopped.', failureCode: undefined, prompt: undefined, action: undefined, completed: undefined, total: undefined, unit: undefined, remainingSeconds: undefined, cancelRequested: undefined}));
    try {
      setProgress({workflow: mode, stage: 0, state: 'waiting', label: 'Connecting to your light…', finishedStages: []});
      setYes(false); setLive(true); setPage('live');
      const active = start(plan, audit, summary, p => {setProgress(p); if (p.prompt?.id !== promptId.current) setYes(false); promptId.current = p.prompt?.id;}, mode);
      execution.current = active;
      void active.done.catch(stopped)
        .finally(() => {setLive(false); execution.current = undefined;});
    } catch (caught) {setLive(false); stopped(caught);}
  };
  const reviewInstallation = (selectedMode: InstallationMode) => {
    if (!start || !summary || live || execution.current) return;
    setMode(selectedMode); setAudit(`${plan}.${selectedMode}-${randomUUID()}.jsonl`); setExclusive(false); go('confirm');
  };
  usePaste(text => {if (!busy && page !== 'live' && page !== 'confirm') change(text);});
  useInput((input, key) => {
    if (key.ctrl && input === 'c') {if (live) execution.current?.cancel(); else if (busy) operation.current?.abort(); else exit(); return;}
    if (busy) return;
    if (page === 'live') {
      if (!live && key.escape) go('ready');
      if (input === 'd') setDetails(v => !v);
      if (live && progress.state !== 'stopped' && progress.prompt && !progress.cancelRequested) {
        if (key.upArrow || key.downArrow || key.tab) setYes(v => !v);
        else if (key.return) {execution.current?.answer(progress.prompt.id, yes ? 'yes' : 'no'); setYes(false);}
      }
      return;
    }
    if (key.escape) {go(previous[page] ?? 'home'); return;}
    if (page === 'home') {
      if (key.tab || key.downArrow) select(selected + 1);
      else if (key.upArrow) select(selected - 1);
      else if (key.return) {if (selected === 0) discoverNearby(); else go((['target', 'open', 'preview'] as Page[])[selected - 1]!);}
      return;
    }
    if (page === 'discover') {
      if (key.tab || key.downArrow) select(selected + 1); else if (key.upArrow) select(selected - 1);
      else if (key.return) {if (selected === lights.length) discoverNearby(); else if (selected === lights.length + 1) go('target'); else selectLight(lights[selected]!);}
      return;
    }
    if (page === 'installed') {
      if (key.tab || key.downArrow) select(selected + 1); else if (key.upArrow) select(selected - 1);
      else if (key.return) {if (selected === 0) go('discover'); else discoverNearby();}
      return;
    }
    if (page === 'preview') return;
    if (page === 'ready') {if (input === 'd') setDetails(v => !v); else if (input === 'f') reviewInstallation('finish'); else if (key.return) reviewInstallation('install'); return;}
    if (page === 'confirm') {if (input === 'd') setDetails(v => !v); else if (input === ' ') setExclusive(v => !v); else if (key.return && exclusive) install(); return;}
    if (page === 'open') {
      if (key.return) void task(async signal => {const value = await validatePlan(plan, run, signal); setSummary(value); go('ready');});
      else change(input, key);
      return;
    }
    if (page === 'review') {
      if (key.return) void task(async signal => {const value = await createPlan(draft, run, signal); setSummary(value); setPlan(resolve(draft.output)); go('ready');});
      return;
    }
    if (key.ctrl && input === 'r' && page === 'files') {scan(); return;}
    if (key.ctrl && input === 'd' && page === 'restore' && !manualRestore) {downloadRestore(); return;}
    if (key.ctrl && input === 'm' && page === 'restore') {setManualRestore(v => !v); setSelected(0); setCursor(0); return;}
    if (key.tab || key.downArrow) select(selected + 1);
    else if (key.upArrow) select(selected - 1);
    else if (key.return) {
      if (selected < fields.length) select(selected + 1);
      else if (page === 'restore' && !draft.restore) downloadRestore();
      else go(next[page] ?? 'home');
    } else change(input, key);
  });

  if (page === 'preview') return <ProgressView progress={previewProgress} preview width={columns} />;
  if (page === 'live') return <ProgressView progress={progress} width={columns} yes={yes} running={live} auditPath={audit} details={details} targetName={summary?.target_name} />;
  return <Frame width={columns} compact={rows < 30 || columns <= 80}>
    {page === 'home' && <>
      <Heading eyebrow="SETUP" title="Let's set up your light." detail="Firmware, controls and a dashboard. All on your light." />
      <Box flexDirection="column" marginY={1}>
        {['Find my light · guided setup', 'Advanced · choose builds and target', 'Open a prepared plan', 'Preview the installation'].map((label, index) => <Button key={label} label={label} active={selected === index} />)}
      </Box>
    </>}
    {page === 'discover' && <LightChoices lights={lights} selected={selected} />}
    {page === 'installed' && installedLight && <>
      <Heading eyebrow={installedDetails ? 'YOUR EXISTING INSTALLATION' : 'DISCOVERED DASHBOARD'} title="Open your light." detail={installedLight.name} />
      <Text bold color={palette.accent}>{`http://${installedLight.ip}/`}</Text>
      {installedDetails ? <Box flexDirection="column" marginY={1}>
        <Text color={palette.ink}>Open Keylight {installedDetails.firmware}</Text>
        <Text color={palette.muted}>{installedDetails.deviceId} · identity checked just now</Text>
      </Box> : <Text color={palette.muted}>Open Keylight was announced at this address. Version is not verified yet.</Text>}
      <Text color={palette.ink}>{installedDetails ? 'No reinstall needed. Your dashboard has controls and updates.' : 'Verify this light in its dashboard before choosing an update.'}</Text>
      <Text color={palette.muted}>For updates, open System in the dashboard and choose an application image.</Text>
      {installedDetails?.trialPending && <Notice>A firmware trial is pending. Use the dashboard to check your controls and confirm the trial.</Notice>}
      <Button label="Back to discovered lights" active={selected === 0} />
      <Button label="Find lights again" active={selected === 1} />
    </>}
    {fields.length > 0 && <>
      <Heading eyebrow={page === 'target' ? '01 / YOUR LIGHT' : page === 'files' ? '02 / ORIGINAL FIRMWARE' : '03 / RECOVERY + PLAN'}
        title={page === 'target' ? 'One light. An exact match.' : page === 'files' ? 'Bring the reviewed builds together.' : 'Keep a clear route back.'}
        detail={page === 'files' ? 'Ctrl+R scans this local folder. Each path stays editable.' : undefined} />
      {page === 'restore' && !manualRestore && <Box flexDirection="column" marginBottom={1}>
        <Text color={palette.ink}>{draft.restore ? '✓ Verified recovery artifact selected' : 'Get the reviewed stock recovery image automatically'}</Text>
        <Text color={palette.muted}>Razer HTTPS download → pinned hashes → complete recovery bank.</Text>
        <Text color={palette.muted}>A vendor-derived restore image, never a backup of your device.</Text>
        <Text color={palette.accent}>Ctrl+D download / reuse cache · Ctrl+M advanced local artifact</Text>
      </Box>}
      {fields.map((item, index) => ({item, index})).filter(({index}) => fields.length <= 3 || (index >= firstField && index < firstField + visibleCount)).map(({item, index}) => <Field key={item.key} label={`${fields.length > 3 ? `${index + 1}/${fields.length} · ` : ''}${item.label}`} value={draft[item.key]} active={selected === index} cursor={cursor} hint={item.hint} />)}
      <Button label={page === 'restore' ? (!draft.restore ? 'Get the verified recovery image' : 'Review the plan') : 'Continue'} active={selected === fields.length} />
    </>}
    {page === 'open' && <>
      <Heading eyebrow="OPEN A LOCAL PLAN" title="Check every artifact before connecting." detail="The reviewed Python backend validates packages, hashes and embedded assets offline." />
      <Field label="Migration plan .json" value={plan} active cursor={cursor} hint="Paste the path, then Enter. No device requests." />
    </>}
    {page === 'review' && <>
      <Heading eyebrow="04 / REVIEW" title={draft.name || 'Your selected light'} detail={`${draft.ip || 'IP missing'}  ·  ${draft.deviceId || 'Device ID missing'}`} />
      <ArtifactList draft={draft} />
      <Text color={palette.muted}>Restore: {draft.restore || 'Not selected'}</Text>
      <Text color={palette.muted}>Plan: {draft.output || 'Not selected'}</Text>
      {draftErrors(draft).slice(0, 3).map(message => <Notice key={message}>{message}</Notice>)}
      <Button label="Validate files and create this plan" />
      <Text color={palette.muted}>Creates one new local file. Nothing is uploaded.</Text>
    </>}
    {page === 'ready' && summary && <>
      <PlanCard summary={summary} details={details} />
      {details && <Box marginTop={1}><Text color={palette.muted}>Saved plan: {plan}</Text></Box>}
      <Text color={palette.muted}>D {details ? 'hide' : 'show'} technical details</Text>
      <Button label="Review installation and start" active={!!start} />
      {start && details && <Text color={palette.muted}>F · Finish a previous installation</Text>}
      {!start && <Notice>This build has no live backend adapter. The plan is ready; no installation has started.</Notice>}
    </>}
    {page === 'confirm' && summary && <>
      <Heading eyebrow={mode === 'finish' ? 'FINISH YOUR INSTALLATION' : 'READY TO INSTALL'} title={summary.target_name} detail={`${summary.target_ip} · Open Keylight ${summary.esp.version}`} />
      <Text color={palette.ink}>{mode === 'finish' ? 'The installed light engine is retained. Only the dashboard application is replaced.' : 'The light will go dark, then show five gentle colour pulses.'}</Text>
      <Text color={palette.muted}>Keep power connected and close other light controls.</Text>
      <Box marginY={1}><Text color={exclusive ? palette.accent : palette.ink}>{exclusive ? '☑' : '☐'} {mode === 'finish' ? 'Other light controls are closed.' : 'I can watch the light. Other controls are closed.'} <Text dimColor>[Space]</Text></Text></Box>
      <Button label={mode === 'finish' ? 'Finish installation' : 'Install Open Keylight'} active={exclusive} />
      <Box marginTop={1}><Text color={palette.muted}>D · {details ? 'Hide' : 'Show'} technical details</Text></Box>
      {details && <Box flexDirection="column" marginTop={1}><Text color={palette.muted}>{summary.device_id} · {summary.source_commit}</Text><Text color={palette.muted} wrap="truncate-middle">Audit: {audit}</Text><Text color={palette.muted}>Ctrl+C requests a stop after the current write or restart finishes.</Text></Box>}
    </>}
    {notice && <Notice error={error}>{notice}</Notice>}
  </Frame>;
}
