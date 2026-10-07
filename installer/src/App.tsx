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

type Page = 'home' | 'discover' | 'installed' | 'bundle' | 'target' | 'files' | 'restore' | 'review' | 'open' | 'ready' | 'preview' | 'confirm' | 'live';
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
  const [lights, setLights] = useState<Light[]>([]), [bundle, setBundle] = useState<Bundle>();
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
    setNotice(found.length ? 'Discovery is a selection hint. The installer checks the exact device before any write.' : 'No light answered. Discovery can be blocked on guest or isolated networks.');
  });};
  const selectLight = (light: Light) => void task(async signal => {
    if (light.installed) {
      setInstalledLight(light); setInstalledDetails(undefined); go('installed'); setNotice('Reading device details. No settings or firmware will change…');
      const details = await nativeReader(light, signal); signal.throwIfAborted(); setInstalledDetails(details); setNotice(''); return;
    }
    setDraft(d => ({...d, ip: light.ip, name: light.name, deviceId: light.deviceId}));
    const selectedBundle = await bundleLoader(initialBundle, initialFolder || root); signal.throwIfAborted(); setBundle(selectedBundle); go('bundle');
  });
  const prepareGuided = () => void task(async signal => {
    if (!bundle) throw new Error('Select the release bundle first.');
    setNotice('Obtaining the pinned official recovery image…');
    const restore = await acquireRestore(resolve(homedir(), '.cache/open-keylight/stock-nxp-1.3.0.bin'), run, signal);
    const plans = resolve(homedir(), '.open-keylight/plans'); await mkdir(plans, {recursive: true, mode: 0o700});
    const output = resolve(plans, `${draft.deviceId}-${randomUUID()}.json`);
    const complete = {...draft, ...bundle.files, commit: bundle.commit, restore: restore.path, restoreVersion: '1.3.0.0', provenance: restore.provenance, output};
    const result = await createPlan(complete, run, signal); setDraft(complete); setSummary(result); setPlan(output); go('ready');
  });
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
    try {
      setYes(false); setLive(true); setPage('live');
      const active = start(plan, audit, summary, p => {setProgress(p); if (p.prompt?.id !== promptId.current) setYes(false); promptId.current = p.prompt?.id;}, mode);
      execution.current = active;
      void active.done.catch(caught => setProgress(p => ({...p, state: 'stopped', label: caught instanceof Error ? caught.message : 'Installer stopped.'})))
        .finally(() => {setLive(false); execution.current = undefined;});
    } catch (caught) {setLive(false); setProgress(p => ({...p, state: 'stopped', label: caught instanceof Error ? caught.message : 'Could not start installer.'}));}
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
      if (!live && progress.state === 'stopped' && progress.stage === 0 && mode === 'install' && input === 'f') reviewInstallation('finish');
      if (live && progress.prompt && !progress.cancelRequested) {
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
    if (page === 'bundle') {if (key.return) prepareGuided(); return;}
    if (page === 'preview') return;
    if (page === 'ready') {if (input === 'd') setDetails(v => !v); else if (input === 'f') reviewInstallation('finish'); else if (key.return) reviewInstallation('install'); return;}
    if (page === 'confirm') {if (input === ' ') setExclusive(v => !v); else if (key.return && exclusive) install(); return;}
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
  if (page === 'live') return <ProgressView progress={progress} width={columns} yes={yes} running={live} auditPath={audit} canFinish={!live && progress.state === 'stopped' && progress.stage === 0 && mode === 'install'} />;
  return <Frame width={columns} compact={rows < 30 || columns <= 80}>
    {page === 'home' && <>
      <Heading eyebrow="A LIGHT THAT BELONGS TO YOU" title="Beautiful light. Entirely yours." detail="A guided path from stock firmware to your own local dashboard." />
      <Box flexDirection="column" marginY={1}>
        {['Find my light · guided setup', 'Advanced · choose builds and target', 'Open a prepared plan', 'Preview the installation'].map((label, index) => <Button key={label} label={label} active={selected === index} />)}
      </Box>
      <Text color={palette.muted}>We find your light and prepare the right files. You watch two short checks; we handle the installation.</Text>
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
    {page === 'bundle' && bundle && <>
      <Heading eyebrow="02 / YOUR RELEASE" title={`Open Keylight ${bundle.version}`} detail={`Selected for ${draft.name} · ${draft.ip}`} />
      <Text color={palette.accent}>✓ Firmware and dashboard files verified</Text>
      <Text color={palette.muted}>Replaces both controller applications and installs the dashboard on your light.</Text>
      <Box marginY={1} flexDirection="column"><Text color={palette.ink}>Recovery is prepared for you</Text><Text color={palette.muted}>A stock recovery image is downloaded directly from Razer and checked before installation.</Text></Box>
      <Text color={palette.muted}>{bundle.version.includes('-') ? 'Early-access release' : 'Selected release'} · Stay beside the light for its two visual checks.</Text>
      <Button label="Prepare and review this installation" />
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
      {start && <Text color={palette.accent}>F · Finish a previous installation — keep its Open Keylight light engine</Text>}
      {!start && <Notice>This build has no live backend adapter. The plan is ready; no installation has started.</Notice>}
    </>}
    {page === 'confirm' && summary && <>
      <Heading eyebrow={mode === 'finish' ? 'FINISH YOUR INSTALLATION' : 'READY WHEN YOU ARE'} title={`${mode === 'finish' ? 'Finish setup on' : 'Install on'} ${summary.target_name}`} detail={`${summary.target_ip} · ${summary.device_id}`} />
      <Text color={palette.ink}>{mode === 'finish' ? 'Verify the installed Open Keylight light engine, then install the dashboard application. The light engine is retained.' : 'Watch this light for the darkness check and five low colour pulses.'}</Text>
      {mode === 'finish' && <Text color={palette.muted}>Requires a running Open Keylight controller. An unknown controller or recovery loader will stop the check. Setup ends with a brief 5% white check and Off.</Text>}
      <Text color={palette.muted}>Keep power connected. Close other light controls. We stop on a failed check and never retry or restore automatically.</Text>
      <Box marginY={1}><Text color={exclusive ? palette.accent : palette.amber}>{exclusive ? '☑' : '☐'} I have exclusive control and can observe this light. <Text dimColor>[Space]</Text></Text></Box>
      <Text color={palette.muted}>New audit: {audit}</Text>
      <Button label={mode === 'finish' ? 'Verify the light engine and finish setup' : 'Start the guided installation'} active={exclusive} />
      <Notice>Ctrl+C during installation requests a stop after the current safe stage. Do not close the terminal during a write or quiet interval.</Notice>
    </>}
    {notice && <Notice error={error}>{notice}</Notice>}
  </Frame>;
}
